#ifndef FIBER_NACOS_CONFIG_SERVICE_H
#define FIBER_NACOS_CONFIG_SERVICE_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <fiber/async/Task.h>
#include <fiber/async/Watch.h>
#include <fiber/common/IoError.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>

#include "NacosCreateError.h"
#include "NacosRpcOptions.h"
#include "NacosServiceStatus.h"
#include "Subscription.h"

namespace fiber::nacos {

class NacosClient;

struct ConfigServiceOptions {
    NacosRpcOptions rpc;
    std::chrono::milliseconds subscription_redo_interval{180000};
    std::size_t max_content_bytes = 10 * 1024 * 1024;
    std::size_t max_data_id_bytes = 1024;
    std::size_t max_group_bytes = 1024;
    std::size_t max_listen_contexts_per_request = 100;
};

enum class ConfigType : std::uint8_t {
    Json,
    Text,
    Yaml,
    Properties,
    Xml,
    Html,
};

enum class ConfigState : std::uint8_t {
    Present,
    NotFound,
};

// Immutable configuration view owned by the shared_ptr returned from get_config()
// or carried by SubscriptionResult. Copy that shared_ptr to retain the md5 and
// content views. state is Present for a real config and NotFound for a
// confirmed-absent config; shutdown is delivered with ResultKind::Closed.
struct ConfigData {
    ConfigData() noexcept = default;
    ConfigData(const ConfigData &) = delete;
    ConfigData &operator=(const ConfigData &) = delete;
    ConfigData(ConfigData &&) = delete;
    ConfigData &operator=(ConfigData &&) = delete;

    ConfigState state = ConfigState::Present;
    std::string_view md5;
    std::string_view content;
};

enum class ConfigServiceErrorCode : std::uint8_t {
    InvalidArgument,
    Shutdown,
    AuthenticationUnavailable,
    Transport,
    GrpcStatus,
    Protocol,
    Server,
    ContentTooLarge,
};

struct ConfigServiceError {
    ConfigServiceErrorCode code = ConfigServiceErrorCode::Protocol;
    common::IoErr io_error = common::IoErr::None;
    int grpc_status = 0;
    std::int32_t result_code = 0;
    std::int32_t error_code = 0;
    std::string message;
};

class ConfigService : public common::NonCopyable, public common::NonMovable {
public:
    using StatusSubscriber = async::Watch<ConfigServiceStatus>::Subscriber;

    [[nodiscard]] static std::expected<std::unique_ptr<ConfigService>, NacosCreateError>
    create(NacosClient &client, ConfigServiceOptions options = {});

    virtual ~ConfigService() = default;

    [[nodiscard]] virtual common::IoResult<void> start() noexcept = 0;
    [[nodiscard]] virtual async::Task<void> shutdown() noexcept = 0;

    // Status is published on the client EventLoop. The bounded snapshot owns
    // no implementation pointers, identifiers, addresses, or error text.
    [[nodiscard]] virtual StatusSubscriber subscribe_status() = 0;

    // A successful query always returns a non-null snapshot. A missing config
    // is represented by ConfigState::NotFound, matching subscription results.
    [[nodiscard]] virtual async::Task<std::expected<std::shared_ptr<const ConfigData>, ConfigServiceError>>
    get_config(std::string data_id, std::string group) noexcept = 0;

    [[nodiscard]] virtual async::Task<std::expected<void, ConfigServiceError>>
    publish(std::string data_id, std::string group, std::string content, ConfigType type,
            std::optional<std::string> cas_md5 = std::nullopt) noexcept = 0;

    [[nodiscard]] virtual async::Task<std::expected<void, ConfigServiceError>>
    remove_config(std::string data_id, std::string group) noexcept = 0;

    // Subscribe to (data_id, group). Notifications and cached-value replay run
    // synchronously on the client EventLoop; cached replay may run before this
    // function returns. Returns Shutdown when the service is stopping. The
    // move-only handle must be closed/destroyed on that loop.
    [[nodiscard]] virtual std::expected<Subscription<ConfigData>, ConfigServiceError>
    subscribe(std::string_view data_id, std::string_view group, Subscription<ConfigData>::NotifyCallback on_notify,
              void *ctx) = 0;

protected:
    ConfigService() = default;
};

} // namespace fiber::nacos

#endif // FIBER_NACOS_CONFIG_SERVICE_H
