#ifndef FIBER_NACOS_CONFIG_SERVICE_H
#define FIBER_NACOS_CONFIG_SERVICE_H

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <async/Task.h>
#include <async/Watch.h>
#include <common/IoError.h>
#include <common/NonCopyable.h>

namespace fiber::nacos {
namespace detail {
class ConfigServiceImpl;
struct ConfigSubscriptionLease;
} // namespace detail

enum class ConfigType : std::uint8_t {
    Json,
    Text,
    Yaml,
    Properties,
    Xml,
    Html,
};

struct ConfigData {
    std::string md5;
    std::string content;
};

enum class ConfigState : std::uint8_t {
    Pending,
    Present,
    NotFound,
    Stopped,
};

struct ConfigSnapshot {
    ConfigState state = ConfigState::Pending;
    ConfigData data;
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

class ConfigSubscription {
public:
    using Subscriber = async::Watch<ConfigSnapshot>::Subscriber;
    using Snapshot = async::Watch<ConfigSnapshot>::Snapshot;
    using NextAwaiter = Subscriber::NextAwaiter;

    ConfigSubscription(const ConfigSubscription &) = delete;
    ConfigSubscription &operator=(const ConfigSubscription &) = delete;
    ConfigSubscription(ConfigSubscription &&other) noexcept;
    ConfigSubscription &operator=(ConfigSubscription &&other) noexcept;
    ~ConfigSubscription();

    [[nodiscard]] Snapshot current() const;
    [[nodiscard]] NextAwaiter next(std::uint64_t received_version) const noexcept;
    void close() noexcept;
    [[nodiscard]] bool valid() const noexcept;

private:
    friend class detail::ConfigServiceImpl;

    ConfigSubscription(std::shared_ptr<detail::ConfigSubscriptionLease> lease, Subscriber subscriber) noexcept;

    std::shared_ptr<detail::ConfigSubscriptionLease> lease_;
    std::optional<Subscriber> subscriber_;
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

    [[nodiscard]] virtual std::expected<ConfigSubscription, ConfigServiceError> subscribe(std::string_view data_id,
                                                                                          std::string_view group) = 0;

protected:
    ConfigService() = default;
};

} // namespace fiber::nacos

#endif // FIBER_NACOS_CONFIG_SERVICE_H
