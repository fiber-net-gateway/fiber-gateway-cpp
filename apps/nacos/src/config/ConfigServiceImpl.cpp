#include "ConfigServiceImpl.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

#include <async/Timeout.h>
#include <common/Assert.h>
#include <fiber/nacos/dto/Config.h>

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

ConfigServiceError shutdown_error() {
    return ConfigServiceError{.code = ConfigServiceErrorCode::Shutdown, .io_error = common::IoErr::Canceled};
}

} // namespace

ConfigServiceImpl::ConfigServiceImpl(event::EventLoop &loop, const NacosClientConfig &config,
                                     const NacosClientOptions &options, NacosGrpcConnection &connection) :
    loop_(&loop), config_(&config), options_(&options), connection_(&connection),
    pool_([this](EntryPtr entry) { on_subscription_add(std::move(entry)); },
          [this](EntryPtr entry) { return on_subscription_remove(std::move(entry)); }) {
    connection_->set_push_handler(NacosPushHandler{
            .context = this,
            .callback = &ConfigServiceImpl::handle_push,
    });
}

ConfigServiceImpl::~ConfigServiceImpl() {
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

async::Task<std::expected<std::optional<ConfigData>, ConfigServiceError>>
ConfigServiceImpl::get_config(std::string data_id, std::string group) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (stopping_) {
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
            .state = ConfigState::Present,
            .md5 = std::string(response.md5.value()),
            .content = std::string(response.content.value()),
    });
}

async::Task<std::expected<void, ConfigServiceError>>
ConfigServiceImpl::publish(std::string data_id, std::string group, std::string content, ConfigType type,
                           std::optional<std::string> cas_md5) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (stopping_) {
        co_return std::unexpected(shutdown_error());
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
    auto result = co_await connection_->request(request, pool, response);
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
    if (stopping_ || !pool_.active()) {
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
    if (active_generation_ != 0) {
        schedule_registration({std::move(entry)}, true, active_generation_);
    }
}

RemoveDecision ConfigServiceImpl::on_subscription_remove(EntryPtr entry) {
    FIBER_ASSERT(loop_->in_loop());
    entry->proto.draining = true;
    const std::uint64_t generation = active_generation_;
    if (!stopping_ && generation != 0 &&
        (entry->proto.registered_generation == generation || entry->proto.registration_in_flight)) {
        // Registered or mid-registration: defer the free. The pool has already
        // unlinked the entry from the tree; this owning EntryPtr keeps it alive
        // while an async listen=false runs, and releases it to 0 on completion.
        schedule_registration({std::move(entry)}, false, generation);
        return RemoveDecision::Defer;
    }
    // Never registered (no connection / generation 0) or shutting down: free now.
    return RemoveDecision::UnlinkNow;
}

void ConfigServiceImpl::publish_value(ConfigEntry &entry, ConfigData value) {
    FIBER_ASSERT(loop_->in_loop());
    pool_.publish(entry, ConfigResult{.kind = ResultKind::Success, .data = std::move(value)});
}

void ConfigServiceImpl::schedule_query(const EntryPtr &entry, std::uint64_t generation) {
    FIBER_ASSERT(loop_->in_loop());
    if (stopping_ || generation == 0 || generation != active_generation_ || entry->proto.draining) {
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
    async::spawn(*loop_, [this, entry, generation, sequence]() { return query_and_sync(entry, generation, sequence); });
}

async::DetachedTask ConfigServiceImpl::query_and_sync(EntryPtr entry, std::uint64_t generation,
                                                      std::uint64_t sequence) noexcept {
    while (!stopping_ && generation != 0 && !entry->proto.draining) {
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
        auto result = co_await connection_->request(request, pool, response);

        if (!stopping_ && !entry->proto.draining && generation == active_generation_ &&
            sequence == entry->proto.query_sequence && result) {
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

        if (!entry->proto.dirty || stopping_ || entry->proto.draining || active_generation_ == 0) {
            break;
        }
        entry->proto.dirty = false;
        generation = active_generation_;
        sequence = ++entry->proto.query_sequence;
    }
    entry->proto.query_in_flight = false;
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
    async::spawn(*loop_, [this, entries = std::move(scheduled), listen, generation]() mutable {
        return register_entries(std::move(entries), listen, generation);
    });
}

void ConfigServiceImpl::complete_registration(const EntryPtr &entry, bool listen, std::uint64_t generation,
                                              bool success) {
    FIBER_ASSERT(entry->proto.registration_in_flight);
    entry->proto.registration_in_flight = false;
    if (success) {
        if (listen) {
            entry->proto.registered_generation = generation;
        } else if (entry->proto.registered_generation == generation) {
            entry->proto.registered_generation = 0;
        }
    }
    const bool dirty = std::exchange(entry->proto.registration_dirty, false);
    if (stopping_) {
        return;
    }

    // Only entries still in the tree (live subscribers) take further action. A
    // detached draining entry has no subscribers and is just awaiting its async
    // unregistration to release the owning EntryPtr to 0.
    if (entry->pool == nullptr) {
        return;
    }
    const bool wants_listen = !entry->proto.draining && active_generation_ != 0;
    if (wants_listen) {
        if ((!success && (!listen || dirty)) ||
            (success && (entry->proto.registered_generation != active_generation_ || dirty || !listen))) {
            schedule_registration({entry}, true, active_generation_);
        }
        return;
    }
    if (active_generation_ != 0 && entry->proto.registered_generation == active_generation_ &&
        (success || dirty || listen)) {
        schedule_registration({entry}, false, active_generation_);
        return;
    }
    // No further registration needed; if this entry is draining with no live
    // subscribers the pool already detached it - nothing to do here.
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
            if (stopping_ || generation != active_generation_ || (listen && entry->proto.draining) ||
                (!listen && entry->proto.registered_generation != generation)) {
                complete_registration(entry, listen, generation, false);
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
    pool_.for_each([&entries](ConfigEntry &entry) {
        if (!entry.proto.draining) {
            entries.push_back(EntryPtr(&entry));
        }
    });
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
        if (auto entry = pool_.find(changed.data_id.value(), changed.group.value())) {
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
            if (auto entry = pool_.find(notify.data_id.value(), notify.group.value())) {
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
    // Let every in-flight registration/query task (including deferred
    // unregistrations holding owning EntryPtrs) finish or short-circuit before
    // we close live subscriptions.
    co_await tasks_.join();
    // Close every still-live entry exactly once and wake its waiters with
    // ResultKind::Closed.
    pool_.close_all();
    run_active_ = false;
}

void ConfigServiceImpl::shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (stopping_) {
        return;
    }
    stopping_ = true;
    pool_.disable();
    pool_.close_all();
}

void ConfigServiceImpl::task_done() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    tasks_.done();
}

} // namespace fiber::nacos::detail
