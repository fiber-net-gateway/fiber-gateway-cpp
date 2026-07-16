#ifndef FIBER_NACOS_AUTH_NACOS_AUTHENTICATOR_H
#define FIBER_NACOS_AUTH_NACOS_AUTHENTICATOR_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>

#include <async/Spawn.h>
#include <async/Task.h>
#include <async/Watch.h>
#include <event/EventLoop.h>
#include <fiber/nacos/NacosAuth.h>
#include <fiber/nacos/NacosClientConfig.h>

namespace fiber::nacos::detail {

class AuthHttpOperation;
class NacosClientImpl;
struct AuthHttpSuccess;

class NacosAuthenticator {
public:
    explicit NacosAuthenticator(NacosClientImpl &client);
    ~NacosAuthenticator();

    void start() noexcept;
    void stop() noexcept;
    void publish_stopped();

private:
    class TaskDoneGuard {
    public:
        explicit TaskDoneGuard(NacosClientImpl &client) noexcept : client_(&client) {}
        ~TaskDoneGuard();

    private:
        NacosClientImpl *client_ = nullptr;
    };

    void start_attempt() noexcept;
    void schedule_attempt(std::chrono::steady_clock::time_point when) noexcept;
    void schedule_retry(const NacosAuthError &error);
    void handle_success(AuthHttpSuccess success, std::size_t server_index);
    void publish_failure(const NacosAuthError &error);
    void publish_expired();

    [[nodiscard]] async::DetachedTask run_attempt_tracked() noexcept;
    [[nodiscard]] async::Task<void> run_attempt() noexcept;
    [[nodiscard]] async::Task<std::expected<AuthHttpSuccess, NacosAuthError>>
    request(std::size_t server_index, NacosAuthApiVersion version) noexcept;

    [[nodiscard]] std::string make_target(NacosAuthApiVersion version) const;
    [[nodiscard]] bool token_valid(std::chrono::steady_clock::time_point now) const noexcept;
    [[nodiscard]] bool shutdown_requested();

    static void on_attempt_timer(NacosAuthenticator *authenticator) noexcept;
    static void on_expiry_timer(NacosAuthenticator *authenticator) noexcept;

    NacosClientImpl *client_ = nullptr;
    event::EventLoop::TimerEntry attempt_timer_;
    event::EventLoop::TimerEntry expiry_timer_;
    async::Watch<bool>::Subscriber shutdown_subscriber_;
    std::shared_ptr<AuthHttpOperation> active_operation_;
    std::string auth_body_;
    NacosAuthSnapshot snapshot_;
    std::optional<NacosAuthApiVersion> resolved_auth_api_;
    std::size_t preferred_server_index_ = 0;
    std::chrono::milliseconds retry_delay_{};
    bool attempt_active_ = false;
    bool stopping_ = false;
};

} // namespace fiber::nacos::detail

#endif // FIBER_NACOS_AUTH_NACOS_AUTHENTICATOR_H
