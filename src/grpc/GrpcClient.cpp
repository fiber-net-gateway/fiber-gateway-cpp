#include "GrpcClient.h"

#include <array>
#include <cstdint>
#include <new>
#include <utility>

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

} // namespace

GrpcClient::GrpcClient(event::EventLoop &loop, Options options) noexcept : loop_(&loop) {
    FIBER_ASSERT(grpc_catalog_.init(kGrpcHeaderPolicy));
    http::Http2ClientConnection::Options conn_options;
    conn_options.peer_addr = options.peer_addr;
    conn_options.tls = std::move(options.tls);
    conn_options.h2 = options.h2;
    if (conn_options.h2.outbound_hpack_catalog == nullptr) {
        conn_options.h2.outbound_hpack_catalog = &grpc_catalog_;
    }
    conn_ = std::make_shared<http::Http2ClientConnection>(loop, std::move(conn_options));
    assign_view(authority_, options.authority);
    assign_view(scheme_, options.scheme);
}

GrpcClient::~GrpcClient() {
    if (conn_) {
        conn_->shutdown(); // best-effort; callers should shutdown() on-loop first
    }
}

fiber::async::Task<common::IoResult<void>> GrpcClient::connect() noexcept {
    if (!conn_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    auto connect_result = co_await conn_->connect();
    if (!connect_result) {
        co_return std::unexpected(connect_result.error());
    }

    run_state_ = std::make_shared<RunState>();
    fiber::async::spawn(*loop_, [conn = conn_, state = run_state_]() { return run_loop(conn, state); });
    co_return common::IoResult<void>{};
}

fiber::async::DetachedTask GrpcClient::run_loop(std::shared_ptr<http::Http2ClientConnection> conn,
                                                std::shared_ptr<RunState> state) noexcept {
    auto run_result = co_await conn->run();
    state->err.store(run_result ? common::IoErr::None : run_result.error(), std::memory_order_release);
    state->done.store(true, std::memory_order_release);
    co_return;
}

void GrpcClient::shutdown() noexcept {
    if (conn_) {
        conn_->shutdown();
    }
}

bool GrpcClient::run_done() const noexcept { return run_state_ && run_state_->done.load(std::memory_order_acquire); }

GrpcStream GrpcClient::open_stream(std::string_view service, std::string_view method, mem::BufPool &pool,
                                   GrpcStream::Options options) noexcept(false) {
    return GrpcStream(conn_, authority_, scheme_, service, method, pool, options);
}

fiber::async::Task<common::IoResult<GrpcStatus>>
GrpcClient::unary_call(std::string_view service, std::string_view method, const google::protobuf::MessageLite &request,
                       google::protobuf::MessageLite &response, mem::BufPool &pool) noexcept {
    if (!conn_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    // Unary is just a constrained streaming call: open, send exactly one request
    // (half-close), read exactly one response, then finish for the gRPC status.
    // Delegating to GrpcStream avoids a second copy of the framing/read/trailer
    // logic and keeps the two paths from diverging.
    GrpcStream stream;
    try {
        stream = open_stream(service, method, pool, {});
    } catch (const std::bad_alloc &) {
        // open_stream (GrpcStream ctor) allocates the authority/path strings; a
        // bad_alloc there is reported as IoErr::NoMem rather than terminating
        // this noexcept coroutine. Matches ScriptCompiler's bad_alloc handling.
        co_return std::unexpected(common::IoErr::NoMem);
    }

    if (auto r = co_await stream.open(); !r) {
        co_return std::unexpected(r.error());
    }
    if (auto r = co_await stream.write(request); !r) {
        co_return std::unexpected(r.error());
    }
    if (auto r = co_await stream.writes_done(); !r) {
        co_return std::unexpected(r.error());
    }

    // A trailers-only response (immediate error) carries no message and yields End.
    const auto first = co_await stream.read(response);
    if (!first) {
        co_return std::unexpected(first.error());
    }
    // Unary permits exactly one response message; a second is a protocol violation.
    if (*first == GrpcReadOutcome::Message) {
        const auto second = co_await stream.read(response);
        if (!second) {
            co_return std::unexpected(second.error());
        }
        if (*second == GrpcReadOutcome::Message) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
    }

    auto finish_result = co_await stream.finish();
    if (!finish_result) {
        co_return std::unexpected(finish_result.error());
    }
    if (!finish_result->ok()) {
        co_return *finish_result; // gRPC-level error; do not trust the response
    }
    if (*first != GrpcReadOutcome::Message) {
        co_return std::unexpected(common::IoErr::Invalid); // OK status but no message
    }
    co_return *finish_result;
}

} // namespace fiber::grpc
