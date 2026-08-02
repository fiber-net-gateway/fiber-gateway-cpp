#include "GrpcClient.h"

#include <utility>

namespace fiber::nacos::detail::grpc {

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

http::Http2ClientConnection::Options make_connection_options(GrpcClient::Options &options) noexcept {
    http::Http2ClientConnection::Options conn_options;
    conn_options.peer_addr = std::move(options.peer_addr);
    conn_options.tcp = options.tcp;
    conn_options.tls = std::move(options.tls);
    conn_options.h2 = std::move(options.h2);
    return conn_options;
}

} // namespace

GrpcClient::GrpcClient(event::EventLoop &loop, Options options) noexcept :
    loop_(&loop), conn_(loop, make_connection_options(options)) {
    assign_view(authority_, options.authority);
    assign_view(scheme_, options.scheme);
}

GrpcClient::~GrpcClient() { FIBER_ASSERT(state_ == State::Created || state_ == State::Stopped); }

fiber::async::Task<common::IoResult<void>> GrpcClient::connect(std::chrono::milliseconds timeout) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ != State::Created) {
        co_return std::unexpected(common::IoErr::Busy);
    }
    auto connect_result = co_await conn_.connect(timeout);
    if (!connect_result) {
        if (conn_.http2().state() != http::Http2Connection::State::Init) {
            state_ = State::Stopped;
        }
        co_return std::unexpected(connect_result.error());
    }

    state_ = State::Connected;
    co_return common::IoResult<void>{};
}

fiber::async::Task<GrpcClient::CloseResult> GrpcClient::wait_closed() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ == State::Created) {
        co_return std::unexpected(common::IoErr::Busy);
    }
    auto result = co_await conn_.wait_closed();
    state_ = State::Stopped;
    co_return result;
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
    state_ = State::Stopping;
    conn_.shutdown();
    (void) co_await conn_.wait_closed();
    state_ = State::Stopped;
}

fiber::async::Task<GrpcClient::CloseResult> GrpcClient::graceful_shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ == State::Created) {
        state_ = State::Stopped;
        co_return CloseResult{};
    }
    if (state_ == State::Stopped) {
        if (conn_.http2().state() == http::Http2Connection::State::Init) {
            co_return CloseResult{};
        }
        co_return co_await conn_.wait_closed();
    }

    const bool initiate = state_ == State::Connected;
    state_ = State::Stopping;
    auto result = initiate ? co_await conn_.graceful_shutdown() : co_await conn_.wait_closed();
    state_ = State::Stopped;
    co_return result;
}

GrpcStream GrpcClient::open_stream(std::string_view service, std::string_view method, mem::BufPool &pool,
                                   GrpcStream::Options options) noexcept(false) {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(state_ == State::Connected);
    return GrpcStream(conn_, authority_, scheme_, service, method, pool, options);
}

} // namespace fiber::nacos::detail::grpc
