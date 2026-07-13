#include "GrpcClient.h"

#include <array>
#include <cstdint>
#include <utility>

#include "../http/HttpHeaderHash.h"

namespace fiber::grpc {

int parse_grpc_status(std::string_view s) noexcept {
    int v = 0;
    for (char c: s) {
        if (c < '0' || c > '9') {
            break;
        }
        v = v * 10 + (c - '0');
        if (v > 0x7fffffff) {
            return -1;
        }
    }
    return v;
}

namespace {

void assign_view(std::string &dst, std::string_view src) { dst.assign(src.data(), src.size()); }

// Pre-hashed trailer/header names for per-response gRPC status extraction.
constexpr std::uint64_t kGrpcStatusHash = http::http_header_name_hash("grpc-status");
constexpr std::uint64_t kGrpcMessageHash = http::http_header_name_hash("grpc-message");

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

    // 1. encode + frame the request message
    auto payload = encode(loop_->io_buf_node_pool(), request);
    if (!payload) {
        co_return std::unexpected(payload.error());
    }
    auto framed = frame(std::move(*payload));
    if (!framed) {
        co_return std::unexpected(framed.error());
    }

    // 2. request headers: POST /{service}/{method}
    http::HttpHeaders headers(pool);
    headers.set("content-type", "application/grpc");
    headers.set("te", "trailers");
    headers.set("grpc-encoding", "identity");

    std::string path;
    path.reserve(service.size() + method.size() + 2);
    path.push_back('/');
    path.append(service.data(), service.size());
    path.push_back('/');
    path.append(method.data(), method.size());

    const http::Http2RequestHead head{
            .method = http::HttpMethod::Post,
            .scheme = std::string_view(scheme_),
            .authority = std::string_view(authority_),
            .path = std::string_view(path),
            .headers = &headers,
    };

    // 3. open exchange, send headers, then the framed body (END_STREAM)
    http::ClientHttp2Exchange exchange(*conn_, pool);
    auto send_result = co_await exchange.send_request_header(head, false);
    if (!send_result) {
        co_return std::unexpected(send_result.error());
    }
    framed->mark_complete(); // END_STREAM travels with this write
    auto write_result = co_await exchange.write_body(std::move(*framed));
    if (!write_result) {
        co_return std::unexpected(write_result.error());
    }

    // 4. response headers
    auto header_result = co_await exchange.read_header();
    if (!header_result) {
        co_return std::unexpected(header_result.error());
    }
    const http::Http2ResponseHead *resp = *header_result;
    if (resp->status_code != 200) {
        co_return std::unexpected(common::IoErr::Unknown);
    }

    int grpc_code = 0;
    std::string grpc_message;
    if (auto s = resp->headers.get("grpc-status", kGrpcStatusHash); !s.empty()) {
        grpc_code = parse_grpc_status(s);
    }
    if (auto m = resp->headers.get("grpc-message", kGrpcMessageHash); !m.empty()) {
        assign_view(grpc_message, m);
    }

    // grpc-status may ride the response headers with END_STREAM (no body / no
    // separate trailer block) - e.g. immediate errors.
    if (resp->end_stream) {
        co_return GrpcStatus{grpc_code, std::move(grpc_message)};
    }

    // 5. read + deframe the body (expect exactly one message for unary)
    GrpcFrameReader reader;
    mem::IoBufChain payload_chain;
    bool got_message = false;
    for (;;) {
        auto body_result = co_await exchange.read_body(64 * 1024);
        if (!body_result) {
            co_return std::unexpected(body_result.error());
        }
        // The chain is moved into the frame reader below; capture END_STREAM first.
        const bool stream_end = body_result->complete();
        if (body_result->readable_bytes() > 0) {
            auto append_result = reader.append(std::move(*body_result));
            if (!append_result) {
                co_return std::unexpected(append_result.error());
            }
        }
        for (;;) {
            mem::IoBufChain frame_payload;
            auto extract_result = reader.next_payload(frame_payload);
            if (!extract_result) {
                co_return std::unexpected(extract_result.error());
            }
            if (!*extract_result) {
                break; // need more bytes
            }
            if (got_message) {
                co_return std::unexpected(common::IoErr::Invalid); // unary: exactly one message
            }
            payload_chain = std::move(frame_payload);
            got_message = true;
        }
        if (stream_end) {
            break;
        }
    }

    // 6. trailers (grpc-status / grpc-message normally live here)
    auto trailer_result = co_await exchange.read_header();
    if (!trailer_result) {
        co_return std::unexpected(trailer_result.error());
    }
    const http::Http2ResponseHead *trailer = *trailer_result;
    if (auto s = trailer->headers.get("grpc-status", kGrpcStatusHash); !s.empty()) {
        grpc_code = parse_grpc_status(s);
    }
    if (auto m = trailer->headers.get("grpc-message", kGrpcMessageHash); !m.empty()) {
        assign_view(grpc_message, m);
    }

    if (grpc_code != 0) {
        co_return GrpcStatus{grpc_code, std::move(grpc_message)};
    }
    if (!got_message) {
        co_return std::unexpected(common::IoErr::Invalid); // OK status but no message
    }

    // 7. decode the reply (zero-copy: parses directly off payload_chain)
    auto decode_result = decode(payload_chain, response);
    if (!decode_result) {
        co_return std::unexpected(decode_result.error());
    }

    co_return GrpcStatus{0, std::string{}};
}

} // namespace fiber::grpc
