#ifndef FIBER_AI_SERVER_AI_SERVER_H
#define FIBER_AI_SERVER_AI_SERVER_H

#include "config/LlmConfigManager.h"
#include "limit/RateLimitShardRing.h"
#include "limit/TokenRateLimitCoordinator.h"
#include "limit/TokenRateLimitRemoteClient.h"
#include "limit/TokenRateLimitService.h"
#include "observability/AiServerMetrics.h"
#include "provider/ProviderConnectionManager.h"
#include "provider/ProviderHttpClient.h"
#include "provider/ProviderRuntime.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "async/Spawn.h"
#include "async/Task.h"
#include "async/WaitGroup.h"
#include "async/Watch.h"
#include "common/IoError.h"
#include "common/NonCopyable.h"
#include "common/NonMovable.h"
#include "event/EventLoop.h"
#include "event/EventLoopGroup.h"
#include "http/Http1Server.h"
#include "log/LogConfig.h"
#include "net/SocketAddress.h"
#include "net/TcpListener.h"

namespace fiber::cat {
class CatClient;
}

namespace fiber::ai_server {

class AiServer final : public common::NonCopyable, public common::NonMovable {
public:
    AiServer(event::EventLoop &accept_loop, event::EventLoopGroup &worker_group, cat::CatClient *cat_client = nullptr,
             std::size_t audit_max_record_bytes = 0, log::AppenderId audit_appender_id = log::kInvalidAppenderId);
    ~AiServer();

    [[nodiscard]] async::Task<bool> start_config_workers(LlmConfigManager &config_manager) noexcept;
    [[nodiscard]] common::IoResult<void> bind(const net::SocketAddress &address, const net::ListenOptions &options);
    async::DetachedTask serve();
    void close();
    [[nodiscard]] async::Task<void> shutdown_and_wait();
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] RateLimitShardRing &rate_limit_ring() noexcept { return rate_limit_ring_; }

private:
    struct WorkerState {
        std::shared_ptr<const LlmConfigSnapshot> config;
        ProviderRuntimeRegistry provider_runtime;
        bool initial_installed = false;
    };

    [[nodiscard]] async::DetachedTask watch_config(std::size_t worker_index,
                                                   LlmConfigManager::SnapshotSubscriber subscription) noexcept;
    [[nodiscard]] async::DetachedTask sweep_rate_limits() noexcept;
    [[nodiscard]] async::DetachedTask detach_cat_worker() noexcept;
    [[nodiscard]] async::Task<void> detach_cat_workers() noexcept;
    [[nodiscard]] async::Task<void> handle(http::HttpExchange &exchange);
    [[nodiscard]] WorkerState &current_worker() noexcept;
    [[nodiscard]] std::shared_ptr<const LlmConfigSnapshot> current_config() const noexcept;

    event::EventLoop *accept_loop_ = nullptr;
    event::EventLoopGroup *worker_group_ = nullptr;
    cat::CatClient *cat_client_ = nullptr;
    std::size_t audit_max_record_bytes_ = 0;
    log::AppenderId audit_appender_id_ = log::kInvalidAppenderId;
    std::vector<WorkerState> workers_;
    async::WaitGroup initial_installs_;
    async::WaitGroup config_tasks_;
    async::WaitGroup sweep_tasks_;
    async::WaitGroup cat_detach_tasks_;
    async::Watch<bool> config_stop_{false};
    std::optional<async::Watch<bool>::Publisher> config_stop_publisher_;
    std::atomic<bool> initial_install_failed_{false};
    bool config_workers_started_ = false;
    bool config_workers_stopping_ = false;
    TokenRateLimitService rate_limiters_;
    RateLimitShardRing rate_limit_ring_;
    TokenRateLimitRemoteClient rate_limit_remote_client_;
    TokenRateLimitCoordinator rate_limit_coordinator_;
    ProviderConnectionManager provider_connections_;
    ProviderHttpClient provider_client_;
    AiServerMetrics metrics_;
    http::Http1Server server_;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_AI_SERVER_H
