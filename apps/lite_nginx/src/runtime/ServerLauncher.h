#ifndef FIBER_LITE_NGINX_RUNTIME_SERVER_LAUNCHER_H
#define FIBER_LITE_NGINX_RUNTIME_SERVER_LAUNCHER_H

#include <expected>
#include <memory>
#include <vector>

#include "event/EventLoop.h"
#include "event/EventLoopGroup.h"
#include "http/HttpServer.h"
#include "net/SocketAddress.h"

#include "RuntimeConfig.h"

namespace fiber::lite_nginx::upstream {
class UpstreamRegistry;
}

namespace fiber::lite_nginx::runtime {

class DnsService;
class HttpScriptServicesImpl;

class ServerLauncher {
public:
    struct BoundListener {
        fiber::net::SocketAddress address;
        bool tls = false;
        bool http3 = false;
    };

    explicit ServerLauncher(fiber::event::EventLoop &accept_loop);
    ~ServerLauncher();

    std::expected<void, RuntimeError> start(const RuntimeConfig &runtime);
    void close();

    [[nodiscard]] const std::vector<BoundListener> &bound_listeners() const noexcept { return bound_listeners_; }

private:
    fiber::event::EventLoop &accept_loop_;
    std::shared_ptr<const RuntimeConfig> runtime_{};
    std::shared_ptr<upstream::UpstreamRegistry> upstreams_{};
    std::unique_ptr<DnsService> dns_{};
    std::unique_ptr<HttpScriptServicesImpl> script_services_{};
    std::unique_ptr<fiber::event::EventLoopGroup> worker_group_;
    std::vector<std::unique_ptr<fiber::http::HttpServer>> servers_;
    std::vector<BoundListener> bound_listeners_;
    bool started_ = false;
};

} // namespace fiber::lite_nginx::runtime

#endif // FIBER_LITE_NGINX_RUNTIME_SERVER_LAUNCHER_H
