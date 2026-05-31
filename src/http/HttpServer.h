#ifndef FIBER_HTTP_HTTP_SERVER_H
#define FIBER_HTTP_HTTP_SERVER_H

#include <atomic>
#include <memory>

#include "../async/Spawn.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "../event/EventLoopGroup.h"
#include "../net/TcpListener.h"
#include "Http2Connection.h"
#include "Http2HpackEncodeCatalog.h"
#include "HttpExchange.h"
#include "HttpTransport.h"
#include "ServerRequestFactory.h"
#include "../net/TlsContext.h"

namespace fiber::http {

class HttpServer : public common::NonCopyable, public common::NonMovable {
public:
    HttpServer(event::EventLoop &loop, HttpHandler handler, HttpServerOptions options = {},
               event::EventLoopGroup *worker_group = nullptr);

    fiber::common::IoResult<void> bind(const net::SocketAddress &addr, const net::ListenOptions &options);
    fiber::async::DetachedTask serve();
    void close();
    [[nodiscard]] int fd() const noexcept;

private:
    [[nodiscard]] event::EventLoop &select_connection_loop() noexcept;
    fiber::async::DetachedTask handle_connection(net::AcceptResult accept);
    fiber::async::Task<void> serve_http1(std::unique_ptr<HttpTransport> transport);
    fiber::async::Task<void> serve_http2(std::unique_ptr<HttpTransport> transport);
    [[nodiscard]] Http2Connection::Options make_http2_options() const noexcept;

    event::EventLoopGroup *worker_group_ = nullptr;
    HttpHandler handler_;
    HttpServerOptions options_;
    Http2HpackEncodeCatalog http2_hpack_encode_catalog_;
    ServerRequestFactory http2_request_factory_;
    net::TcpListener listener_;
    std::unique_ptr<net::TlsServerContext> tls_ctx_;
    std::atomic<std::size_t> next_loop_index_{0};
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP_SERVER_H
