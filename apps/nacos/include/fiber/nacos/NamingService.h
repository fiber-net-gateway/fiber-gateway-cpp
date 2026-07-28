#ifndef FIBER_NACOS_NAMING_SERVICE_H
#define FIBER_NACOS_NAMING_SERVICE_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <async/Task.h>
#include <async/Watch.h>
#include <common/IoError.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>

#include "NacosCreateError.h"
#include "NacosRpcOptions.h"
#include "Subscription.h"

namespace fiber::nacos {

class NacosClient;

struct NamingServiceOptions {
    NacosRpcOptions rpc;
    std::size_t max_service_name_bytes = 1024;
    std::size_t max_group_bytes = 1024;
    std::size_t max_hosts_per_service = 4096;
    std::size_t max_metadata_entries = 256;
    std::size_t max_metadata_key_bytes = 1024;
    std::size_t max_metadata_value_bytes = 4096;
};

struct NamingMetadataEntry {
    std::string key;
    std::string value;
};

struct Instance {
    std::string instance_id;
    std::string ip;
    std::uint16_t port = 0;
    double weight = 1.0;
    bool healthy = true;
    bool enabled = true;
    bool ephemeral = true;
    std::string cluster_name = "DEFAULT";
    std::string service_name;
    std::vector<NamingMetadataEntry> metadata;
};

struct ServiceInfo {
    std::string name;
    std::string group_name;
    std::string clusters;
    std::int64_t cache_millis = 1000;
    std::vector<Instance> hosts;
    std::int64_t last_ref_time = 0;
    std::string checksum;
    bool all_ips = false;
    bool reach_protection_threshold = false;
};

enum class NamingServiceErrorCode : std::uint8_t {
    InvalidArgument,
    Shutdown,
    AuthenticationUnavailable,
    Transport,
    GrpcStatus,
    Protocol,
    Server,
    ResponseTooLarge,
};

struct NamingServiceError {
    NamingServiceErrorCode code = NamingServiceErrorCode::Protocol;
    common::IoErr io_error = common::IoErr::None;
    int grpc_status = 0;
    std::int32_t result_code = 0;
    std::int32_t error_code = 0;
    std::string message;
};

enum class RegistrationState : std::uint8_t {
    Pending,
    Registered,
    Failed,
    Closed,
};

struct RegistrationStatus {
    RegistrationState state = RegistrationState::Pending;
    std::shared_ptr<const NamingServiceError> error;
};

class InstanceRegistration {
public:
    using StatusSubscriber = async::Watch<RegistrationStatus>::Subscriber;

    InstanceRegistration() noexcept = default;
    InstanceRegistration(const InstanceRegistration &) = delete;
    InstanceRegistration &operator=(const InstanceRegistration &) = delete;
    InstanceRegistration(InstanceRegistration &&other) noexcept;
    InstanceRegistration &operator=(InstanceRegistration &&other) noexcept;
    ~InstanceRegistration();

    [[nodiscard]] std::expected<void, NamingServiceError> update(Instance instance) noexcept;
    [[nodiscard]] StatusSubscriber subscribe_status() const;
    void close() noexcept;
    [[nodiscard]] explicit operator bool() const noexcept { return context_ != nullptr; }

    using UpdateFn = std::expected<void, NamingServiceError> (*)(void *, Instance) noexcept;
    using SubscribeFn = StatusSubscriber (*)(void *);
    using CloseFn = void (*)(void *) noexcept;

    InstanceRegistration(std::shared_ptr<void> owner, void *context, UpdateFn update, SubscribeFn subscribe,
                         CloseFn close) noexcept;

private:
    std::shared_ptr<void> owner_;
    void *context_ = nullptr;
    UpdateFn update_ = nullptr;
    SubscribeFn subscribe_ = nullptr;
    CloseFn close_ = nullptr;
};

class NamingService : public common::NonCopyable, public common::NonMovable {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<NamingService>, NacosCreateError>
    create(NacosClient &client, NamingServiceOptions options = {});

    virtual ~NamingService() = default;

    [[nodiscard]] virtual common::IoResult<void> start() noexcept = 0;
    [[nodiscard]] virtual async::Task<void> shutdown() noexcept = 0;

    [[nodiscard]] virtual async::Task<std::expected<std::shared_ptr<const ServiceInfo>, NamingServiceError>>
    get(std::string service_name, std::string group) noexcept = 0;

    [[nodiscard]] virtual std::expected<Subscription<ServiceInfo>, NamingServiceError>
    subscribe(std::string_view service_name, std::string_view group) = 0;

    [[nodiscard]] virtual std::expected<InstanceRegistration, NamingServiceError>
    registry(std::string_view service_name, std::string_view group, Instance instance) = 0;

protected:
    NamingService() = default;
};

} // namespace fiber::nacos

#endif // FIBER_NACOS_NAMING_SERVICE_H
