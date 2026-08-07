#ifndef FIBER_ACCESS_SERVER_ACCESS_SERVER_H
#define FIBER_ACCESS_SERVER_ACCESS_SERVER_H

#include "../execution/AccessRequestHandler.h"
#include "../execution/ProxyExecutor.h"
#include "../observability/AccessServerMetrics.h"
#include "AccessDnsService.h"
#include "RouteConfigStore.h"

#include <cstddef>

#include <async/Task.h>
#include <async/WaitGroup.h>
#include <common/IoError.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <event/EventLoop.h>
#include <event/EventLoopGroup.h>
#include <http/Http1Server.h>
#include <http/StealableHttp1ConnectionPoolSet.h>
#include <net/SocketAddress.h>
#include <net/TcpListener.h>

namespace fiber::cat {
class CatClient;
}

namespace fiber::access_server {

struct AccessServerOptions {
    std::size_t default_max_request_body_size = 400U << 20U;
    AccessRequestScriptAdapter script_adapter;
    ProxyExecutorOptions executor;
    cat::CatClient *cat_client = nullptr;
    bool test_mode = false;
};

class AccessServer final : public common::NonCopyable, public common::NonMovable {
public:
    AccessServer(event::EventLoop &accept_loop, event::EventLoopGroup &workers, const RouteConfigStore &config_store,
                 ProxyClusterMatcher cluster_matcher, AccessServerOptions options = {});
    ~AccessServer();

    [[nodiscard]] common::IoResult<void> initialize() noexcept;
    [[nodiscard]] common::IoResult<void> bind(const net::SocketAddress &address,
                                              const net::ListenOptions &options = {});
    [[nodiscard]] common::IoResult<void> bind_metrics(const net::SocketAddress &address,
                                                      const net::ListenOptions &options = {});
    async::DetachedTask serve();
    async::DetachedTask serve_metrics();
    [[nodiscard]] async::Task<void> shutdown_and_wait() noexcept;
    [[nodiscard]] int fd() const noexcept { return server_.fd(); }
    [[nodiscard]] int metrics_fd() const noexcept { return metrics_server_.fd(); }

private:
    [[nodiscard]] async::Task<void> handle(http::HttpExchange &exchange) noexcept;
    [[nodiscard]] async::Task<void> handle_metrics(http::HttpExchange &exchange) noexcept;
    [[nodiscard]] async::DetachedTask detach_cat_worker() noexcept;
    [[nodiscard]] async::Task<void> detach_cat_workers() noexcept;

    event::EventLoop *accept_loop_ = nullptr;
    event::EventLoopGroup *workers_ = nullptr;
    AccessDnsService dns_;
    http::StealableHttp1ConnectionPoolSet pool_;
    ProxyExecutor executor_;
    AccessRequestHandler handler_;
    AccessServerMetrics metrics_;
    cat::CatClient *cat_client_ = nullptr;
    http::Http1Server server_;
    http::Http1Server metrics_server_;
    async::WaitGroup cat_detach_tasks_;
    bool initialized_ = false;
    bool metrics_bound_ = false;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_SERVER_H
