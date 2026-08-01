#include "NamingServiceImpl.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <async/Sleep.h>
#include <async/Spawn.h>
#include <async/TaskSelect.h>
#include <async/WaitGroup.h>
#include <async/Watch.h>
#include <async/WhenAny.h>
#include <common/Assert.h>
#include <event/EventLoop.h>
#include <fiber/nacos/NacosAuthAccess.h>
#include <fiber/nacos/NacosClient.h>
#include <fiber/nacos/NacosClientConfig.h>

#include "../SubscriptionPool.h"
#include "../dto/JsonCodec.h"
#include "../rpc/NacosBiRequestHandler.h"
#include "../rpc/NacosRpc.h"

namespace fiber::nacos::detail {
namespace {

constexpr std::string_view kRegisterInstance = "registerInstance";
constexpr std::string_view kDeregisterInstance = "deregisterInstance";

NamingServiceError invalid_argument(std::string message) {
    return NamingServiceError{
            .code = NamingServiceErrorCode::InvalidArgument,
            .io_error = common::IoErr::Invalid,
            .message = std::move(message),
    };
}

NamingServiceError shutdown_error() {
    return NamingServiceError{
            .code = NamingServiceErrorCode::Shutdown,
            .io_error = common::IoErr::Canceled,
    };
}

std::string_view nullable_text(const json::Nullable<std::string_view> &value) noexcept {
    return value.is_present() ? value.value() : std::string_view{};
}

} // namespace

struct NamingProtocolState {
    bool registered = false;
    bool operation_in_flight = false;
    bool draining = false;
};

using NamingSubscriptionPool = SubscriptionPool<ServiceInfo, NamingProtocolState>;
using NamingEntry = NamingSubscriptionPool::Entry;
using NamingEntryPtr = NamingSubscriptionPool::EntryPtr;
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
    NamingServiceImpl(NacosServiceDependencies dependencies, NamingServiceOptions options);
    ~NamingServiceImpl() override;

    [[nodiscard]] static bool valid_options(const NamingServiceOptions &options) noexcept;

    [[nodiscard]] common::IoResult<void> start() noexcept override;
    [[nodiscard]] async::Task<void> shutdown() noexcept override;

    [[nodiscard]] async::Task<std::expected<std::shared_ptr<const ServiceInfo>, NamingServiceError>>
    get(std::string service_name, std::string group) noexcept override;
    [[nodiscard]] std::expected<Subscription<ServiceInfo>, NamingServiceError>
    subscribe(std::string_view service_name, std::string_view group,
              Subscription<ServiceInfo>::NotifyCallback on_notify, void *ctx) override;
    [[nodiscard]] std::expected<InstanceRegistration, NamingServiceError>
    registry(std::string_view service_name, std::string_view group, Instance instance) override;

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

    void restore_connection_state();
    void reset_connection_state();
    void request_shutdown() noexcept;
    [[nodiscard]] async::DetachedTask run() noexcept;
    [[nodiscard]] async::Task<void> run_connection() noexcept;
    [[nodiscard]] async::Task<AttemptResult> run_attempt(NacosRpcEndpoint endpoint,
                                                         const NacosBiRequestHandler &handlers) noexcept;
    [[nodiscard]] std::chrono::milliseconds jittered(std::chrono::milliseconds delay) noexcept;

    template<typename Request, typename Response>
    [[nodiscard]] async::Task<std::expected<void, NacosRpcError>>
    request_rpc(const Request &request, mem::BufPool &pool, Response &response) noexcept {
        FIBER_ASSERT(loop_->in_loop());
        if (stopping()) {
            co_return std::unexpected(shutdown_rpc_error());
        }
        NacosRpc *rpc = ready_rpc_;
        if (!rpc) {
            co_return std::unexpected(not_connected_rpc_error());
        }
        auto result = co_await rpc->request(request, pool, response);
        if (!result && result.error().code == NacosRpcErrorCode::Shutdown && !stopping()) {
            co_return std::unexpected(not_connected_rpc_error());
        }
        co_return std::move(result);
    }

    [[nodiscard]] static NacosRpcError shutdown_rpc_error();
    [[nodiscard]] static NacosRpcError not_connected_rpc_error();
    [[nodiscard]] NacosServiceState state() const {
        auto snapshot = lifecycle_.current();
        FIBER_ASSERT(snapshot.value != nullptr);
        return *snapshot.value;
    }
    [[nodiscard]] bool running() const { return state() == NacosServiceState::Running; }
    [[nodiscard]] bool stopping() const {
        const NacosServiceState current = state();
        return current == NacosServiceState::Stopping || current == NacosServiceState::Stopped;
    }

    event::EventLoop *loop_ = nullptr;
    std::shared_ptr<const NacosClientConfig> config_;
    NamingServiceOptions options_;
    NacosAuthSubscriber auth_;
    NamingSubscriptionPool pool_;
    std::vector<std::shared_ptr<RegistrationEntry>> registrations_;
    async::WaitGroup tasks_;
    async::Watch<NacosServiceState> lifecycle_{NacosServiceState::Created};
    std::optional<async::Watch<NacosServiceState>::Publisher> lifecycle_publisher_;
    NacosRpc *ready_rpc_ = nullptr;
    std::size_t preferred_server_index_ = 0;
    std::uint64_t random_state_ = 0x243f6a8885a308d3ull;
};

NamingServiceImpl::RegistrationEntry::RegistrationEntry(NamingServiceImpl &service, std::string service_name,
                                                        std::string group, Instance instance) :
    owner(&service), service_name(std::move(service_name)), group(std::move(group)), instance(std::move(instance)) {
    status_publisher = status_watch.acquire_publisher();
    FIBER_ASSERT(status_publisher.has_value());
}

NamingServiceImpl::NamingServiceImpl(NacosServiceDependencies dependencies, NamingServiceOptions options) :
    loop_(dependencies.loop), config_(std::move(dependencies.config)), options_(std::move(options)),
    auth_(std::move(dependencies.auth)),
    pool_([this](EntryPtr entry) { on_subscription_add(std::move(entry)); },
          [this](EntryPtr entry) { return on_subscription_remove(std::move(entry)); }) {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(config_ != nullptr);
    FIBER_ASSERT(valid_options(options_));
    lifecycle_publisher_ = lifecycle_.acquire_publisher();
    FIBER_ASSERT(lifecycle_publisher_.has_value());
}

NamingServiceImpl::~NamingServiceImpl() {
    FIBER_ASSERT(state() == NacosServiceState::Created || state() == NacosServiceState::Stopped);
    FIBER_ASSERT(ready_rpc_ == nullptr);
    FIBER_ASSERT(tasks_.empty());
    FIBER_ASSERT(registrations_.empty());
}

common::IoResult<void> NamingServiceImpl::start() noexcept {
    if (!loop_->in_loop()) {
        return std::unexpected(common::IoErr::NotSupported);
    }
    if (state() != NacosServiceState::Created) {
        return std::unexpected(common::IoErr::Already);
    }
    lifecycle_publisher_->publish(NacosServiceState::Running);
    async::spawn([this]() { return run(); });
    return {};
}

bool NamingServiceImpl::valid_options(const NamingServiceOptions &options) noexcept {
    return NacosRpc::valid_options(options.rpc) && options.max_service_name_bytes > 0 && options.max_group_bytes > 0 &&
           options.max_hosts_per_service > 0 && options.max_metadata_entries > 0 &&
           options.max_metadata_key_bytes > 0 && options.max_metadata_value_bytes > 0;
}

NacosRpcError NamingServiceImpl::shutdown_rpc_error() {
    return NacosRpcError{
            .code = NacosRpcErrorCode::Shutdown,
            .io_error = common::IoErr::Canceled,
    };
}

NacosRpcError NamingServiceImpl::not_connected_rpc_error() {
    return NacosRpcError{
            .code = NacosRpcErrorCode::Transport,
            .io_error = common::IoErr::NotConnected,
            .message = "Nacos naming RPC is not connected",
    };
}

bool NamingServiceImpl::valid_key(std::string_view service_name, std::string_view group) const noexcept {
    return !service_name.empty() && service_name.size() <= options_.max_service_name_bytes && !group.empty() &&
           group.size() <= options_.max_group_bytes;
}

NamingServiceError NamingServiceImpl::validate_key(std::string_view service_name, std::string_view group) const {
    if (service_name.empty()) {
        return invalid_argument("Nacos service name is empty");
    }
    if (service_name.size() > options_.max_service_name_bytes) {
        return invalid_argument("Nacos service name exceeds configured limit");
    }
    if (group.empty()) {
        return invalid_argument("Nacos naming group is empty");
    }
    return invalid_argument("Nacos naming group exceeds configured limit");
}

bool NamingServiceImpl::valid_instance(const Instance &instance) const noexcept {
    if (instance.ip.empty() || instance.port == 0 || !std::isfinite(instance.weight) || instance.weight < 0.0 ||
        instance.metadata.size() > options_.max_metadata_entries) {
        return false;
    }
    for (const NamingMetadataEntry &entry: instance.metadata) {
        if (entry.key.empty() || entry.key.size() > options_.max_metadata_key_bytes ||
            entry.value.size() > options_.max_metadata_value_bytes) {
            return false;
        }
    }
    return true;
}

NamingServiceError NamingServiceImpl::validate_instance(const Instance &instance) const {
    if (instance.ip.empty()) {
        return invalid_argument("Nacos instance IP is empty");
    }
    if (instance.port == 0) {
        return invalid_argument("Nacos instance port is zero");
    }
    if (!std::isfinite(instance.weight) || instance.weight < 0.0) {
        return invalid_argument("Nacos instance weight is invalid");
    }
    if (instance.metadata.size() > options_.max_metadata_entries) {
        return invalid_argument("Nacos instance metadata exceeds configured entry limit");
    }
    for (const NamingMetadataEntry &entry: instance.metadata) {
        if (entry.key.empty()) {
            return invalid_argument("Nacos instance metadata key is empty");
        }
        if (entry.key.size() > options_.max_metadata_key_bytes ||
            entry.value.size() > options_.max_metadata_value_bytes) {
            return invalid_argument("Nacos instance metadata exceeds configured size limit");
        }
    }
    return invalid_argument("invalid Nacos instance");
}

NamingServiceError NamingServiceImpl::map_error(NacosRpcError error) const {
    NamingServiceError result{
            .io_error = error.io_error,
            .grpc_status = error.grpc_status,
            .result_code = error.result_code,
            .error_code = error.error_code,
            .message = std::move(error.message),
    };
    switch (error.code) {
        case NacosRpcErrorCode::InvalidState:
        case NacosRpcErrorCode::Transport:
            result.code = NamingServiceErrorCode::Transport;
            break;
        case NacosRpcErrorCode::AuthenticationUnavailable:
            result.code = NamingServiceErrorCode::AuthenticationUnavailable;
            break;
        case NacosRpcErrorCode::GrpcStatus:
            result.code = NamingServiceErrorCode::GrpcStatus;
            break;
        case NacosRpcErrorCode::Protocol:
            result.code = error.io_error == common::IoErr::MessageTooLarge ? NamingServiceErrorCode::ResponseTooLarge
                                                                           : NamingServiceErrorCode::Protocol;
            break;
        case NacosRpcErrorCode::Server:
            result.code = NamingServiceErrorCode::Server;
            break;
        case NacosRpcErrorCode::Shutdown:
            result.code = NamingServiceErrorCode::Shutdown;
            break;
    }
    return result;
}

NamingServiceError NamingServiceImpl::response_error(const dto::ResponseBase &response) const {
    NamingServiceError error{
            .code = NamingServiceErrorCode::Server,
            .result_code = response.result_code,
            .error_code = response.error_code,
    };
    if (response.message.is_present()) {
        error.message.assign(response.message.value().substr(0, 512));
    }
    return error;
}

std::expected<ServiceInfo, NamingServiceError>
NamingServiceImpl::own_service_info(const dto::NamingServiceInfo &value) const {
    if (!value.name.is_present() || !value.group_name.is_present() ||
        !valid_key(value.name.value(), value.group_name.value())) {
        return std::unexpected(NamingServiceError{
                .code = NamingServiceErrorCode::Protocol,
                .io_error = common::IoErr::Invalid,
                .message = "Nacos service info has an invalid service key",
        });
    }
    if (value.hosts.size() > options_.max_hosts_per_service) {
        return std::unexpected(NamingServiceError{
                .code = NamingServiceErrorCode::ResponseTooLarge,
                .io_error = common::IoErr::MessageTooLarge,
                .message = "Nacos service info exceeds configured host limit",
        });
    }

    ServiceInfo result{
            .name = std::string(value.name.value()),
            .group_name = std::string(value.group_name.value()),
            .clusters = std::string(nullable_text(value.clusters)),
            .cache_millis = value.cache_millis,
            .last_ref_time = value.last_ref_time,
            .checksum = std::string(nullable_text(value.checksum)),
            .all_ips = value.all_ips,
            .reach_protection_threshold = value.reach_protection_threshold,
    };
    result.hosts.reserve(value.hosts.size());
    for (const dto::NamingInstance &wire: value.hosts) {
        if (!wire.ip.is_present() || wire.ip.value().empty() || wire.port <= 0 || wire.port > 65535 ||
            !std::isfinite(wire.weight) || wire.weight < 0.0 ||
            (wire.metadata.is_present() && wire.metadata.value().size() > options_.max_metadata_entries)) {
            return std::unexpected(NamingServiceError{
                    .code = NamingServiceErrorCode::Protocol,
                    .io_error = common::IoErr::Invalid,
                    .message = "Nacos service info contains an invalid instance",
            });
        }
        Instance host{
                .instance_id = std::string(nullable_text(wire.instance_id)),
                .ip = std::string(wire.ip.value()),
                .port = static_cast<std::uint16_t>(wire.port),
                .weight = wire.weight,
                .healthy = wire.healthy,
                .enabled = wire.enabled,
                .ephemeral = wire.ephemeral,
                .cluster_name = std::string(nullable_text(wire.cluster_name)),
                .service_name = std::string(nullable_text(wire.service_name)),
        };
        if (wire.metadata.is_present()) {
            host.metadata.reserve(wire.metadata.value().size());
            for (const auto &metadata: wire.metadata.value()) {
                if (metadata.key.empty() || metadata.key.size() > options_.max_metadata_key_bytes ||
                    metadata.value.size() > options_.max_metadata_value_bytes) {
                    return std::unexpected(NamingServiceError{
                            .code = NamingServiceErrorCode::ResponseTooLarge,
                            .io_error = common::IoErr::MessageTooLarge,
                            .message = "Nacos instance metadata exceeds configured limit",
                    });
                }
                host.metadata.push_back({std::string(metadata.key), std::string(metadata.value)});
            }
        }
        result.hosts.push_back(std::move(host));
    }
    return result;
}

async::Task<std::expected<std::shared_ptr<const ServiceInfo>, NamingServiceError>>
NamingServiceImpl::get(std::string service_name, std::string group) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (stopping()) {
        co_return std::unexpected(shutdown_error());
    }
    if (!valid_key(service_name, group)) {
        co_return std::unexpected(validate_key(service_name, group));
    }

    if (auto entry = pool_.find(service_name, group)) {
        const auto snapshot = entry->latest;
        if (snapshot && snapshot->kind == ResultKind::Success && snapshot->data) {
            co_return std::shared_ptr<const ServiceInfo>(snapshot, &*snapshot->data);
        }
    }

    dto::req::ServiceQueryRequest request;
    request.namespace_id.set_present(config_->namespace_id());
    request.service_name.set_present(service_name);
    request.group_name.set_present(group);
    dto::resp::QueryServiceResponse response;
    mem::BufPool pool;
    auto rpc_result = co_await request_rpc(request, pool, response);
    if (!rpc_result) {
        co_return std::unexpected(map_error(std::move(rpc_result.error())));
    }
    if (!response.success()) {
        co_return std::unexpected(response_error(response));
    }
    if (!response.service_info.is_present()) {
        co_return std::unexpected(NamingServiceError{
                .code = NamingServiceErrorCode::Protocol,
                .io_error = common::IoErr::Invalid,
                .message = "Nacos query response is missing serviceInfo",
        });
    }
    auto owned = own_service_info(response.service_info.value());
    if (!owned) {
        co_return std::unexpected(std::move(owned.error()));
    }
    co_return std::make_shared<const ServiceInfo>(std::move(*owned));
}

std::expected<Subscription<ServiceInfo>, NamingServiceError>
NamingServiceImpl::subscribe(std::string_view service_name, std::string_view group,
                             Subscription<ServiceInfo>::NotifyCallback on_notify, void *ctx) {
    FIBER_ASSERT(loop_->in_loop());
    if (stopping() || !pool_.active()) {
        return std::unexpected(shutdown_error());
    }
    if (!valid_key(service_name, group)) {
        return std::unexpected(validate_key(service_name, group));
    }
    if (on_notify == nullptr) {
        return std::unexpected(NamingServiceError{
                .code = NamingServiceErrorCode::InvalidArgument,
                .io_error = common::IoErr::Invalid,
                .message = "notification callback must not be null",
        });
    }
    auto subscription = pool_.subscribe(service_name, group, on_notify, ctx);
    if (!subscription) {
        return std::unexpected(shutdown_error());
    }
    return std::move(*subscription);
}

std::expected<InstanceRegistration, NamingServiceError>
NamingServiceImpl::registry(std::string_view service_name, std::string_view group, Instance instance) {
    FIBER_ASSERT(loop_->in_loop());
    if (stopping()) {
        return std::unexpected(shutdown_error());
    }
    if (!valid_key(service_name, group)) {
        return std::unexpected(validate_key(service_name, group));
    }
    if (!valid_instance(instance)) {
        return std::unexpected(validate_instance(instance));
    }

    auto entry = std::make_shared<RegistrationEntry>(*this, std::string(service_name), std::string(group),
                                                     std::move(instance));
    registrations_.push_back(entry);
    if (ready_rpc_) {
        schedule_registration(entry);
    }
    return InstanceRegistration(std::static_pointer_cast<void>(entry), entry.get(), &update_registration_callback,
                                &subscribe_registration_callback, &close_registration_callback);
}

void NamingServiceImpl::on_subscription_add(EntryPtr entry) {
    entry->proto.draining = false;
    if (ready_rpc_) {
        schedule_subscription(std::move(entry), true);
    }
}

RemoveDecision NamingServiceImpl::on_subscription_remove(EntryPtr entry) {
    FIBER_ASSERT(loop_->in_loop());
    entry->proto.draining = true;
    if (!stopping() && ready_rpc_ && (entry->proto.registered || entry->proto.operation_in_flight)) {
        schedule_subscription(std::move(entry), false);
        return RemoveDecision::KeepLinked;
    }
    return RemoveDecision::RetireNow;
}

void NamingServiceImpl::schedule_subscription(EntryPtr entry, bool subscribe_value) {
    FIBER_ASSERT(loop_->in_loop());
    if (stopping() || !ready_rpc_ || entry->proto.operation_in_flight) {
        return;
    }
    if (subscribe_value && entry->proto.draining) {
        return;
    }
    if (!subscribe_value && !entry->proto.registered) {
        return;
    }
    entry->proto.operation_in_flight = true;
    tasks_.add();
    async::spawn([this, entry = std::move(entry), subscribe_value]() mutable {
        return run_subscription(std::move(entry), subscribe_value);
    });
}

async::DetachedTask NamingServiceImpl::run_subscription(EntryPtr entry, bool subscribe_value) noexcept {
    dto::req::SubscribeServiceRequest request;
    request.namespace_id.set_present(config_->namespace_id());
    request.service_name.set_present(entry->data_id);
    request.group_name.set_present(entry->group);
    request.subscribe = subscribe_value;
    dto::resp::SubscribeServiceResponse response;
    mem::BufPool pool;
    auto result = co_await request_rpc(request, pool, response);

    entry->proto.operation_in_flight = false;
    if (result && response.success()) {
        entry->proto.registered = subscribe_value;
    } else if (!subscribe_value) {
        entry->proto.registered = false;
    }

    if (!stopping() && entry->pool != nullptr) {
        const bool desired = !entry->proto.draining;
        if (desired != subscribe_value) {
            if (desired) {
                schedule_subscription(entry, true);
            } else if (entry->proto.registered) {
                schedule_subscription(entry, false);
            } else {
                pool_.retire(*entry);
            }
        } else if (!desired && !entry->proto.registered) {
            pool_.retire(*entry);
        }
    }
    tasks_.done();
}

void NamingServiceImpl::publish_value(NamingEntry &entry, ServiceInfo value) {
    pool_.publish(entry, NamingResult{.kind = ResultKind::Success, .data = std::move(value)});
}

async::Task<common::IoResult<dto::resp::NotifySubscriberResponse>>
NamingServiceImpl::handle_notify(void *context, NacosServerRequestContext &,
                                 const dto::req::NotifySubscriberRequest &request) noexcept {
    auto *self = static_cast<NamingServiceImpl *>(context);
    FIBER_ASSERT(self != nullptr);
    if (self->stopping() || !request.service_info.is_present()) {
        co_return dto::resp::NotifySubscriberResponse{};
    }
    auto owned = self->own_service_info(request.service_info.value());
    if (!owned) {
        co_return dto::resp::NotifySubscriberResponse{};
    }
    if (auto entry = self->pool_.find(owned->name, owned->group_name)) {
        const auto current = entry->latest;
        if (!current || current->kind != ResultKind::Success || !current->data ||
            current->data->last_ref_time != owned->last_ref_time) {
            self->publish_value(*entry, std::move(*owned));
        }
    }
    co_return dto::resp::NotifySubscriberResponse{};
}

std::expected<void, NamingServiceError> NamingServiceImpl::update_registration_callback(void *context,
                                                                                        Instance instance) noexcept {
    auto *entry = static_cast<RegistrationEntry *>(context);
    FIBER_ASSERT(entry != nullptr);
    if (!entry->owner) {
        return std::unexpected(shutdown_error());
    }
    return entry->owner->update_registration(*entry, std::move(instance));
}

InstanceRegistration::StatusSubscriber NamingServiceImpl::subscribe_registration_callback(void *context) {
    auto *entry = static_cast<RegistrationEntry *>(context);
    FIBER_ASSERT(entry != nullptr);
    return entry->status_watch.subscribe();
}

void NamingServiceImpl::close_registration_callback(void *context) noexcept {
    auto *entry = static_cast<RegistrationEntry *>(context);
    FIBER_ASSERT(entry != nullptr);
    if (entry->owner) {
        entry->owner->close_registration(*entry);
    }
}

std::expected<void, NamingServiceError> NamingServiceImpl::update_registration(RegistrationEntry &entry,
                                                                               Instance instance) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (stopping() || entry.closing) {
        return std::unexpected(shutdown_error());
    }
    if (!valid_instance(instance)) {
        return std::unexpected(validate_instance(instance));
    }
    entry.instance = std::move(instance);
    ++entry.desired_version;
    publish_registration(entry, RegistrationState::Pending);
    for (const auto &candidate: registrations_) {
        if (candidate.get() == &entry) {
            schedule_registration(candidate);
            break;
        }
    }
    return {};
}

void NamingServiceImpl::close_registration(RegistrationEntry &entry) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (entry.closing) {
        return;
    }
    entry.closing = true;
    for (const auto &candidate: registrations_) {
        if (candidate.get() != &entry) {
            continue;
        }
        if (entry.operation_in_flight) {
            return;
        }
        if (ready_rpc_ && entry.registered) {
            schedule_registration(candidate);
            return;
        }
        publish_registration(entry, RegistrationState::Closed);
        entry.owner = nullptr;
        erase_registration(entry);
        return;
    }
}

void NamingServiceImpl::schedule_registration(const std::shared_ptr<RegistrationEntry> &entry) {
    FIBER_ASSERT(loop_->in_loop());
    if (stopping() || !ready_rpc_ || entry->operation_in_flight) {
        return;
    }
    const bool deregister = entry->closing;
    if (deregister && !entry->registered) {
        publish_registration(*entry, RegistrationState::Closed);
        entry->owner = nullptr;
        erase_registration(*entry);
        return;
    }
    if (!deregister && entry->registered && entry->completed_version == entry->desired_version) {
        return;
    }
    entry->operation_in_flight = true;
    const std::uint64_t version = entry->desired_version;
    Instance instance = entry->instance;
    tasks_.add();
    async::spawn([this, entry, instance = std::move(instance), version, deregister]() mutable {
        return run_registration(entry, std::move(instance), version, deregister);
    });
}

async::DetachedTask NamingServiceImpl::run_registration(std::shared_ptr<RegistrationEntry> entry, Instance instance,
                                                        std::uint64_t version, bool deregister) noexcept {
    std::vector<json::JsonObject<std::string_view>::Entry> metadata;
    metadata.reserve(instance.metadata.size());
    for (const NamingMetadataEntry &item: instance.metadata) {
        metadata.push_back({.key = item.key, .value = item.value});
    }

    dto::NamingInstance wire;
    if (!instance.instance_id.empty()) {
        wire.instance_id.set_present(instance.instance_id);
    }
    wire.ip.set_present(instance.ip);
    wire.port = instance.port;
    wire.weight = instance.weight;
    wire.healthy = instance.healthy;
    wire.enabled = instance.enabled;
    wire.ephemeral = instance.ephemeral;
    if (!instance.cluster_name.empty()) {
        wire.cluster_name.set_present(instance.cluster_name);
    }
    if (!instance.service_name.empty()) {
        wire.service_name.set_present(instance.service_name);
    }
    wire.metadata.set_present(json::JsonObject<std::string_view>(metadata.data(), metadata.size()));

    dto::req::InstanceRequest request;
    request.namespace_id.set_present(config_->namespace_id());
    request.service_name.set_present(entry->service_name);
    request.group_name.set_present(entry->group);
    request.type.set_present(deregister ? kDeregisterInstance : kRegisterInstance);
    request.instance.set_present(wire);
    dto::resp::InstanceResponse response;
    mem::BufPool pool;
    auto result = co_await request_rpc(request, pool, response);

    entry->operation_in_flight = false;
    if (stopping()) {
        tasks_.done();
        co_return;
    }

    if (deregister) {
        entry->registered = false;
        publish_registration(*entry, RegistrationState::Closed);
        entry->owner = nullptr;
        erase_registration(*entry);
        tasks_.done();
        co_return;
    }

    entry->completed_version = version;
    if (!result) {
        publish_registration(*entry, RegistrationState::Failed, map_error(std::move(result.error())));
    } else if (!response.success()) {
        publish_registration(*entry, RegistrationState::Failed, response_error(response));
    } else {
        entry->registered = true;
        publish_registration(*entry, RegistrationState::Registered);
    }

    if (entry->closing || entry->desired_version != entry->completed_version) {
        schedule_registration(entry);
    }
    tasks_.done();
}

void NamingServiceImpl::restore_connection_state() {
    pool_.for_each([this](NamingEntry &entry) {
        if (!entry.proto.draining) {
            schedule_subscription(EntryPtr(&entry), true);
        }
    });
    for (const auto &entry: registrations_) {
        schedule_registration(entry);
    }
}

void NamingServiceImpl::erase_registration(RegistrationEntry &entry) noexcept {
    auto found = std::find_if(registrations_.begin(), registrations_.end(),
                              [&entry](const auto &candidate) { return candidate.get() == &entry; });
    if (found != registrations_.end()) {
        registrations_.erase(found);
    }
}

void NamingServiceImpl::publish_registration(RegistrationEntry &entry, RegistrationState state,
                                             std::optional<NamingServiceError> error) {
    RegistrationStatus status{.state = state};
    if (error) {
        status.error = std::make_shared<const NamingServiceError>(std::move(*error));
    }
    entry.status_publisher->publish(std::move(status));
}

async::DetachedTask NamingServiceImpl::run() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(state() == NacosServiceState::Running || state() == NacosServiceState::Stopping);
    if (running()) {
        co_await run_connection();
    }
    FIBER_ASSERT(ready_rpc_ == nullptr);
    co_await tasks_.join();
    pool_.disable();
    pool_.close_all();
    for (const auto &entry: registrations_) {
        if (entry->owner) {
            publish_registration(*entry, RegistrationState::Closed);
        }
        entry->owner = nullptr;
    }
    registrations_.clear();
    FIBER_ASSERT(state() == NacosServiceState::Stopping);
    lifecycle_publisher_->publish(NacosServiceState::Stopped);
}

async::Task<void> NamingServiceImpl::shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    auto lifecycle = lifecycle_.subscribe();
    auto snapshot = lifecycle.current();
    FIBER_ASSERT(snapshot.value != nullptr);
    if (*snapshot.value == NacosServiceState::Stopped) {
        co_return;
    }
    if (*snapshot.value == NacosServiceState::Created) {
        request_shutdown();
        registrations_.clear();
        lifecycle_publisher_->publish(NacosServiceState::Stopped);
        co_return;
    }
    request_shutdown();
    snapshot = lifecycle.current();
    while (snapshot.value && *snapshot.value != NacosServiceState::Stopped) {
        snapshot = co_await lifecycle.next(snapshot.version);
    }
    FIBER_ASSERT(snapshot.value && *snapshot.value == NacosServiceState::Stopped);
}

void NamingServiceImpl::request_shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (stopping()) {
        return;
    }
    FIBER_ASSERT(state() == NacosServiceState::Created || state() == NacosServiceState::Running);
    lifecycle_publisher_->publish(NacosServiceState::Stopping);
    pool_.disable();
    pool_.close_all();
    for (const auto &entry: registrations_) {
        publish_registration(*entry, RegistrationState::Closed);
        entry->owner = nullptr;
    }
}

void NamingServiceImpl::reset_connection_state() {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(ready_rpc_ == nullptr);
    FIBER_ASSERT(tasks_.empty());
    pool_.for_each([](NamingEntry &entry) {
        entry.proto.registered = false;
        entry.proto.operation_in_flight = false;
    });
    for (const auto &entry: registrations_) {
        entry->registered = false;
        entry->operation_in_flight = false;
        if (!entry->closing) {
            publish_registration(*entry, RegistrationState::Pending);
        }
    }
}

async::Task<NamingServiceImpl::AttemptResult>
NamingServiceImpl::run_attempt(NacosRpcEndpoint endpoint, const NacosBiRequestHandler &handlers) noexcept {
    FIBER_ASSERT(ready_rpc_ == nullptr);
    NacosRpc rpc(NacosRpcDependencies{.loop = *loop_, .config = *config_, .options = options_.rpc, .auth = auth_},
                 std::move(endpoint), NacosRpcModule::Naming);

    async::WaitGroup run_done;
    std::optional<NacosRpcCloseResult> close;
    run_done.add();
    async::spawn([&rpc, &handlers, &run_done, &close]() -> async::DetachedTask {
        close = co_await rpc.run(handlers);
        run_done.done();
    });

    AttemptResult result;
    auto lifecycle = lifecycle_.subscribe();
    auto lifecycle_snapshot = lifecycle.current();
    if (running()) {
        auto ready_or_stopping = co_await async::when_any(
                [&rpc]() { return rpc.wait_ready().select(); },
                [&lifecycle, version = lifecycle_snapshot.version]() { return lifecycle.next(version); });
        if (ready_or_stopping.is<0>()) {
            auto ready = std::move(ready_or_stopping).get<0>();
            if (ready && running()) {
                result.reached_ready = true;
                ready_rpc_ = &rpc;
                restore_connection_state();
            }
        } else {
            std::move(ready_or_stopping).get<1>();
        }
    }

    lifecycle_snapshot = lifecycle.current();
    if (!running()) {
        ready_rpc_ = nullptr;
        rpc.shutdown();
    } else if (!run_done.empty()) {
        auto closed_or_stopping = co_await async::when_any(
                [&run_done]() { return run_done.join(); },
                [&lifecycle, version = lifecycle_snapshot.version]() { return lifecycle.next(version); });
        if (closed_or_stopping.is<1>()) {
            std::move(closed_or_stopping).get<1>();
            ready_rpc_ = nullptr;
            rpc.shutdown();
        } else {
            closed_or_stopping.get<0>();
        }
    }
    if (!run_done.empty()) {
        co_await run_done.join();
    }
    FIBER_ASSERT(close.has_value());
    result.close = std::move(*close);
    ready_rpc_ = nullptr;
    co_await tasks_.join();
    reset_connection_state();
    co_return result;
}

std::chrono::milliseconds NamingServiceImpl::jittered(std::chrono::milliseconds delay) noexcept {
    random_state_ ^= random_state_ << 13;
    random_state_ ^= random_state_ >> 7;
    random_state_ ^= random_state_ << 17;
    const auto spread = delay / 5;
    if (spread <= std::chrono::milliseconds::zero()) {
        return delay;
    }
    const std::uint64_t width = static_cast<std::uint64_t>(spread.count()) * 2 + 1;
    const auto offset = static_cast<std::int64_t>(random_state_ % width) - spread.count();
    return delay + std::chrono::milliseconds(offset);
}

async::Task<void> NamingServiceImpl::run_connection() noexcept {
    NacosBiRequestHandler handlers;
    auto registered =
            handlers.add_request_handler<dto::req::NotifySubscriberRequest, dto::resp::NotifySubscriberResponse>(
                    &handle_notify, this);
    FIBER_ASSERT(registered.has_value());

    std::optional<NacosRpcEndpoint> redirect;
    auto retry_delay = options_.rpc.reconnect_initial_delay;
    while (running()) {
        if (redirect) {
            NacosRpcEndpoint endpoint = std::move(*redirect);
            redirect.reset();
            auto result = co_await run_attempt(std::move(endpoint), handlers);
            if (result.reached_ready) {
                retry_delay = options_.rpc.reconnect_initial_delay;
            }
            if (result.close.kind == NacosRpcCloseKind::Shutdown) {
                request_shutdown();
                break;
            }
            if (result.close.redirect) {
                redirect = std::move(result.close.redirect);
            }
            if (!running() || redirect) {
                continue;
            }
        }

        const std::size_t server_count = config_->server_ips().size();
        const std::size_t round_start = preferred_server_index_;
        for (std::size_t offset = 0; offset < server_count && running(); ++offset) {
            const std::size_t server_index = (round_start + offset) % server_count;
            NacosRpcEndpoint endpoint{
                    .ip = config_->server_ips()[server_index],
                    .port = config_->grpc_port(),
                    .server_index = server_index,
            };
            auto result = co_await run_attempt(std::move(endpoint), handlers);
            if (result.reached_ready) {
                preferred_server_index_ = server_index;
                retry_delay = options_.rpc.reconnect_initial_delay;
            }
            if (result.close.kind == NacosRpcCloseKind::Shutdown) {
                request_shutdown();
                break;
            }
            if (result.close.redirect) {
                redirect = std::move(result.close.redirect);
                break;
            }
        }
        if (!running()) {
            break;
        }
        if (redirect) {
            continue;
        }
        auto lifecycle = lifecycle_.subscribe();
        const auto lifecycle_snapshot = lifecycle.current();
        const auto delay = jittered(retry_delay);
        auto backoff_or_stopping = co_await async::when_any(
                [delay]() { return async::sleep(delay); },
                [&lifecycle, version = lifecycle_snapshot.version]() { return lifecycle.next(version); });
        if (backoff_or_stopping.is<0>()) {
            backoff_or_stopping.get<0>();
        } else {
            std::move(backoff_or_stopping).get<1>();
        }
        if (retry_delay < options_.rpc.reconnect_max_delay) {
            retry_delay = retry_delay > options_.rpc.reconnect_max_delay / 2 ? options_.rpc.reconnect_max_delay
                                                                             : retry_delay * 2;
        }
    }
}

std::expected<std::unique_ptr<NamingService>, NacosCreateError>
create_naming_service(NacosServiceDependencies dependencies, NamingServiceOptions options) {
    if (!NamingServiceImpl::valid_options(options)) {
        return std::unexpected(NacosCreateError{.code = NacosCreateErrorCode::InvalidOptions});
    }
    auto service = std::unique_ptr<NamingService>(
            new (std::nothrow) NamingServiceImpl(std::move(dependencies), std::move(options)));
    if (!service) {
        return std::unexpected(NacosCreateError{.code = NacosCreateErrorCode::NoMem});
    }
    return service;
}

} // namespace fiber::nacos::detail

namespace fiber::nacos {

std::expected<std::unique_ptr<NamingService>, NacosCreateError> NamingService::create(NacosClient &client,
                                                                                      NamingServiceOptions options) {
    if (!detail::NamingServiceImpl::valid_options(options)) {
        return std::unexpected(NacosCreateError{.code = NacosCreateErrorCode::InvalidOptions});
    }
    auto dependencies = client.service_dependencies();
    if (!dependencies) {
        return std::unexpected(dependencies.error());
    }
    return detail::create_naming_service(std::move(*dependencies), std::move(options));
}

} // namespace fiber::nacos
