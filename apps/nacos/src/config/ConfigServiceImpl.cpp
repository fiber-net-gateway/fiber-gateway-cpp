#include "ConfigServiceImpl.h"

#include <algorithm>
#include <chrono>
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
#include "../dto/Config.h"
#include "../rpc/NacosBiRequestHandler.h"
#include "../rpc/NacosRpc.h"

namespace fiber::nacos::detail {
namespace {

constexpr std::string_view kConfigTypeNames[] = {
        "json", "text", "yaml", "properties", "xml", "html",
};

ConfigServiceError invalid_argument(std::string message) {
    return ConfigServiceError{
            .code = ConfigServiceErrorCode::InvalidArgument,
            .io_error = common::IoErr::Invalid,
            .message = std::move(message),
    };
}

ConfigServiceError shutdown_error() {
    return ConfigServiceError{.code = ConfigServiceErrorCode::Shutdown, .io_error = common::IoErr::Canceled};
}

} // namespace

struct ConfigProtocolState {
    std::uint64_t query_sequence = 0;
    bool registered = false;
    bool query_in_flight = false;
    bool dirty = false;
    bool registration_in_flight = false;
    bool registration_dirty = false;
    bool draining = false;
};

using ConfigEntry = SubscriptionEntry<ConfigData, ConfigProtocolState>;
using ConfigEntryPtr = EntryPtr<ConfigEntry>;
using ConfigResult = SubscriptionResult<ConfigData>;

class ConfigServiceImpl final : public ConfigService {
public:
    ConfigServiceImpl(NacosServiceDependencies dependencies, ConfigServiceOptions options);
    ~ConfigServiceImpl() override;

    [[nodiscard]] static bool valid_options(const ConfigServiceOptions &options) noexcept;

    [[nodiscard]] common::IoResult<void> start() noexcept override;
    [[nodiscard]] async::Task<void> shutdown() noexcept override;

    [[nodiscard]] async::Task<std::expected<std::optional<ConfigData>, ConfigServiceError>>
    get_config(std::string data_id, std::string group) noexcept override;
    [[nodiscard]] async::Task<std::expected<void, ConfigServiceError>>
    publish(std::string data_id, std::string group, std::string content, ConfigType type,
            std::optional<std::string> cas_md5 = std::nullopt) noexcept override;
    [[nodiscard]] async::Task<std::expected<void, ConfigServiceError>>
    remove_config(std::string data_id, std::string group) noexcept override;
    [[nodiscard]] std::expected<Subscription<ConfigData>, ConfigServiceError>
    subscribe(std::string_view data_id, std::string_view group) override;

private:
    using EntryPtr = ConfigEntryPtr;

    struct AttemptResult {
        NacosRpcCloseResult close;
        bool reached_ready = false;
    };

    void on_subscription_add(EntryPtr entry);
    [[nodiscard]] RemoveDecision on_subscription_remove(EntryPtr entry);

    [[nodiscard]] static async::Task<common::IoResult<dto::resp::ConfigChangeNotifyResponse>>
    handle_config_change(void *context, NacosServerRequestContext &,
                         const dto::req::ConfigChangeNotifyRequest &request) noexcept;

    [[nodiscard]] ConfigServiceError validate_key(std::string_view data_id, std::string_view group) const;
    [[nodiscard]] ConfigServiceError map_error(NacosRpcError error) const;
    [[nodiscard]] ConfigServiceError response_error(const dto::ResponseBase &response) const;
    [[nodiscard]] bool valid_key(std::string_view data_id, std::string_view group) const noexcept;
    [[nodiscard]] bool response_content_valid(const dto::resp::ConfigQueryResponse &response) const noexcept;
    void publish_value(ConfigEntry &entry, ConfigData value);
    void schedule_query(const EntryPtr &entry);
    [[nodiscard]] async::DetachedTask query_and_sync(EntryPtr entry, std::uint64_t sequence) noexcept;
    void schedule_registration(std::vector<EntryPtr> entries, bool listen);
    void complete_registration(const EntryPtr &entry, bool listen, bool success);
    [[nodiscard]] async::DetachedTask register_entries(std::vector<EntryPtr> entries, bool listen) noexcept;
    void register_all();
    void process_changed(const dto::resp::ConfigChangeBatchListenResponse &response);
    void reset_connection_state();
    void request_shutdown() noexcept;
    [[nodiscard]] async::DetachedTask run() noexcept;
    [[nodiscard]] async::Task<void> run_connection() noexcept;
    [[nodiscard]] async::Task<AttemptResult> run_attempt(NacosRpcEndpoint endpoint,
                                                         const NacosBiRequestHandler &handlers) noexcept;
    [[nodiscard]] std::chrono::milliseconds jittered(std::chrono::milliseconds delay) noexcept;
    void task_done() noexcept;

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
    ConfigServiceOptions options_;
    NacosAuthSubscriber auth_;
    SubscriptionPool<ConfigEntry> pool_;
    async::WaitGroup tasks_;
    async::Watch<NacosServiceState> lifecycle_{NacosServiceState::Created};
    std::optional<async::Watch<NacosServiceState>::Publisher> lifecycle_publisher_;
    NacosRpc *ready_rpc_ = nullptr;
    std::size_t preferred_server_index_ = 0;
    std::uint64_t random_state_ = 0x9e3779b97f4a7c15ull;
};

ConfigServiceImpl::ConfigServiceImpl(NacosServiceDependencies dependencies, ConfigServiceOptions options) :
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

ConfigServiceImpl::~ConfigServiceImpl() {
    FIBER_ASSERT(state() == NacosServiceState::Created || state() == NacosServiceState::Stopped);
    FIBER_ASSERT(ready_rpc_ == nullptr);
    FIBER_ASSERT(tasks_.empty());
}

common::IoResult<void> ConfigServiceImpl::start() noexcept {
    if (!loop_->in_loop()) {
        return std::unexpected(common::IoErr::NotSupported);
    }
    if (state() != NacosServiceState::Created) {
        return std::unexpected(common::IoErr::Already);
    }
    lifecycle_publisher_->publish(NacosServiceState::Running);
    async::spawn(*loop_, [this]() { return run(); });
    return {};
}

NacosRpcError ConfigServiceImpl::shutdown_rpc_error() {
    return NacosRpcError{
            .code = NacosRpcErrorCode::Shutdown,
            .io_error = common::IoErr::Canceled,
    };
}

NacosRpcError ConfigServiceImpl::not_connected_rpc_error() {
    return NacosRpcError{
            .code = NacosRpcErrorCode::Transport,
            .io_error = common::IoErr::NotConnected,
    };
}

bool ConfigServiceImpl::valid_options(const ConfigServiceOptions &options) noexcept {
    return NacosRpc::valid_options(options.rpc) &&
           options.subscription_redo_interval > std::chrono::milliseconds::zero() && options.max_content_bytes > 0 &&
           options.max_content_bytes <= options.rpc.max_inbound_message_bytes && options.max_data_id_bytes > 0 &&
           options.max_group_bytes > 0 && options.max_listen_contexts_per_request > 0;
}

bool ConfigServiceImpl::valid_key(std::string_view data_id, std::string_view group) const noexcept {
    return !data_id.empty() && data_id.size() <= options_.max_data_id_bytes && !group.empty() &&
           group.size() <= options_.max_group_bytes;
}

ConfigServiceError ConfigServiceImpl::validate_key(std::string_view data_id, std::string_view group) const {
    if (data_id.empty()) {
        return invalid_argument("Nacos config dataId must not be empty");
    }
    if (data_id.size() > options_.max_data_id_bytes) {
        return invalid_argument("Nacos config dataId exceeds configured limit");
    }
    if (group.empty()) {
        return invalid_argument("Nacos config group must not be empty");
    }
    return invalid_argument("Nacos config group exceeds configured limit");
}

ConfigServiceError ConfigServiceImpl::map_error(NacosRpcError error) const {
    ConfigServiceError result{
            .io_error = error.io_error,
            .grpc_status = error.grpc_status,
            .result_code = error.result_code,
            .error_code = error.error_code,
            .message = std::move(error.message),
    };
    switch (error.code) {
        case NacosRpcErrorCode::InvalidState:
            result.code = ConfigServiceErrorCode::Transport;
            break;
        case NacosRpcErrorCode::AuthenticationUnavailable:
            result.code = ConfigServiceErrorCode::AuthenticationUnavailable;
            break;
        case NacosRpcErrorCode::Transport:
            result.code = ConfigServiceErrorCode::Transport;
            break;
        case NacosRpcErrorCode::GrpcStatus:
            result.code = ConfigServiceErrorCode::GrpcStatus;
            break;
        case NacosRpcErrorCode::Protocol:
            result.code = error.io_error == common::IoErr::MessageTooLarge ? ConfigServiceErrorCode::ContentTooLarge
                                                                           : ConfigServiceErrorCode::Protocol;
            break;
        case NacosRpcErrorCode::Server:
            result.code = ConfigServiceErrorCode::Server;
            break;
        case NacosRpcErrorCode::Shutdown:
            result.code = ConfigServiceErrorCode::Shutdown;
            break;
    }
    return result;
}

ConfigServiceError ConfigServiceImpl::response_error(const dto::ResponseBase &response) const {
    ConfigServiceError error{
            .code = ConfigServiceErrorCode::Server,
            .result_code = response.result_code,
            .error_code = response.error_code,
    };
    if (response.message.is_present()) {
        error.message.assign(response.message.value().substr(0, 512));
    }
    return error;
}

bool ConfigServiceImpl::response_content_valid(const dto::resp::ConfigQueryResponse &response) const noexcept {
    return response.md5.is_present() && response.content.is_present() &&
           response.content.value().size() <= options_.max_content_bytes;
}

async::Task<std::expected<std::optional<ConfigData>, ConfigServiceError>>
ConfigServiceImpl::get_config(std::string data_id, std::string group) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (stopping()) {
        co_return std::unexpected(shutdown_error());
    }
    if (!valid_key(data_id, group)) {
        co_return std::unexpected(validate_key(data_id, group));
    }

    // Serve from a synced subscription cache if present.
    if (auto entry = pool_.find(data_id, group)) {
        const auto snapshot = entry->watch.current();
        if (snapshot.value != nullptr && snapshot.value->kind == ResultKind::Success &&
            snapshot.value->data.has_value()) {
            if (snapshot.value->data->state == ConfigState::Present) {
                co_return std::optional<ConfigData>(*snapshot.value->data);
            }
            if (snapshot.value->data->state == ConfigState::NotFound) {
                co_return std::optional<ConfigData>{};
            }
        }
    }

    dto::req::ConfigQueryRequest request = dto::req::ConfigQueryRequest::build(data_id, group, config_->tenant());
    dto::resp::ConfigQueryResponse response;
    mem::BufPool pool;
    auto result = co_await request_rpc(request, pool, response);
    if (!result) {
        co_return std::unexpected(map_error(std::move(result.error())));
    }
    if (response.error_code == dto::resp::ConfigQueryResponse::kConfigNotFound) {
        co_return std::optional<ConfigData>{};
    }
    if (!response.success()) {
        co_return std::unexpected(response_error(response));
    }
    if (!response.md5.is_present() || !response.content.is_present()) {
        co_return std::unexpected(ConfigServiceError{
                .code = ConfigServiceErrorCode::Protocol,
                .io_error = common::IoErr::Invalid,
                .message = "Nacos config query response is missing content or md5",
        });
    }
    if (response.content.value().size() > options_.max_content_bytes) {
        co_return std::unexpected(ConfigServiceError{
                .code = ConfigServiceErrorCode::ContentTooLarge,
                .io_error = common::IoErr::MessageTooLarge,
                .message = "Nacos config content exceeds configured limit",
        });
    }
    co_return std::optional<ConfigData>(ConfigData{
            .state = ConfigState::Present,
            .md5 = std::string(response.md5.value()),
            .content = std::string(response.content.value()),
    });
}

async::Task<std::expected<void, ConfigServiceError>>
ConfigServiceImpl::publish(std::string data_id, std::string group, std::string content, ConfigType type,
                           std::optional<std::string> cas_md5) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (stopping()) {
        co_return std::unexpected(shutdown_error());
    }
    if (!valid_key(data_id, group)) {
        co_return std::unexpected(validate_key(data_id, group));
    }
    if (content.size() > options_.max_content_bytes) {
        co_return std::unexpected(ConfigServiceError{
                .code = ConfigServiceErrorCode::ContentTooLarge,
                .io_error = common::IoErr::MessageTooLarge,
                .message = "Nacos config content exceeds configured limit",
        });
    }
    const std::size_t type_index = static_cast<std::size_t>(type);
    if (type_index >= std::size(kConfigTypeNames)) {
        co_return std::unexpected(invalid_argument("invalid Nacos config type"));
    }

    dto::req::ConfigPublishRequest request;
    request.data_id.set_present(data_id);
    request.group.set_present(group);
    request.tenant.set_present(config_->tenant());
    request.content.set_present(content);
    if (cas_md5 && !cas_md5->empty()) {
        request.cas_md5.set_present(*cas_md5);
    }
    json::JsonObject<std::string_view>::Entry addition_entries[] = {
            {.key = "type", .value = kConfigTypeNames[type_index]},
    };
    request.addition_map.set_present(json::JsonObject<std::string_view>(addition_entries, std::size(addition_entries)));

    dto::resp::ConfigPublishResponse response;
    mem::BufPool pool;
    auto result = co_await request_rpc(request, pool, response);
    if (!result) {
        co_return std::unexpected(map_error(std::move(result.error())));
    }
    if (!response.success()) {
        co_return std::unexpected(response_error(response));
    }
    co_return std::expected<void, ConfigServiceError>{};
}

async::Task<std::expected<void, ConfigServiceError>> ConfigServiceImpl::remove_config(std::string data_id,
                                                                                      std::string group) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (stopping()) {
        co_return std::unexpected(shutdown_error());
    }
    if (!valid_key(data_id, group)) {
        co_return std::unexpected(validate_key(data_id, group));
    }

    dto::req::ConfigRemoveRequest request;
    request.data_id.set_present(data_id);
    request.group.set_present(group);
    request.tenant.set_present(config_->tenant());
    dto::resp::ConfigRemoveResponse response;
    mem::BufPool pool;
    auto result = co_await request_rpc(request, pool, response);
    if (!result) {
        co_return std::unexpected(map_error(std::move(result.error())));
    }
    if (!response.success()) {
        co_return std::unexpected(response_error(response));
    }
    co_return std::expected<void, ConfigServiceError>{};
}

std::expected<Subscription<ConfigData>, ConfigServiceError> ConfigServiceImpl::subscribe(std::string_view data_id,
                                                                                         std::string_view group) {
    FIBER_ASSERT(loop_->in_loop());
    if (stopping() || !pool_.active()) {
        return std::unexpected(shutdown_error());
    }
    if (!valid_key(data_id, group)) {
        return std::unexpected(validate_key(data_id, group));
    }
    auto subscription = pool_.subscribe(data_id, group);
    if (!subscription) {
        return std::unexpected(shutdown_error());
    }
    return std::move(*subscription);
}

void ConfigServiceImpl::on_subscription_add(EntryPtr entry) {
    if (ready_rpc_) {
        schedule_registration({std::move(entry)}, true);
    }
}

RemoveDecision ConfigServiceImpl::on_subscription_remove(EntryPtr entry) {
    FIBER_ASSERT(loop_->in_loop());
    entry->proto.draining = true;
    if (!stopping() && ready_rpc_ && (entry->proto.registered || entry->proto.registration_in_flight)) {
        // Registered or mid-registration: defer the free. The pool has already
        // unlinked the entry from the tree; this owning EntryPtr keeps it alive
        // while an async listen=false runs, and releases it to 0 on completion.
        schedule_registration({std::move(entry)}, false);
        return RemoveDecision::Defer;
    }
    // Never registered, disconnected, or shutting down: free now.
    return RemoveDecision::UnlinkNow;
}

void ConfigServiceImpl::publish_value(ConfigEntry &entry, ConfigData value) {
    FIBER_ASSERT(loop_->in_loop());
    pool_.publish(entry, ConfigResult{.kind = ResultKind::Success, .data = std::move(value)});
}

void ConfigServiceImpl::schedule_query(const EntryPtr &entry) {
    FIBER_ASSERT(loop_->in_loop());
    if (stopping() || !ready_rpc_ || entry->proto.draining) {
        return;
    }
    if (entry->proto.query_in_flight) {
        entry->proto.dirty = true;
        return;
    }
    entry->proto.query_in_flight = true;
    entry->proto.dirty = false;
    const std::uint64_t sequence = ++entry->proto.query_sequence;
    tasks_.add();
    async::spawn(*loop_, [this, entry, sequence]() { return query_and_sync(entry, sequence); });
}

async::DetachedTask ConfigServiceImpl::query_and_sync(EntryPtr entry, std::uint64_t sequence) noexcept {
    while (!stopping() && ready_rpc_ && !entry->proto.draining) {
        // Stop once the entry is detached from the pool tree (last subscriber
        // gone -> on_subscription_remove set draining and the pool detached it).
        // The watch/publisher outlive tree membership, so reads above are safe.
        if (entry->pool == nullptr) {
            break;
        }
        dto::req::ConfigQueryRequest request =
                dto::req::ConfigQueryRequest::build(entry->data_id, entry->group, config_->tenant());
        dto::resp::ConfigQueryResponse response;
        mem::BufPool pool;
        auto result = co_await request_rpc(request, pool, response);

        if (!stopping() && ready_rpc_ && !entry->proto.draining && sequence == entry->proto.query_sequence && result) {
            const auto snapshot = entry->watch.current();
            const ConfigData *current =
                    (snapshot.value != nullptr && snapshot.value->data.has_value()) ? &*snapshot.value->data : nullptr;
            if (response.error_code == dto::resp::ConfigQueryResponse::kConfigNotFound) {
                const bool was_not_found = current != nullptr && current->state == ConfigState::NotFound;
                if (!was_not_found) {
                    publish_value(*entry, ConfigData{.state = ConfigState::NotFound});
                }
            } else if (response.success() && response_content_valid(response)) {
                const std::string_view md5 = response.md5.value();
                const bool stale = current == nullptr || current->state != ConfigState::Present || current->md5 != md5;
                if (stale) {
                    publish_value(*entry, ConfigData{
                                                  .state = ConfigState::Present,
                                                  .md5 = std::string(md5),
                                                  .content = std::string(response.content.value()),
                                          });
                }
            }
        }

        if (!entry->proto.dirty || stopping() || entry->proto.draining || !ready_rpc_) {
            break;
        }
        entry->proto.dirty = false;
        sequence = ++entry->proto.query_sequence;
    }
    entry->proto.query_in_flight = false;
    task_done();
}

void ConfigServiceImpl::schedule_registration(std::vector<EntryPtr> entries, bool listen) {
    FIBER_ASSERT(loop_->in_loop());
    if (stopping() || !ready_rpc_ || entries.empty()) {
        return;
    }
    std::vector<EntryPtr> scheduled;
    scheduled.reserve(entries.size());
    for (EntryPtr &entry: entries) {
        if (entry->proto.registration_in_flight) {
            entry->proto.registration_dirty = true;
            continue;
        }
        entry->proto.registration_in_flight = true;
        scheduled.push_back(std::move(entry));
    }
    if (scheduled.empty()) {
        return;
    }
    tasks_.add();
    async::spawn(*loop_, [this, entries = std::move(scheduled), listen]() mutable {
        return register_entries(std::move(entries), listen);
    });
}

void ConfigServiceImpl::complete_registration(const EntryPtr &entry, bool listen, bool success) {
    FIBER_ASSERT(entry->proto.registration_in_flight);
    entry->proto.registration_in_flight = false;
    if (success) {
        entry->proto.registered = listen;
    }
    const bool dirty = std::exchange(entry->proto.registration_dirty, false);
    if (stopping()) {
        return;
    }

    // Only entries still in the tree (live subscribers) take further action. A
    // detached draining entry has no subscribers and is just awaiting its async
    // unregistration to release the owning EntryPtr to 0.
    if (entry->pool == nullptr) {
        return;
    }
    const bool wants_listen = !entry->proto.draining && ready_rpc_;
    if (wants_listen) {
        if ((!success && (!listen || dirty)) || (success && (!entry->proto.registered || dirty || !listen))) {
            schedule_registration({entry}, true);
        }
        return;
    }
    if (ready_rpc_ && entry->proto.registered && (success || dirty || listen)) {
        schedule_registration({entry}, false);
        return;
    }
    // No further registration needed; if this entry is draining with no live
    // subscribers the pool already detached it - nothing to do here.
}

async::DetachedTask ConfigServiceImpl::register_entries(std::vector<EntryPtr> entries, bool listen) noexcept {
    std::size_t offset = 0;
    while (offset < entries.size()) {
        const std::size_t limit = std::min(entries.size(), offset + options_.max_listen_contexts_per_request);
        std::vector<EntryPtr> included;
        std::vector<dto::req::ConfigListenContext> contexts;
        included.reserve(limit - offset);
        contexts.reserve(limit - offset);
        for (; offset < limit; ++offset) {
            const EntryPtr &entry = entries[offset];
            if (stopping() || !ready_rpc_ || (listen && entry->proto.draining) ||
                (!listen && !entry->proto.registered)) {
                complete_registration(entry, listen, false);
                continue;
            }
            dto::req::ConfigListenContext context;
            context.group.set_present(entry->group);
            context.data_id.set_present(entry->data_id);
            context.tenant.set_present(config_->tenant());
            const auto snapshot = entry->watch.current();
            if (snapshot.value != nullptr && snapshot.value->kind == ResultKind::Success &&
                snapshot.value->data.has_value()) {
                const ConfigData &data = *snapshot.value->data;
                if (data.state == ConfigState::Present) {
                    context.md5.set_present(data.md5);
                } else if (data.state == ConfigState::NotFound) {
                    context.md5.set_present("");
                }
            }
            // never-synced: omit md5 (matches Java: data != null ? md5 : null)
            contexts.push_back(context);
            included.push_back(entry);
        }
        if (contexts.empty()) {
            continue;
        }

        dto::req::ConfigBatchListenRequest request;
        request.listen = listen;
        request.config_listen_contexts =
                json::JsonArray<dto::req::ConfigListenContext>(contexts.data(), contexts.size());
        dto::resp::ConfigChangeBatchListenResponse response;
        mem::BufPool pool;
        auto result = co_await request_rpc(request, pool, response);
        const bool success = result && !stopping() && ready_rpc_ && response.success();
        if (!success) {
            for (const EntryPtr &entry: included) {
                complete_registration(entry, listen, false);
            }
            continue;
        }
        if (listen) {
            process_changed(response);
        }
        for (const EntryPtr &entry: included) {
            complete_registration(entry, listen, true);
        }
    }
    task_done();
}

void ConfigServiceImpl::register_all() {
    std::vector<EntryPtr> entries;
    pool_.for_each([&entries](ConfigEntry &entry) {
        if (!entry.proto.draining) {
            entries.push_back(EntryPtr(&entry));
        }
    });
    schedule_registration(std::move(entries), true);
}

void ConfigServiceImpl::process_changed(const dto::resp::ConfigChangeBatchListenResponse &response) {
    for (const dto::resp::ConfigContext &changed: response.changed_configs) {
        if (!changed.data_id.is_present() || !changed.group.is_present()) {
            continue;
        }
        if (changed.tenant.is_present() && changed.tenant.value() != config_->tenant()) {
            continue;
        }
        if (auto entry = pool_.find(changed.data_id.value(), changed.group.value())) {
            schedule_query(entry);
        }
    }
}

async::Task<common::IoResult<dto::resp::ConfigChangeNotifyResponse>>
ConfigServiceImpl::handle_config_change(void *context, NacosServerRequestContext &,
                                        const dto::req::ConfigChangeNotifyRequest &request) noexcept {
    FIBER_ASSERT(context != nullptr);
    auto *self = static_cast<ConfigServiceImpl *>(context);
    if (!request.data_id.is_present() || !request.group.is_present()) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    const bool tenant_matches = !request.tenant.is_present() || request.tenant.value() == self->config_->tenant();
    if (!self->stopping() && self->ready_rpc_ && tenant_matches) {
        if (auto entry = self->pool_.find(request.data_id.value(), request.group.value())) {
            self->schedule_query(entry);
        }
    }
    co_return dto::resp::ConfigChangeNotifyResponse{};
}

async::DetachedTask ConfigServiceImpl::run() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(state() == NacosServiceState::Running || state() == NacosServiceState::Stopping);
    if (running()) {
        co_await run_connection();
    }
    FIBER_ASSERT(ready_rpc_ == nullptr);
    co_await tasks_.join();
    pool_.disable();
    pool_.close_all();
    FIBER_ASSERT(state() == NacosServiceState::Stopping);
    lifecycle_publisher_->publish(NacosServiceState::Stopped);
}

async::Task<void> ConfigServiceImpl::shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    auto lifecycle = lifecycle_.subscribe();
    auto snapshot = lifecycle.current();
    FIBER_ASSERT(snapshot.value != nullptr);
    if (*snapshot.value == NacosServiceState::Stopped) {
        co_return;
    }
    if (*snapshot.value == NacosServiceState::Created) {
        request_shutdown();
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

void ConfigServiceImpl::request_shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (stopping()) {
        return;
    }
    FIBER_ASSERT(state() == NacosServiceState::Created || state() == NacosServiceState::Running);
    lifecycle_publisher_->publish(NacosServiceState::Stopping);
    pool_.disable();
    pool_.close_all();
}

void ConfigServiceImpl::reset_connection_state() {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(ready_rpc_ == nullptr);
    FIBER_ASSERT(tasks_.empty());
    pool_.for_each([](ConfigEntry &entry) {
        FIBER_ASSERT(!entry.proto.query_in_flight);
        FIBER_ASSERT(!entry.proto.registration_in_flight);
        entry.proto.registered = false;
        entry.proto.dirty = false;
        entry.proto.registration_dirty = false;
    });
}

async::Task<ConfigServiceImpl::AttemptResult>
ConfigServiceImpl::run_attempt(NacosRpcEndpoint endpoint, const NacosBiRequestHandler &handlers) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(ready_rpc_ == nullptr);
    NacosRpc rpc(NacosRpcDependencies{.loop = *loop_, .config = *config_, .options = options_.rpc, .auth = auth_},
                 std::move(endpoint), NacosRpcModule::Config);

    async::WaitGroup run_done;
    std::optional<NacosRpcCloseResult> close;
    run_done.add();
    async::spawn(*loop_, [&rpc, &handlers, &run_done, &close]() -> async::DetachedTask {
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
                register_all();
            }
        } else {
            std::move(ready_or_stopping).get<1>();
        }
    }

    lifecycle_snapshot = lifecycle.current();
    while (running() && !run_done.empty()) {
        auto event = co_await async::when_any(
                [&run_done]() { return run_done.join(); },
                [&lifecycle, version = lifecycle_snapshot.version]() { return lifecycle.next(version); },
                [delay = options_.subscription_redo_interval]() { return async::sleep(delay); });
        if (event.is<0>()) {
            event.get<0>();
            break;
        }
        if (event.is<1>()) {
            std::move(event).get<1>();
            ready_rpc_ = nullptr;
            rpc.shutdown();
            break;
        }
        event.get<2>();
        if (ready_rpc_ && running() && !run_done.empty()) {
            register_all();
        }
        lifecycle_snapshot = lifecycle.current();
    }
    if (!running() && !run_done.empty()) {
        ready_rpc_ = nullptr;
        rpc.shutdown();
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

std::chrono::milliseconds ConfigServiceImpl::jittered(std::chrono::milliseconds delay) noexcept {
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

async::Task<void> ConfigServiceImpl::run_connection() noexcept {
    NacosBiRequestHandler handlers;
    auto registered =
            handlers.add_request_handler<dto::req::ConfigChangeNotifyRequest, dto::resp::ConfigChangeNotifyResponse>(
                    &handle_config_change, this);
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

void ConfigServiceImpl::task_done() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    tasks_.done();
}

std::expected<std::unique_ptr<ConfigService>, NacosCreateError>
create_config_service(NacosServiceDependencies dependencies, ConfigServiceOptions options) {
    if (!ConfigServiceImpl::valid_options(options)) {
        return std::unexpected(NacosCreateError{.code = NacosCreateErrorCode::InvalidOptions});
    }
    auto service = std::unique_ptr<ConfigService>(
            new (std::nothrow) ConfigServiceImpl(std::move(dependencies), std::move(options)));
    if (!service) {
        return std::unexpected(NacosCreateError{.code = NacosCreateErrorCode::NoMem});
    }
    return service;
}

} // namespace fiber::nacos::detail

namespace fiber::nacos {

std::expected<std::unique_ptr<ConfigService>, NacosCreateError> ConfigService::create(NacosClient &client,
                                                                                      ConfigServiceOptions options) {
    if (!detail::ConfigServiceImpl::valid_options(options)) {
        return std::unexpected(NacosCreateError{.code = NacosCreateErrorCode::InvalidOptions});
    }
    auto dependencies = client.service_dependencies();
    if (!dependencies) {
        return std::unexpected(dependencies.error());
    }
    return detail::create_config_service(std::move(*dependencies), std::move(options));
}

} // namespace fiber::nacos
