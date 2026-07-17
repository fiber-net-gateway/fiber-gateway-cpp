#include "ConfigServiceImpl.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <utility>

#include <async/Timeout.h>
#include <common/Assert.h>
#include <fiber/nacos/dto/Config.h>

namespace fiber::nacos {

ConfigSubscription::ConfigSubscription(std::shared_ptr<detail::ConfigSubscriptionLease> lease,
                                       Subscriber subscriber) noexcept :
    lease_(std::move(lease)), subscriber_(std::move(subscriber)) {}

ConfigSubscription::ConfigSubscription(ConfigSubscription &&other) noexcept = default;

ConfigSubscription &ConfigSubscription::operator=(ConfigSubscription &&other) noexcept {
    if (this != &other) {
        close();
        lease_ = std::move(other.lease_);
        subscriber_ = std::move(other.subscriber_);
    }
    return *this;
}

ConfigSubscription::~ConfigSubscription() { close(); }

ConfigSubscription::Snapshot ConfigSubscription::current() const {
    FIBER_ASSERT(subscriber_.has_value());
    return subscriber_->current();
}

ConfigSubscription::NextAwaiter ConfigSubscription::next(std::uint64_t received_version) const noexcept {
    FIBER_ASSERT(subscriber_.has_value());
    return subscriber_->next(received_version);
}

void ConfigSubscription::close() noexcept {
    if (lease_) {
        lease_->close();
        lease_.reset();
    }
    subscriber_.reset();
}

bool ConfigSubscription::valid() const noexcept { return lease_ != nullptr && subscriber_.has_value(); }

} // namespace fiber::nacos

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

NacosRpcError protocol_error(std::string message, common::IoErr io_error = common::IoErr::Invalid) {
    return NacosRpcError{
            .code = NacosRpcErrorCode::Protocol,
            .io_error = io_error,
            .message = std::move(message),
    };
}

std::size_t hash_key(std::string_view data_id, std::string_view group) noexcept {
    const std::size_t first = std::hash<std::string_view>{}(data_id);
    const std::size_t second = std::hash<std::string_view>{}(group);
    return first ^ (second + 0x9e3779b9u + (first << 6u) + (first >> 2u));
}

} // namespace

std::size_t ConfigKeyHash::operator()(const ConfigKey &key) const noexcept { return hash_key(key.data_id, key.group); }

std::size_t ConfigKeyHash::operator()(ConfigKeyView key) const noexcept { return hash_key(key.data_id, key.group); }

bool ConfigKeyEqual::operator()(const ConfigKey &lhs, const ConfigKey &rhs) const noexcept { return lhs == rhs; }

bool ConfigKeyEqual::operator()(const ConfigKey &lhs, ConfigKeyView rhs) const noexcept {
    return lhs.data_id == rhs.data_id && lhs.group == rhs.group;
}

bool ConfigKeyEqual::operator()(ConfigKeyView lhs, const ConfigKey &rhs) const noexcept { return (*this)(rhs, lhs); }

ConfigEntry::ConfigEntry(ConfigKey key_value) : key(std::move(key_value)) {
    publisher = watch.acquire_publisher();
    FIBER_ASSERT(publisher.has_value());
}

ConfigSubscriptionLease::ConfigSubscriptionLease(std::shared_ptr<ConfigServiceLifetime> lifetime_value,
                                                 std::shared_ptr<ConfigEntry> entry_value) noexcept :
    lifetime(std::move(lifetime_value)), entry(std::move(entry_value)) {}

ConfigSubscriptionLease::~ConfigSubscriptionLease() { close(); }

void ConfigSubscriptionLease::close() noexcept {
    if (closed) {
        return;
    }
    closed = true;
    if (lifetime && lifetime->owner) {
        lifetime->owner->release_subscription(entry);
    }
    entry.reset();
    lifetime.reset();
}

ConfigServiceImpl::ConfigServiceImpl(event::EventLoop &loop, const NacosClientConfig &config,
                                     const NacosClientOptions &options, NacosGrpcConnection &connection) :
    loop_(&loop), config_(&config), options_(&options), connection_(&connection),
    lifetime_(std::make_shared<ConfigServiceLifetime>()) {
    lifetime_->owner = this;
    connection_->set_push_handler(NacosPushHandler{
            .context = this,
            .callback = &ConfigServiceImpl::handle_push,
    });
}

ConfigServiceImpl::~ConfigServiceImpl() {
    lifetime_->owner = nullptr;
    FIBER_ASSERT(!run_active_);
    FIBER_ASSERT(tasks_.empty());
}

bool ConfigServiceImpl::valid_key(std::string_view data_id, std::string_view group) const noexcept {
    return !data_id.empty() && data_id.size() <= options_->max_config_data_id_bytes && !group.empty() &&
           group.size() <= options_->max_config_group_bytes;
}

ConfigServiceError ConfigServiceImpl::validate_key(std::string_view data_id, std::string_view group) const {
    if (data_id.empty()) {
        return invalid_argument("Nacos config dataId must not be empty");
    }
    if (data_id.size() > options_->max_config_data_id_bytes) {
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
        case NacosRpcErrorCode::AuthenticationUnavailable:
            result.code = ConfigServiceErrorCode::AuthenticationUnavailable;
            break;
        case NacosRpcErrorCode::Transport:
        case NacosRpcErrorCode::QueueFull:
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
           response.content.value().size() <= options_->max_config_content_bytes;
}

ConfigServiceImpl::EntryPtr ConfigServiceImpl::find_entry(std::string_view data_id, std::string_view group) {
    auto it = entries_.find(ConfigKeyView{.data_id = data_id, .group = group});
    return it == entries_.end() ? nullptr : it->second;
}

async::Task<std::expected<std::optional<ConfigData>, ConfigServiceError>>
ConfigServiceImpl::get_config(std::string data_id, std::string group) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (stopping_) {
        co_return std::unexpected(
                ConfigServiceError{.code = ConfigServiceErrorCode::Shutdown, .io_error = common::IoErr::Canceled});
    }
    if (!valid_key(data_id, group)) {
        co_return std::unexpected(validate_key(data_id, group));
    }

    if (auto entry = find_entry(data_id, group)) {
        const auto snapshot = entry->watch.current();
        if (snapshot.value->state == ConfigState::Present) {
            co_return std::optional<ConfigData>(snapshot.value->data);
        }
        if (snapshot.value->state == ConfigState::NotFound) {
            co_return std::optional<ConfigData>{};
        }
    }

    dto::req::ConfigQueryRequest request = dto::req::ConfigQueryRequest::build(data_id, group, config_->tenant());
    dto::resp::ConfigQueryResponse response;
    mem::BufPool pool;
    auto result = co_await connection_->request(request, pool, response);
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
    if (response.content.value().size() > options_->max_config_content_bytes) {
        co_return std::unexpected(ConfigServiceError{
                .code = ConfigServiceErrorCode::ContentTooLarge,
                .io_error = common::IoErr::MessageTooLarge,
                .message = "Nacos config content exceeds configured limit",
        });
    }
    co_return std::optional<ConfigData>(ConfigData{
            .md5 = std::string(response.md5.value()),
            .content = std::string(response.content.value()),
    });
}

async::Task<std::expected<void, ConfigServiceError>>
ConfigServiceImpl::publish(std::string data_id, std::string group, std::string content, ConfigType type,
                           std::optional<std::string> cas_md5) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (stopping_) {
        co_return std::unexpected(
                ConfigServiceError{.code = ConfigServiceErrorCode::Shutdown, .io_error = common::IoErr::Canceled});
    }
    if (!valid_key(data_id, group)) {
        co_return std::unexpected(validate_key(data_id, group));
    }
    if (content.size() > options_->max_config_content_bytes) {
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
    auto result = co_await connection_->request(request, pool, response);
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
    if (stopping_) {
        co_return std::unexpected(
                ConfigServiceError{.code = ConfigServiceErrorCode::Shutdown, .io_error = common::IoErr::Canceled});
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
    auto result = co_await connection_->request(request, pool, response);
    if (!result) {
        co_return std::unexpected(map_error(std::move(result.error())));
    }
    if (!response.success()) {
        co_return std::unexpected(response_error(response));
    }
    co_return std::expected<void, ConfigServiceError>{};
}

std::expected<ConfigSubscription, ConfigServiceError> ConfigServiceImpl::subscribe(std::string_view data_id,
                                                                                   std::string_view group) {
    FIBER_ASSERT(loop_->in_loop());
    if (stopping_) {
        return std::unexpected(
                ConfigServiceError{.code = ConfigServiceErrorCode::Shutdown, .io_error = common::IoErr::Canceled});
    }
    if (!valid_key(data_id, group)) {
        return std::unexpected(validate_key(data_id, group));
    }

    EntryPtr entry = find_entry(data_id, group);
    const bool first = entry == nullptr;
    const bool reactivating = entry && entry->closing;
    if (first) {
        entry = std::make_shared<ConfigEntry>(ConfigKey{
                .data_id = std::string(data_id),
                .group = std::string(group),
        });
        entries_.emplace(entry->key, entry);
    }
    entry->closing = false;
    ++entry->subscribers;
    auto lease = std::make_shared<ConfigSubscriptionLease>(lifetime_, entry);
    ConfigSubscription subscription(lease, entry->watch.subscribe());
    if ((first || reactivating) && active_generation_ != 0) {
        schedule_registration({entry}, true, active_generation_);
    }
    return subscription;
}

void ConfigServiceImpl::release_subscription(const EntryPtr &entry) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (!entry || entry->subscribers == 0) {
        return;
    }
    --entry->subscribers;
    if (entry->subscribers != 0) {
        return;
    }
    entry->closing = true;
    const std::uint64_t generation = active_generation_;
    if (!stopping_ && generation != 0 &&
        (entry->registered_generation == generation || entry->registration_in_flight)) {
        schedule_registration({entry}, false, generation);
    }
    if (!entry->registration_in_flight &&
        (generation == 0 || entry->registered_generation == 0 || entry->registered_generation != generation)) {
        auto it = entries_.find(entry->key);
        if (it != entries_.end() && it->second == entry) {
            entries_.erase(it);
        }
    }
}

void ConfigServiceImpl::publish_snapshot(const EntryPtr &entry, ConfigSnapshot snapshot) {
    FIBER_ASSERT(loop_->in_loop());
    entry->publisher->publish(std::move(snapshot));
}

void ConfigServiceImpl::schedule_query(const EntryPtr &entry, std::uint64_t generation) {
    FIBER_ASSERT(loop_->in_loop());
    if (stopping_ || generation == 0 || generation != active_generation_ || entry->subscribers == 0 || entry->closing) {
        return;
    }
    if (entry->query_in_flight) {
        entry->dirty = true;
        return;
    }
    entry->query_in_flight = true;
    entry->dirty = false;
    const std::uint64_t sequence = ++entry->query_sequence;
    tasks_.add();
    async::spawn(*loop_, [this, entry, generation, sequence]() { return query_and_sync(entry, generation, sequence); });
}

async::DetachedTask ConfigServiceImpl::query_and_sync(EntryPtr entry, std::uint64_t generation,
                                                      std::uint64_t sequence) noexcept {
    while (!stopping_ && generation != 0 && entry->subscribers > 0 && !entry->closing) {
        dto::req::ConfigQueryRequest request =
                dto::req::ConfigQueryRequest::build(entry->key.data_id, entry->key.group, config_->tenant());
        dto::resp::ConfigQueryResponse response;
        mem::BufPool pool;
        auto result = co_await connection_->request(request, pool, response);

        if (!stopping_ && entry->subscribers > 0 && !entry->closing && generation == active_generation_ &&
            sequence == entry->query_sequence && result) {
            if (response.error_code == dto::resp::ConfigQueryResponse::kConfigNotFound) {
                if (entry->watch.current().value->state != ConfigState::NotFound) {
                    publish_snapshot(entry, ConfigSnapshot{.state = ConfigState::NotFound});
                }
            } else if (response.success() && response_content_valid(response)) {
                const std::string_view md5 = response.md5.value();
                const auto snapshot = entry->watch.current();
                if (snapshot.value->state != ConfigState::Present || snapshot.value->data.md5 != md5) {
                    publish_snapshot(entry, ConfigSnapshot{
                                                    .state = ConfigState::Present,
                                                    .data =
                                                            ConfigData{
                                                                    .md5 = std::string(md5),
                                                                    .content = std::string(response.content.value()),
                                                            },
                                            });
                }
            }
        }

        if (!entry->dirty || stopping_ || entry->subscribers == 0 || entry->closing || active_generation_ == 0) {
            break;
        }
        entry->dirty = false;
        generation = active_generation_;
        sequence = ++entry->query_sequence;
    }
    entry->query_in_flight = false;
    task_done();
}

void ConfigServiceImpl::schedule_registration(std::vector<EntryPtr> entries, bool listen, std::uint64_t generation) {
    FIBER_ASSERT(loop_->in_loop());
    if (stopping_ || generation == 0 || entries.empty()) {
        return;
    }
    std::vector<EntryPtr> scheduled;
    scheduled.reserve(entries.size());
    for (EntryPtr &entry: entries) {
        if (entry->registration_in_flight) {
            entry->registration_dirty = true;
            continue;
        }
        entry->registration_in_flight = true;
        scheduled.push_back(std::move(entry));
    }
    if (scheduled.empty()) {
        return;
    }
    tasks_.add();
    async::spawn(*loop_, [this, entries = std::move(scheduled), listen, generation]() mutable {
        return register_entries(std::move(entries), listen, generation);
    });
}

void ConfigServiceImpl::complete_registration(const EntryPtr &entry, bool listen, std::uint64_t generation,
                                              bool success) {
    FIBER_ASSERT(entry->registration_in_flight);
    entry->registration_in_flight = false;
    if (success) {
        if (listen) {
            entry->registered_generation = generation;
        } else if (entry->registered_generation == generation) {
            entry->registered_generation = 0;
        }
    }
    const bool dirty = std::exchange(entry->registration_dirty, false);
    if (stopping_) {
        return;
    }

    const bool wants_listen = entry->subscribers > 0 && !entry->closing && active_generation_ != 0;
    if (wants_listen) {
        if ((!success && (!listen || dirty)) ||
            (success && (entry->registered_generation != active_generation_ || dirty || !listen))) {
            schedule_registration({entry}, true, active_generation_);
        }
        return;
    }
    if (active_generation_ != 0 && entry->registered_generation == active_generation_ && (success || dirty || listen)) {
        schedule_registration({entry}, false, active_generation_);
        return;
    }
    auto it = entries_.find(entry->key);
    if (it != entries_.end() && it->second == entry) {
        entries_.erase(it);
    }
}

async::DetachedTask ConfigServiceImpl::register_entries(std::vector<EntryPtr> entries, bool listen,
                                                        std::uint64_t generation) noexcept {
    std::size_t offset = 0;
    while (offset < entries.size()) {
        const std::size_t limit = std::min(entries.size(), offset + options_->max_listen_contexts_per_request);
        std::vector<EntryPtr> included;
        std::vector<dto::req::ConfigListenContext> contexts;
        included.reserve(limit - offset);
        contexts.reserve(limit - offset);
        for (; offset < limit; ++offset) {
            const EntryPtr &entry = entries[offset];
            if (stopping_ || generation != active_generation_ ||
                (listen && (entry->subscribers == 0 || entry->closing)) ||
                (!listen && entry->registered_generation != generation)) {
                complete_registration(entry, listen, generation, false);
                continue;
            }
            dto::req::ConfigListenContext context;
            context.group.set_present(entry->key.group);
            context.data_id.set_present(entry->key.data_id);
            context.tenant.set_present(config_->tenant());
            const auto snapshot = entry->watch.current();
            if (snapshot.value->state == ConfigState::Present) {
                context.md5.set_present(snapshot.value->data.md5);
            } else if (snapshot.value->state == ConfigState::NotFound) {
                context.md5.set_present("");
            }
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
        auto result = co_await connection_->request(request, pool, response);
        const bool success = result && !stopping_ && generation == active_generation_ && response.success();
        if (!success) {
            for (const EntryPtr &entry: included) {
                complete_registration(entry, listen, generation, false);
            }
            continue;
        }
        if (listen) {
            process_changed(response, generation);
        }
        for (const EntryPtr &entry: included) {
            complete_registration(entry, listen, generation, true);
        }
    }
    task_done();
}

void ConfigServiceImpl::register_all(std::uint64_t generation) {
    std::vector<EntryPtr> entries;
    entries.reserve(entries_.size());
    for (const auto &[key, entry]: entries_) {
        (void) key;
        if (entry->subscribers > 0 && !entry->closing) {
            entries.push_back(entry);
        }
    }
    schedule_registration(std::move(entries), true, generation);
}

void ConfigServiceImpl::process_changed(const dto::resp::ConfigChangeBatchListenResponse &response,
                                        std::uint64_t generation) {
    for (const dto::resp::ConfigContext &changed: response.changed_configs) {
        if (!changed.data_id.is_present() || !changed.group.is_present()) {
            continue;
        }
        if (changed.tenant.is_present() && changed.tenant.value() != config_->tenant()) {
            continue;
        }
        if (auto entry = find_entry(changed.data_id.value(), changed.group.value())) {
            schedule_query(entry, generation);
        }
    }
}

std::expected<proto::Payload, NacosRpcError> ConfigServiceImpl::handle_push(void *context,
                                                                            const proto::Payload &request,
                                                                            const NacosPayloadMetadata &metadata,
                                                                            std::size_t max_payload_bytes) noexcept {
    FIBER_ASSERT(context != nullptr);
    return static_cast<ConfigServiceImpl *>(context)->handle_push(request, metadata, max_payload_bytes);
}

std::expected<proto::Payload, NacosRpcError> ConfigServiceImpl::handle_push(const proto::Payload &request,
                                                                            const NacosPayloadMetadata &metadata,
                                                                            std::size_t max_payload_bytes) noexcept {
    auto view = validate_payload(request, max_payload_bytes);
    if (!view) {
        return std::unexpected(std::move(view.error()));
    }
    mem::BufPool pool;
    if (view->type == dto::req::ConfigChangeNotifyRequest::kTypeName) {
        dto::req::ConfigChangeNotifyRequest notify;
        auto parsed = parse_payload_json(*view, pool, notify);
        if (!parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
        if (!notify.data_id.is_present() || !notify.group.is_present()) {
            return std::unexpected(protocol_error("Nacos config change notification is missing key"));
        }
        const bool tenant_matches = !notify.tenant.is_present() || notify.tenant.value() == config_->tenant();
        if (!stopping_ && tenant_matches && active_generation_ != 0) {
            if (auto entry = find_entry(notify.data_id.value(), notify.group.value())) {
                schedule_query(entry, active_generation_);
            }
        }
        dto::resp::ConfigChangeNotifyResponse response;
        response.request_id = notify.request_id;
        return encode_payload(response, metadata, max_payload_bytes);
    }

    dto::RequestBase request_base;
    auto parsed = parse_payload_json(*view, pool, request_base);
    if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    dto::resp::ErrorResponse response;
    response.result_code = dto::kResponseFail;
    response.error_code = dto::kResponseFail;
    response.message.set_present("unsupported Nacos config server request");
    response.request_id = request_base.request_id;
    return encode_payload(response, metadata, max_payload_bytes);
}

async::Task<void> ConfigServiceImpl::run() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(!run_active_);
    run_active_ = true;
    auto states = connection_->subscribe_state();
    auto state = states.current();
    std::uint64_t state_version = state.version;
    auto redo_at = std::chrono::steady_clock::time_point::max();

    while (!stopping_) {
        if (state.value && state.value->state == NacosGrpcConnectionState::Ready &&
            state.value->generation != active_generation_) {
            active_generation_ = state.value->generation;
            register_all(active_generation_);
            redo_at = event::EventLoop::current().now() + options_->config_subscription_redo_interval;
        } else if (state.value && state.value->state != NacosGrpcConnectionState::Ready && active_generation_ != 0) {
            active_generation_ = 0;
            redo_at = std::chrono::steady_clock::time_point::max();
        }

        if (stopping_) {
            break;
        }
        if (redo_at == std::chrono::steady_clock::time_point::max()) {
            state = co_await states.next(state_version);
            state_version = state.version;
            continue;
        }

        const auto now = event::EventLoop::current().now();
        if (now >= redo_at) {
            register_all(active_generation_);
            redo_at = now + options_->config_subscription_redo_interval;
            continue;
        }
        auto next = co_await async::timeout_for([&states, state_version]() { return states.next(state_version); },
                                                redo_at - now);
        if (next) {
            state = std::move(*next);
            state_version = state.version;
        } else if (next.error() == common::IoErr::TimedOut) {
            register_all(active_generation_);
            redo_at = event::EventLoop::current().now() + options_->config_subscription_redo_interval;
        }
    }

    active_generation_ = 0;
    co_await tasks_.join();
    for (auto &[key, entry]: entries_) {
        (void) key;
        const auto snapshot = entry->watch.current();
        if (snapshot.value->state != ConfigState::Stopped) {
            ConfigSnapshot stopped = *snapshot.value;
            stopped.state = ConfigState::Stopped;
            publish_snapshot(entry, std::move(stopped));
        }
    }
    entries_.clear();
    run_active_ = false;
}

void ConfigServiceImpl::shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (stopping_) {
        return;
    }
    stopping_ = true;
    for (auto &[key, entry]: entries_) {
        (void) key;
        const auto snapshot = entry->watch.current();
        if (snapshot.value->state != ConfigState::Stopped) {
            ConfigSnapshot stopped = *snapshot.value;
            stopped.state = ConfigState::Stopped;
            publish_snapshot(entry, std::move(stopped));
        }
    }
}

void ConfigServiceImpl::task_done() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    tasks_.done();
}

} // namespace fiber::nacos::detail
