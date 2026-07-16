#ifndef FIBER_NACOS_DETAIL_NACOS_CLIENT_IMPL_H
#define FIBER_NACOS_DETAIL_NACOS_CLIENT_IMPL_H

#include <cstdint>
#include <memory>
#include <optional>

#include <async/Task.h>
#include <async/WaitGroup.h>
#include <async/Watch.h>
#include <common/IoError.h>
#include <event/EventLoop.h>
#include <fiber/nacos/NacosAuth.h>
#include <fiber/nacos/NacosClient.h>
#include <fiber/nacos/NacosClientConfig.h>

namespace fiber::nacos::detail {

class NacosAuthenticator;

enum class NacosClientState : std::uint8_t {
    Created,
    Running,
    Stopping,
    Stopped,
};

class NacosClientImpl {
public:
    NacosClientImpl(event::EventLoop &loop, NacosClientConfig config, NacosClientOptions options);
    ~NacosClientImpl();

    [[nodiscard]] common::IoResult<void> start() noexcept;
    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] async::Watch<NacosAuthSnapshot>::Subscriber subscribe_auth();
    [[nodiscard]] async::Watch<bool>::Subscriber subscribe_shutdown();

    [[nodiscard]] event::EventLoop &loop() const noexcept { return *loop_; }
    [[nodiscard]] const NacosClientConfig &config() const noexcept { return config_; }
    [[nodiscard]] const NacosClientOptions &options() const noexcept { return options_; }
    [[nodiscard]] bool running() const noexcept { return state_ == NacosClientState::Running; }

    [[nodiscard]] bool try_begin_task() noexcept;
    void end_task() noexcept;
    void publish_auth(NacosAuthSnapshot snapshot);

private:
    event::EventLoop *loop_ = nullptr;
    NacosClientConfig config_;
    NacosClientOptions options_;
    NacosClientState state_ = NacosClientState::Created;
    async::WaitGroup task_group_;
    async::Watch<bool> shutdown_watch_{false};
    std::optional<async::Watch<bool>::Publisher> shutdown_publisher_;
    async::Watch<NacosAuthSnapshot> auth_watch_;
    std::optional<async::Watch<NacosAuthSnapshot>::Publisher> auth_publisher_;
    std::unique_ptr<NacosAuthenticator> authenticator_;
};

} // namespace fiber::nacos::detail

#endif // FIBER_NACOS_DETAIL_NACOS_CLIENT_IMPL_H
