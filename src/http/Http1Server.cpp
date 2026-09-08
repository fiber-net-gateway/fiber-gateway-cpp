#include <fiber/http/Http1Server.h>

#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/common/IoError.h>
#include <fiber/http/Http1Connection.h>
#include <fiber/http/Http1ServerOptions.h>
#include <fiber/http/HttpTransport.h>
#include <fiber/net/TcpStream.h>
#include "http/TlsAlpn.h"

namespace fiber::http {

namespace {

// Http1Server still takes the catch-all HttpServerOptions; the connection now
// takes only the fields it reads. Both go away when this class does (P5).
Http1ServerOptions make_http1_options(const HttpServerOptions &options) noexcept {
    return Http1ServerOptions{
            .keep_alive_timeout = options.keep_alive_timeout,
            .header_timeout = options.header_timeout,
            .write_timeout = options.write_timeout,
            .header_init_size = options.header_init_size,
            .header_large_size = options.header_large_size,
            .header_large_num = options.header_large_num,
            .drain_unread_body = options.drain_unread_body,
    };
}

} // namespace

Http1Server::Http1Server(event::EventLoop &loop, HttpHandler handler, HttpServerOptions options,
                         event::EventLoopGroup *worker_group) :
    worker_group_(worker_group), handler_(std::move(handler)), options_(std::move(options)), listener_(loop) {}

fiber::common::IoResult<void> Http1Server::bind(const net::SocketAddress &addr, const net::ListenOptions &options) {
    auto result = listener_.bind(addr, options);
    if (!result) {
        return std::unexpected(result.error());
    }
    return {};
}

fiber::async::DetachedTask Http1Server::serve() {
    auto *accept_loop = event::EventLoop::current_or_null();
    FIBER_ASSERT(accept_loop != nullptr);

    while (listener_.valid() && !shutdown_.load(std::memory_order_acquire)) {
        auto accept_result = co_await listener_.accept();
        if (!accept_result) {
            if (accept_result.error() == common::IoErr::Canceled || accept_result.error() == common::IoErr::BadFd) {
                break;
            }
            continue;
        }
        if (shutdown_.load(std::memory_order_acquire)) {
            continue;
        }

        auto accept = std::move(*accept_result);
        connections_wg_.add();

        fiber::async::spawn(
                select_connection_loop(), [this, accept = std::move(accept)]() mutable -> fiber::async::DetachedTask {
                    struct PendingConnectionGuard {
                        Http1Server *server = nullptr;

                        ~PendingConnectionGuard() { server->on_connection_finished(); }
                    } guard{this};

                    std::unique_ptr<HttpTransport> transport;
                    if (options_.tls.enabled()) {
                        auto tls_result =
                                TlsTransport::create(event::EventLoop::current(), std::move(accept), options_.tcp);
                        if (!tls_result) {
                            co_return;
                        }
                        auto tls_transport = std::move(*tls_result);
                        auto tls_param = make_http1_server_tls_param(options_.tls);
                        auto hs_result = co_await tls_transport->handshake(tls_param, net::kDefaultTlsHandshakeTimeout);
                        if (!hs_result) {
                            tls_transport->close();
                            co_return;
                        }
                        transport = std::move(tls_transport);
                    } else {
                        auto tcp_result =
                                TcpTransport::create(event::EventLoop::current(), std::move(accept), options_.tcp);
                        if (!tcp_result) {
                            co_return;
                        }
                        transport = std::move(*tcp_result);
                    }

                    // shutdown_ is this server's cancellation flag; it lives as
                    // long as the server, which outlives its connections.
                    Http1Connection connection(std::move(transport), handler_, make_http1_options(options_), nullptr,
                                               &shutdown_);
                    co_await connection.run();
                });
    }
    co_return;
}

void Http1Server::close() { listener_.close(); }

fiber::async::Task<void> Http1Server::shutdown_and_wait() {
    shutdown_.store(true, std::memory_order_release);
    listener_.close();
    co_await connections_wg_.join();
}

int Http1Server::fd() const noexcept { return listener_.fd(); }

event::EventLoop &Http1Server::select_connection_loop() noexcept {
    if (!worker_group_ || worker_group_->size() == 0) {
        return event::EventLoop::current();
    }
    std::size_t index = next_loop_index_.fetch_add(1, std::memory_order_relaxed);
    return worker_group_->at(index % worker_group_->size());
}

void Http1Server::on_connection_finished() noexcept { connections_wg_.done(); }

} // namespace fiber::http
