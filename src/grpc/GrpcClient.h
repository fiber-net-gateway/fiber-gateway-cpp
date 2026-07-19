#ifndef FIBER_GRPC_GRPC_CLIENT_H
#define FIBER_GRPC_GRPC_CLIENT_H

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../async/Task.h"
#include "../async/WaitGroup.h"
#include "../common/Assert.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/BufPool.h"
#include "../event/EventLoop.h"
#include "../http/Http2ClientConnection.h"
#include "../http/Http2Connection.h"
#include "../net/SocketAddress.h"
#include "../net/TcpSocketOptions.h"
#include "../net/TlsOptions.h"
#include "GrpcStream.h"

namespace fiber::grpc {

// gRPC client over a single multiplexed HTTP/2 connection. The caller owns the
// connection run coroutine: connect() establishes the transport, run() drives
// it, and shutdown() requests teardown and waits for run() to finish.
//
class GrpcClient : public common::NonCopyable, public common::NonMovable {
public:
    using RunResult = http::Http2Connection::RunResult;

    struct Options {
        net::SocketAddress peer_addr{};
        net::TcpSocketOptions tcp{.no_delay = net::TcpOptionMode::Enabled};
        net::TlsOptions tls{};
        http::Http2Connection::Options h2{};
        std::string_view authority{};
        std::string_view scheme{"https"};
    };

    GrpcClient(event::EventLoop &loop, Options options) noexcept;
    ~GrpcClient();

    // Establish the TCP+TLS+h2 connection. timeout applies to the TCP connect
    // phase; TLS handshake timeout is configured separately. A successful
    // connect() creates an obligation for the caller to drive run() exactly
    // once. Must be co_awaited on the client's event loop.
    fiber::async::Task<common::IoResult<void>> connect(std::chrono::milliseconds timeout) noexcept;

    // Drive the HTTP/2 connection until it closes. The caller must schedule and
    // retain this coroutine; only one run() invocation is accepted.
    fiber::async::Task<RunResult> run() noexcept;

    // Stop accepting new calls, request connection teardown, and wait until
    // run() has completely returned. Safe to call repeatedly on the loop.
    fiber::async::Task<void> shutdown() noexcept;

    // Open a full-duplex streaming call (server/client/bidi streaming are all
    // usage patterns over the returned GrpcStream). Synchronous: constructs the
    // stream; call open() on it next. The returned stream borrows the connection
    // and must not outlive this client. Must be called on the loop before
    // shutdown begins.
    GrpcStream open_stream(std::string_view service, std::string_view method, mem::BufPool &pool,
                           GrpcStream::Options options = {}) noexcept(false);

    [[nodiscard]] const std::optional<net::SocketAddress> &local_addr() const noexcept { return conn_.local_addr(); }

private:
    enum class State : std::uint8_t {
        Created,
        Connected,
        Running,
        StopPending,
        Stopping,
        Stopped,
    };

    event::EventLoop *loop_;
    http::Http2ClientConnection conn_;
    fiber::async::WaitGroup run_started_wg_;
    fiber::async::WaitGroup run_finished_wg_;
    State state_ = State::Created;
    std::string authority_;
    std::string scheme_;
};

} // namespace fiber::grpc

#endif // FIBER_GRPC_GRPC_CLIENT_H
