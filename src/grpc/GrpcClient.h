#ifndef FIBER_GRPC_GRPC_CLIENT_H
#define FIBER_GRPC_GRPC_CLIENT_H

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

#include <google/protobuf/message_lite.h>

#include "../async/Spawn.h"
#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/BufPool.h"
#include "../event/EventLoop.h"
#include "../http/ClientHttp2Exchange.h"
#include "../http/Http2ClientConnection.h"
#include "../http/Http2Connection.h"
#include "../http/HttpCommon.h"
#include "../net/SocketAddress.h"
#include "../net/TlsOptions.h"
#include "GrpcFraming.h"
#include "GrpcStatus.h"
#include "GrpcStream.h"
#include "ProtoCodec.h"

namespace fiber::grpc {

// Unary gRPC client over a single HTTP/2 connection (multiplexed: many
// unary_call()s may share one connection). The caller owns the HPACK encode
// catalog referenced by Options::h2.outbound_hpack_catalog and must keep it
// alive for the lifetime of the client.
class GrpcClient : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        net::SocketAddress peer_addr{};
        net::TlsOptions tls{};
        http::Http2Connection::Options h2{};
        std::string_view authority{};
        std::string_view scheme{"https"};
    };

    GrpcClient(event::EventLoop &loop, Options options) noexcept;
    ~GrpcClient();

    // Establish the TCP+TLS+h2 connection and start the background receive
    // loop. Must be co_awaited on the client's event loop.
    fiber::async::Task<common::IoResult<void>> connect() noexcept;

    // Initiate connection teardown. The background run task completes
    // asynchronously; poll run_done() (on the loop) before stopping it.
    void shutdown() noexcept;

    // Unary RPC: POST /{service}/{method}. On transport success returns
    // GrpcStatus (check .ok()); on transport/protocol failure returns IoErr.
    // On OK, `response` is filled from the decoded reply message.
    fiber::async::Task<common::IoResult<GrpcStatus>> unary_call(std::string_view service, std::string_view method,
                                                                const google::protobuf::MessageLite &request,
                                                                google::protobuf::MessageLite &response,
                                                                mem::BufPool &pool) noexcept;

    // Open a full-duplex streaming call (server/client/bidi streaming are all
    // usage patterns over the returned GrpcStream). Synchronous: constructs the
    // stream; call open() on it next. The returned stream shares ownership of the
    // connection, so it may outlive the GrpcClient. Must be called on the loop.
    GrpcStream open_stream(std::string_view service, std::string_view method, mem::BufPool &pool,
                           GrpcStream::Options options = {}) noexcept(false);

    [[nodiscard]] bool run_done() const noexcept;

private:
    struct RunState {
        std::atomic<bool> done{false};
        std::atomic<common::IoErr> err{common::IoErr::None};
    };

    // Separate coroutine (params are copied into its frame) so the spawn lambda
    // can be destroyed after invoking it without dangling the captured handles.
    static fiber::async::DetachedTask run_loop(std::shared_ptr<http::Http2ClientConnection> conn,
                                               std::shared_ptr<RunState> state) noexcept;

    event::EventLoop *loop_;
    std::shared_ptr<http::Http2ClientConnection> conn_;
    std::shared_ptr<RunState> run_state_;
    std::string authority_;
    std::string scheme_;
};

} // namespace fiber::grpc

#endif // FIBER_GRPC_GRPC_CLIENT_H
