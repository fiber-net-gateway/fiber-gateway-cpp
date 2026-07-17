#include "GrpcClient.h"

#include <array>
#include <utility>

#include "../common/Assert.h"
#include "../http/HttpHeaderHash.h"

namespace fiber::grpc {

int parse_grpc_status(std::string_view s) noexcept {
    // An empty value is treated as OK (0) - matches a missing grpc-status.
    if (s.empty()) {
        return 0;
    }
    // Accumulate in unsigned so the multiply/add never triggers signed-overflow
    // UB. Reject any non-digit character (the whole value must be decimal) and
    // any value exceeding INT_MAX, both as -1 per the documented contract.
    unsigned v = 0;
    for (char c: s) {
        if (c < '0' || c > '9') {
            return -1;
        }
        const unsigned digit = static_cast<unsigned>(c - '0');
        if (v > (0x7fffffffu - digit) / 10u) {
            return -1;
        }
        v = v * 10u + digit;
    }
    return static_cast<int>(v);
}

namespace {

void assign_view(std::string &dst, std::string_view src) { dst.assign(src.data(), src.size()); }

// Common gRPC request headers that are identical on every call: index them in
// the HPACK dynamic table so repeated calls on one connection encode them as
// 1-byte indexed fields. All string_views point at string literals (static
// storage), so the catalog holds no references to GrpcClient members.
constexpr std::array<http::Http2HpackEncodeCatalog::PolicyEntry, 3> kGrpcHeaderPolicy{{
        {"content-type", http::http_header_name_hash("content-type"), "application/grpc"},
        {"te", http::http_header_name_hash("te"), "trailers"},
        {"grpc-encoding", http::http_header_name_hash("grpc-encoding"), "identity"},
}};

const http::Http2HpackEncodeCatalog &grpc_header_catalog() noexcept {
    static http::Http2HpackEncodeCatalog catalog;
    static const bool initialized = catalog.init(kGrpcHeaderPolicy);
    FIBER_ASSERT(initialized);
    return catalog;
}

http::Http2ClientConnection::Options make_connection_options(GrpcClient::Options &options) noexcept {
    http::Http2ClientConnection::Options conn_options;
    conn_options.peer_addr = std::move(options.peer_addr);
    conn_options.tls = std::move(options.tls);
    conn_options.h2 = std::move(options.h2);
    if (conn_options.h2.outbound_hpack_catalog == nullptr) {
        conn_options.h2.outbound_hpack_catalog = &grpc_header_catalog();
    }
    return conn_options;
}

} // namespace

GrpcClient::GrpcClient(event::EventLoop &loop, Options options) noexcept :
    loop_(&loop), conn_(loop, make_connection_options(options)) {
    assign_view(authority_, options.authority);
    assign_view(scheme_, options.scheme);
}

GrpcClient::~GrpcClient() {
    FIBER_ASSERT(state_ == State::Created || state_ == State::Stopped);
    FIBER_ASSERT(run_started_wg_.empty());
    FIBER_ASSERT(run_finished_wg_.empty());
}

fiber::async::Task<common::IoResult<void>> GrpcClient::connect() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ != State::Created) {
        co_return std::unexpected(common::IoErr::Busy);
    }
    auto connect_result = co_await conn_.connect();
    if (!connect_result) {
        if (conn_.http2().state() != http::Http2Connection::State::Init) {
            state_ = State::Stopped;
        }
        co_return std::unexpected(connect_result.error());
    }

    state_ = State::Connected;
    run_started_wg_.add();
    run_finished_wg_.add();
    co_return common::IoResult<void>{};
}

fiber::async::Task<GrpcClient::RunResult> GrpcClient::run() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ != State::Connected && state_ != State::StopPending) {
        co_return std::unexpected(common::IoErr::Busy);
    }

    state_ = state_ == State::StopPending ? State::Stopping : State::Running;
    run_started_wg_.done();

    auto run_result = co_await conn_.run();
    state_ = State::Stopped;
    run_finished_wg_.done();
    co_return run_result;
}

fiber::async::Task<void> GrpcClient::shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ == State::Created) {
        state_ = State::Stopped;
        co_return;
    }
    if (state_ == State::Stopped) {
        co_return;
    }
    if (state_ == State::Connected) {
        state_ = State::StopPending;
    }
    if (state_ == State::StopPending) {
        co_await run_started_wg_.join();
    }
    if (state_ == State::Stopped) {
        co_return;
    }

    FIBER_ASSERT(state_ == State::Running || state_ == State::Stopping);
    state_ = State::Stopping;
    conn_.shutdown();
    co_await run_finished_wg_.join();
    FIBER_ASSERT(state_ == State::Stopped);
}

GrpcStream GrpcClient::open_stream(std::string_view service, std::string_view method, mem::BufPool &pool,
                                   GrpcStream::Options options) noexcept(false) {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(state_ == State::Connected || state_ == State::Running);
    return GrpcStream(conn_, authority_, scheme_, service, method, pool, options);
}

} // namespace fiber::grpc
