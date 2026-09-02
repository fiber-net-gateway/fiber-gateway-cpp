#ifndef FIBER_HTTP_HTTP_SERVER_H
#define FIBER_HTTP_HTTP_SERVER_H

#include <atomic>
#include <cstdint>
#include <memory>

#include "../async/Spawn.h"
#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "../event/EventLoopGroup.h"
#include "../net/TcpListener.h"
#include "../net/TlsParams.h"
#include "Http2Connection.h"
#include "Http3Server.h"
#include "HttpExchange.h"
#include "HttpTransport.h"
#include "ServerRequestFactory.h"

namespace fiber::http {

class HttpServer : public common::NonCopyable, public common::NonMovable {
public:
    enum class State : std::uint8_t {
        Created,
        Bound,
        Running,
        Closing,
        Closed,
    };

    HttpServer(event::EventLoop &loop, HttpHandler handler, HttpServerOptions options = {},
               event::EventLoopGroup *worker_group = nullptr);
    ~HttpServer();

    fiber::common::IoResult<void> bind(const net::SocketAddress &addr, const net::ListenOptions &options);
    fiber::async::DetachedTask serve();
    // Non-blocking and safe to call from any thread. The actual loop-affine
    // close operation is posted to the owner loop when necessary.
    void close() noexcept;
    void request_close() noexcept;
    // Completes after the accept loop and all protocol connections have
    // stopped. The returned task must be awaited before destroying the server
    // when a caller needs a synchronous lifetime boundary.
    fiber::async::Task<void> shutdown_and_wait();
    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] bool shutting_down() const noexcept;
    [[nodiscard]] int fd() const noexcept;

private:
    struct Runtime;

    static fiber::async::DetachedTask noop_task();
    static fiber::async::DetachedTask serve_loop(std::shared_ptr<Runtime> runtime);
    [[nodiscard]] static event::EventLoop &select_connection_loop(const std::shared_ptr<Runtime> &runtime) noexcept;
    static fiber::async::DetachedTask handle_connection(std::shared_ptr<Runtime> runtime, net::AcceptResult accept);
    static fiber::async::Task<void> serve_http1(std::shared_ptr<Runtime> runtime,
                                                std::unique_ptr<HttpTransport> transport);
    static fiber::async::Task<void> serve_http2(std::shared_ptr<Runtime> runtime,
                                                std::unique_ptr<HttpTransport> transport);
    [[nodiscard]] static Http2Connection::Options make_http2_options(const HttpServerOptions &http_options) noexcept;
    static void close_on_owner_loop(const std::shared_ptr<Runtime> &runtime) noexcept;
    static fiber::async::DetachedTask finish_shutdown(std::shared_ptr<Runtime> runtime);

    std::shared_ptr<Runtime> runtime_;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP_SERVER_H
