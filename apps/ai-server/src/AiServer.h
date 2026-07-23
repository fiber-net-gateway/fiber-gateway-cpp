#ifndef FIBER_AI_SERVER_AI_SERVER_H
#define FIBER_AI_SERVER_AI_SERVER_H

#include "config/LlmConfigManager.h"

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
#include "net/SocketAddress.h"
#include "net/TcpListener.h"

namespace fiber::ai_server {

class AiServer final : public common::NonCopyable, public common::NonMovable {
public:
    AiServer(event::EventLoop &accept_loop, event::EventLoopGroup &worker_group);
    ~AiServer();

    [[nodiscard]] async::Task<bool> start_config_workers(LlmConfigManager &config_manager) noexcept;
    [[nodiscard]] common::IoResult<void> bind(const net::SocketAddress &address, const net::ListenOptions &options);
    async::DetachedTask serve();
    void close();
    [[nodiscard]] async::Task<void> shutdown_and_wait();
    [[nodiscard]] int fd() const noexcept;

private:
    struct WorkerState {
        std::shared_ptr<const LlmServingSnapshot> config;
        bool initial_installed = false;
    };

    [[nodiscard]] async::DetachedTask watch_config(std::size_t worker_index,
                                                   LlmConfigManager::ServingSubscriber subscription) noexcept;
    [[nodiscard]] async::Task<void> handle(http::HttpExchange &exchange);
    [[nodiscard]] std::shared_ptr<const LlmServingSnapshot> current_config() const noexcept;

    event::EventLoop *accept_loop_ = nullptr;
    event::EventLoopGroup *worker_group_ = nullptr;
    std::vector<WorkerState> workers_;
    async::WaitGroup initial_installs_;
    async::WaitGroup config_tasks_;
    async::Watch<bool> config_stop_{false};
    std::optional<async::Watch<bool>::Publisher> config_stop_publisher_;
    std::atomic<bool> initial_install_failed_{false};
    bool config_workers_started_ = false;
    bool config_workers_stopping_ = false;
    http::Http1Server server_;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_AI_SERVER_H
