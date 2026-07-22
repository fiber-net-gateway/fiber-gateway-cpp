#include <fiber/cat/CatClient.h>

#include <cmath>
#include <limits>
#include <new>
#include <utility>

#include <dns/DnsResolver.h>
#include <net/IpAddress.h>

#include "CatClientCore.h"

namespace fiber::cat {

namespace {

bool valid_options(const CatClientOptions &options) noexcept {
    return std::isfinite(options.initial_sample_rate) && options.initial_sample_rate >= 0.0 &&
           options.initial_sample_rate <= 1.0 && options.max_queued_messages > 0 &&
           options.max_queued_messages <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) &&
           options.max_queued_bytes > 0 && options.max_queued_bytes <= std::numeric_limits<std::uint32_t>::max() &&
           options.max_router_response_bytes > 0 && options.max_collectors > 0 && options.max_batch_messages > 0 &&
           options.max_batch_messages <= 16 && options.max_batch_bytes > 0 && options.max_send_bytes_per_pump > 0 &&
           options.max_send_calls_per_pump > 0 && options.router_connect_timeout > std::chrono::milliseconds::zero() &&
           options.max_aggregation_shards > 0 && options.max_aggregation_shards <= 64 &&
           options.max_aggregate_keys_per_shard > 0 && options.max_aggregate_key_bytes > 0 &&
           options.max_aggregate_bytes_per_shard > 0 && options.max_duration_buckets_per_key > 0 &&
           options.max_system_queued_messages > 0 &&
           options.max_system_queued_messages <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) &&
           options.max_system_queued_bytes > 0 &&
           options.max_system_queued_bytes <= std::numeric_limits<std::uint32_t>::max() &&
           options.max_heartbeat_data_bytes > 0 && options.max_heartbeat_data_bytes <= 64 * 1024 &&
           options.max_heartbeat_fields > 0 && options.max_heartbeat_fields <= 128 &&
           options.router_request_timeout > std::chrono::milliseconds::zero() &&
           options.router_refresh_interval > std::chrono::milliseconds::zero() &&
           options.collector_connect_timeout > std::chrono::milliseconds::zero() &&
           options.collector_write_timeout > std::chrono::milliseconds::zero() &&
           options.reconnect_initial_delay > std::chrono::milliseconds::zero() &&
           options.reconnect_max_delay >= options.reconnect_initial_delay &&
           options.shutdown_drain_timeout >= std::chrono::milliseconds::zero() &&
           options.aggregation_flush_interval > std::chrono::milliseconds::zero() &&
           options.heartbeat_interval > std::chrono::milliseconds::zero() &&
           options.heartbeat_initial_delay >= std::chrono::milliseconds::zero();
}

bool needs_resolver(const CatClientConfig &config) noexcept {
    for (const CatRouterEndpoint &router: config.routers()) {
        net::IpAddress literal;
        if (!net::IpAddress::parse(router.host, literal)) {
            return true;
        }
    }
    return false;
}

} // namespace

std::expected<std::unique_ptr<CatClient>, CatClientCreateError>
CatClient::create(event::EventLoop &sender_loop, CatClientConfig config, CatClientOptions options,
                  dns::AddressResolver *resolver) noexcept {
    if (!valid_options(options)) {
        return std::unexpected(CatClientCreateError::InvalidOptions);
    }
    if (needs_resolver(config) && (!resolver || !resolver->valid() || &resolver->loop() != &sender_loop)) {
        return std::unexpected(CatClientCreateError::InvalidResolver);
    }

    auto *raw_core =
            new (std::nothrow) detail::CatClientCore(sender_loop, std::move(config), std::move(options), resolver);
    if (!raw_core) {
        return std::unexpected(CatClientCreateError::NoMemory);
    }
    std::shared_ptr<detail::CatClientCore> core(raw_core);
    auto *raw_client = new (std::nothrow) CatClient(std::move(core));
    if (!raw_client) {
        return std::unexpected(CatClientCreateError::NoMemory);
    }
    return std::unique_ptr<CatClient>(raw_client);
}

CatClient::~CatClient() {
    if (core_) {
        core_->begin_stop();
    }
}

common::IoResult<void> CatClient::start() noexcept { return core_->start(); }

async::Task<void> CatClient::shutdown() noexcept { co_await core_->shutdown(); }

async::Task<RecordError> CatClient::detach_current_event_loop() noexcept {
    event::EventLoop *loop = event::EventLoop::current_or_null();
    if (!core_ || !loop) {
        co_return RecordError::WrongEventLoop;
    }
    co_return co_await core_->detach_aggregation_shard(*loop);
}

CatClientState CatClient::state() const noexcept { return core_->state(); }

CatClientStats CatClient::stats() const noexcept { return core_->stats(); }

event::EventLoop &CatClient::sender_loop() const noexcept { return core_->sender_loop(); }

std::expected<PropagationContext, RecordError>
CatClient::create_remote_context(const PropagationContext &current, std::string_view remote_domain) noexcept {
    if (!core_ || !current.valid() || current.message_id().empty()) {
        if (core_) {
            core_->on_context_failure(RecordError::InvalidContext);
        }
        return std::unexpected(RecordError::InvalidContext);
    }
    auto child_id = core_->create_message_id(remote_domain);
    if (!child_id) {
        return std::unexpected(child_id.error());
    }
    const std::string_view root = current.root_message_id().empty() ? current.message_id() : current.root_message_id();
    auto result = PropagationContext::create({
            .message_id = child_id->view(),
            .root_message_id = root,
            .parent_message_id = current.message_id(),
            .session_token = current.session_token(),
    });
    if (!result) {
        core_->on_context_failure(result.error());
    }
    return result;
}

std::shared_ptr<detail::CatClientCore> CatClient::core() const noexcept { return core_; }

} // namespace fiber::cat
