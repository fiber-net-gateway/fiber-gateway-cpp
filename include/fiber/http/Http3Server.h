#ifndef FIBER_HTTP_HTTP3_SERVER_H
#define FIBER_HTTP_HTTP3_SERVER_H

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

#include "../async/Task.h"
#include "../async/WaitGroup.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "../event/EventLoopGroup.h"
#include "../net/SocketAddress.h"
#include "../net/TlsParams.h"
#include "../quic/QuicUdpEndpoint.h"
#include "Http3Connection.h"
#include "HttpExchange.h"

namespace fiber::http {

class Http3Server : public common::NonCopyable, public common::NonMovable {
public:
    Http3Server(event::EventLoop &loop, HttpHandler handler, HttpServerOptions options,
                event::EventLoopGroup *worker_group = nullptr);
    ~Http3Server();

    [[nodiscard]] common::IoResult<void> bind(const net::SocketAddress &addr) noexcept;
    void serve() noexcept;
    // Non-blocking shutdown request; initiate it on the server's owner loop.
    // Await shutdown_and_wait() before destroying an initialized server.
    void close() noexcept;
    fiber::async::Task<void> shutdown_and_wait() noexcept;
    [[nodiscard]] bool shutting_down() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const net::SocketAddress &local_addr() const noexcept;

private:
    struct Runtime;

    struct Shard {
        Http3Server *server = nullptr;
        event::EventLoop *loop = nullptr;
        quic::QuicUdpEndpoint endpoint{};
        event::EventLoop::NotifyEntry start_entry{};
        event::EventLoop::NotifyEntry close_entry{};
        bool endpoint_started = false;
        bool close_posted = false;
        bool close_completed = false;
    };

    class ServerConnection;

    [[nodiscard]] static quic::QuicConnection::Lease
    create_connection_op(void *owner, const quic::QuicConnection::Options &options) noexcept;
    [[nodiscard]] quic::QuicConnection::Lease create_connection(const quic::QuicConnection::Options &options) noexcept;

    [[nodiscard]] quic::QuicUdpEndpoint::EndpointOptions make_endpoint_options(const net::SocketAddress &addr,
                                                                               bool reuse_port) noexcept;
    [[nodiscard]] quic::QuicUdpEndpoint::ServerAdmissionOptions make_server_admission_options() noexcept;
    [[nodiscard]] std::size_t shard_count() const noexcept;
    static void on_start_shard(Shard *shard) noexcept;
    static void on_close_shard(Shard *shard) noexcept;

    event::EventLoop &loop_;
    event::EventLoopGroup *worker_group_ = nullptr;
    std::shared_ptr<Runtime> runtime_;
    HttpServerOptions options_;
    std::vector<std::unique_ptr<Shard>> shards_{};
    net::SocketAddress local_addr_{};
    std::atomic<bool> started_{false};
    bool initialized_ = false;
    fiber::async::WaitGroup close_wg_{};
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP3_SERVER_H
