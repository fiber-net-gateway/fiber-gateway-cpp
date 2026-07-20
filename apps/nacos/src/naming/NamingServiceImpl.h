#ifndef FIBER_NACOS_NAMING_NAMING_SERVICE_IMPL_H
#define FIBER_NACOS_NAMING_NAMING_SERVICE_IMPL_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <async/Spawn.h>
#include <async/WaitGroup.h>
#include <async/Watch.h>
#include <event/EventLoop.h>
#include <fiber/nacos/NacosAuthAccess.h>
#include <fiber/nacos/NacosClientConfig.h>
#include <fiber/nacos/NamingService.h>
#include "../dto/JsonCodec.h"

#include "../SubscriptionPool.h"
#include "../rpc/NacosBiRequestHandler.h"
#include "../rpc/NacosRpc.h"

namespace fiber::nacos::detail {

struct NamingProtocolState {
    bool registered = false;
    bool operation_in_flight = false;
    bool draining = false;
};

using NamingEntry = SubscriptionEntry<ServiceInfo, NamingProtocolState>;
using NamingEntryPtr = EntryPtr<NamingEntry>;
using NamingResult = SubscriptionResult<ServiceInfo>;

class NamingServiceImpl final : public NamingService {
    struct RegistrationEntry {
        RegistrationEntry(NamingServiceImpl &service, std::string service_name, std::string group, Instance instance);

        NamingServiceImpl *owner = nullptr;
        std::string service_name;
        std::string group;
        Instance instance;
        async::Watch<RegistrationStatus> status_watch{RegistrationStatus{}};
        std::optional<async::Watch<RegistrationStatus>::Publisher> status_publisher;
        std::uint64_t desired_version = 1;
        std::uint64_t completed_version = 0;
        bool operation_in_flight = false;
        bool registered = false;
        bool closing = false;
    };

public:
    using AuthWatch = async::Watch<NacosAuthAccess>;
    using ReadySubscriber = async::Watch<bool>::Subscriber;

    NamingServiceImpl(event::EventLoop &loop, const NacosClientConfig &config, const NacosClientOptions &options,
                      AuthWatch &auth_watch);
    ~NamingServiceImpl() override;

    [[nodiscard]] static bool valid_options(const NacosClientOptions &options) noexcept;

    [[nodiscard]] async::Task<std::expected<std::shared_ptr<const ServiceInfo>, NamingServiceError>>
    get(std::string service_name, std::string group) noexcept override;
    [[nodiscard]] std::expected<Subscription<ServiceInfo>, NamingServiceError>
    subscribe(std::string_view service_name, std::string_view group) override;
    [[nodiscard]] std::expected<InstanceRegistration, NamingServiceError>
    registry(std::string_view service_name, std::string_view group, Instance instance) override;

    [[nodiscard]] async::Task<void> run() noexcept;
    void shutdown() noexcept;
    [[nodiscard]] ReadySubscriber subscribe_connection_ready();

private:
    using EntryPtr = NamingEntryPtr;

    struct AttemptResult {
        NacosRpcCloseResult close;
        bool reached_ready = false;
    };

    void on_subscription_add(EntryPtr entry);
    [[nodiscard]] RemoveDecision on_subscription_remove(EntryPtr entry);
    void schedule_subscription(EntryPtr entry, bool subscribe);
    [[nodiscard]] async::DetachedTask run_subscription(EntryPtr entry, bool subscribe) noexcept;
    void register_all_subscriptions();

    [[nodiscard]] static async::Task<common::IoResult<dto::resp::NotifySubscriberResponse>>
    handle_notify(void *context, NacosServerRequestContext &,
                  const dto::req::NotifySubscriberRequest &request) noexcept;

    [[nodiscard]] static std::expected<void, NamingServiceError>
    update_registration_callback(void *context, Instance instance) noexcept;
    [[nodiscard]] static InstanceRegistration::StatusSubscriber subscribe_registration_callback(void *context);
    static void close_registration_callback(void *context) noexcept;
    [[nodiscard]] std::expected<void, NamingServiceError> update_registration(RegistrationEntry &entry,
                                                                              Instance instance) noexcept;
    void close_registration(RegistrationEntry &entry) noexcept;
    void schedule_registration(const std::shared_ptr<RegistrationEntry> &entry);
    [[nodiscard]] async::DetachedTask run_registration(std::shared_ptr<RegistrationEntry> entry, Instance instance,
                                                       std::uint64_t version, bool deregister) noexcept;
    void register_all_instances();
    void erase_registration(RegistrationEntry &entry) noexcept;
    void publish_registration(RegistrationEntry &entry, RegistrationState state,
                              std::optional<NamingServiceError> error = std::nullopt);

    [[nodiscard]] NamingServiceError validate_key(std::string_view service_name, std::string_view group) const;
    [[nodiscard]] NamingServiceError validate_instance(const Instance &instance) const;
    [[nodiscard]] bool valid_key(std::string_view service_name, std::string_view group) const noexcept;
    [[nodiscard]] bool valid_instance(const Instance &instance) const noexcept;
    [[nodiscard]] NamingServiceError map_error(NacosRpcError error) const;
    [[nodiscard]] NamingServiceError response_error(const dto::ResponseBase &response) const;
    [[nodiscard]] std::expected<ServiceInfo, NamingServiceError>
    own_service_info(const dto::NamingServiceInfo &value) const;
    void publish_value(NamingEntry &entry, ServiceInfo value);

    void set_rpc_ready(bool ready);
    void reset_connection_state();
    [[nodiscard]] async::Task<void> run_connection() noexcept;
    [[nodiscard]] async::Task<AttemptResult> run_attempt(NacosRpcEndpoint endpoint) noexcept;
    [[nodiscard]] async::Task<void> wait_backoff(std::chrono::milliseconds delay) noexcept;
    [[nodiscard]] std::chrono::milliseconds jittered(std::chrono::milliseconds delay) noexcept;
    void task_done() noexcept;

    template<typename Request, typename Response>
    [[nodiscard]] async::Task<std::expected<void, NacosRpcError>>
    request_rpc(const Request &request, mem::BufPool &pool, Response &response) noexcept {
        FIBER_ASSERT(loop_->in_loop());
        if (stopping_) {
            co_return std::unexpected(shutdown_rpc_error());
        }
        if (!rpc_ready_ || !rpc_) {
            co_return std::unexpected(not_connected_rpc_error());
        }
        auto result = co_await rpc_->request(request, pool, response);
        if (!result && result.error().code == NacosRpcErrorCode::Shutdown && !stopping_) {
            co_return std::unexpected(not_connected_rpc_error());
        }
        co_return std::move(result);
    }

    [[nodiscard]] static NacosRpcError shutdown_rpc_error();
    [[nodiscard]] static NacosRpcError not_connected_rpc_error();

    event::EventLoop *loop_ = nullptr;
    const NacosClientConfig *config_ = nullptr;
    const NacosClientOptions *options_ = nullptr;
    AuthWatch *auth_watch_ = nullptr;
    NacosBiRequestHandler handlers_;
    std::optional<NacosRpc> rpc_;
    SubscriptionPool<NamingEntry> pool_;
    std::vector<std::shared_ptr<RegistrationEntry>> registrations_;
    async::WaitGroup tasks_;
    async::Watch<bool> ready_watch_{false};
    std::optional<async::Watch<bool>::Publisher> ready_publisher_;
    async::Watch<bool> wake_watch_{false};
    std::optional<async::Watch<bool>::Publisher> wake_publisher_;
    std::optional<NacosRpcEndpoint> redirect_;
    std::size_t preferred_server_index_ = 0;
    std::uint64_t random_state_ = 0x243f6a8885a308d3ull;
    bool rpc_ready_ = false;
    bool run_active_ = false;
    bool stopping_ = false;
};

} // namespace fiber::nacos::detail

#endif // FIBER_NACOS_NAMING_NAMING_SERVICE_IMPL_H
