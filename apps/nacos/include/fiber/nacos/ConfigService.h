#ifndef FIBER_NACOS_CONFIG_SERVICE_H
#define FIBER_NACOS_CONFIG_SERVICE_H

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <async/Task.h>
#include <common/IoError.h>
#include <common/NonCopyable.h>

#include "Subscription.h"

namespace fiber::nacos {

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

// Published configuration value. state is Present for a real config and NotFound
// for a confirmed-absent config; there is no Pending/Stopped - "never synced" is
// expressed by Subscription::current() returning a null snapshot, and shutdown
// by Subscription::next() returning ResultKind::Closed.
struct ConfigData {
    ConfigState state = ConfigState::Present;
    std::string md5;
    std::string content;
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

class ConfigService : public common::NonCopyable {
public:
    virtual ~ConfigService() = default;

    [[nodiscard]] virtual async::Task<std::expected<std::optional<ConfigData>, ConfigServiceError>>
    get_config(std::string data_id, std::string group) noexcept = 0;

    [[nodiscard]] virtual async::Task<std::expected<void, ConfigServiceError>>
    publish(std::string data_id, std::string group, std::string content, ConfigType type,
            std::optional<std::string> cas_md5 = std::nullopt) noexcept = 0;

    [[nodiscard]] virtual async::Task<std::expected<void, ConfigServiceError>>
    remove_config(std::string data_id, std::string group) noexcept = 0;

    // Subscribe to (data_id, group). Synchronous. Returns Shutdown error when
    // the service is stopping. The returned Subscription is move-only and must
    // be closed/destroyed on the client EventLoop.
    [[nodiscard]] virtual std::expected<Subscription<ConfigData>, ConfigServiceError>
    subscribe(std::string_view data_id, std::string_view group) = 0;

protected:
    ConfigService() = default;
};

} // namespace fiber::nacos

#endif // FIBER_NACOS_CONFIG_SERVICE_H
