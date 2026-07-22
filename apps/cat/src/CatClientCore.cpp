#include "CatClientCore.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <coroutine>
#include <limits>
#include <new>
#include <string>
#include <unistd.h>
#include <utility>

#include <async/Sleep.h>
#include <async/Timeout.h>
#include <common/Assert.h>
#include <common/mem/BufPool.h>
#include <common/mem/IoBufChain.h>
#include <common/util/UrlForm.h>
#include <dns/DnsResolver.h>
#include <http/ClientHttp1Exchange.h>
#include <http/Http1ClientConnection.h>
#include <http/HttpHeaders.h>
#include <net/IpAddress.h>

#include "CatAggregation.h"
#include "CatInternal.h"
#include "CatRouter.h"
#include "CatSystemMessage.h"

namespace fiber::cat::detail {

namespace {

inline constexpr std::uint64_t kBudgetUnitMask = std::numeric_limits<std::uint32_t>::max();
inline constexpr std::size_t kWriteIovCapacity = 16;

std::uint64_t pack_budget(std::uint32_t messages, std::uint32_t bytes) noexcept {
    return static_cast<std::uint64_t>(messages) << 32 | bytes;
}

std::uint32_t budget_messages(std::uint64_t budget) noexcept {
    return static_cast<std::uint32_t>(budget >> 32) & std::numeric_limits<std::int32_t>::max();
}

std::uint32_t budget_bytes(std::uint64_t budget) noexcept { return static_cast<std::uint32_t>(budget); }

std::uint64_t splitmix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

std::uint64_t sample_cutoff(double sample) noexcept {
    if (sample <= 0.0) {
        return 0;
    }
    if (sample >= 1.0) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(sample * static_cast<long double>(std::numeric_limits<std::uint64_t>::max()));
}

std::chrono::milliseconds grow_backoff(std::chrono::milliseconds current, std::chrono::milliseconds maximum) noexcept {
    FIBER_ASSERT(current <= maximum);
    return current >= maximum - current ? maximum : current + current;
}

class NotifyDrainAwaiter {
public:
    NotifyDrainAwaiter() noexcept = default;
    NotifyDrainAwaiter(const NotifyDrainAwaiter &) = delete;
    NotifyDrainAwaiter &operator=(const NotifyDrainAwaiter &) = delete;
    NotifyDrainAwaiter(NotifyDrainAwaiter &&) = delete;
    NotifyDrainAwaiter &operator=(NotifyDrainAwaiter &&) = delete;

    ~NotifyDrainAwaiter() {
        if (!loop_) {
            return;
        }
        FIBER_ASSERT(loop_->in_loop());
        if (timer_entry_.is_in_heap()) {
            loop_->cancel<NotifyDrainAwaiter, &NotifyDrainAwaiter::timer_entry_>(*this);
        }
        if (defer_entry_.is_in_queue()) {
            loop_->cancel<NotifyDrainAwaiter, &NotifyDrainAwaiter::defer_entry_>(*this);
        }
    }

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> handle) noexcept {
        loop_ = &event::EventLoop::current();
        handle_ = handle;
        loop_->post_at<NotifyDrainAwaiter, &NotifyDrainAwaiter::timer_entry_, &NotifyDrainAwaiter::on_timer>(
                loop_->now(), *this);
    }

    void await_resume() noexcept {
        FIBER_ASSERT(!handle_);
        loop_ = nullptr;
    }

private:
    static void on_timer(NotifyDrainAwaiter *awaiter) noexcept {
        awaiter->loop_
                ->post_local<NotifyDrainAwaiter, &NotifyDrainAwaiter::defer_entry_, &NotifyDrainAwaiter::on_deferred>(
                        *awaiter);
    }

    static void on_deferred(NotifyDrainAwaiter *awaiter) noexcept {
        std::coroutine_handle<> handle = std::exchange(awaiter->handle_, {});
        FIBER_ASSERT(handle);
        handle.resume();
    }

    event::EventLoop *loop_ = nullptr;
    std::coroutine_handle<> handle_{};
    event::EventLoop::TimerEntry timer_entry_{};
    event::EventLoop::DeferEntry defer_entry_{};
};

} // namespace

CatClientCore::CatClientCore(event::EventLoop &sender_loop, CatClientConfig config, CatClientOptions options,
                             dns::AddressResolver *resolver) noexcept :
    loop_(&sender_loop), config_(std::move(config)), options_(std::move(options)), resolver_(resolver),
    message_id_generator_(config_.ip()), collectors_(config_.bootstrap_collectors()) {
    sample_cutoff_.store(sample_cutoff(options_.initial_sample_rate), std::memory_order_relaxed);
    control_publisher_ = control_wake_.acquire_publisher();
    FIBER_ASSERT(control_publisher_.has_value());
    process_start_steady_ = loop_->now();
    const auto wall_millis =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count();
    process_start_wall_millis_ = wall_millis < 0 ? 0 : static_cast<std::uint64_t>(wall_millis);
}

CatClientCore::~CatClientCore() {
    FIBER_ASSERT(state() == CatClientState::Created || state() == CatClientState::Stopped);
    FIBER_ASSERT(active_submitters_.load(std::memory_order_relaxed) == 0);
    FIBER_ASSERT((outstanding_state_.load(std::memory_order_relaxed) & ~kClosedMask) == 0);
    FIBER_ASSERT(system_outstanding_state_.load(std::memory_order_relaxed) == 0);
    FIBER_ASSERT(local_head_ == nullptr);
    FIBER_ASSERT(priority_head_ == nullptr);
    for (std::size_t index = 0; index < aggregation_shard_count_; ++index) {
        delete aggregation_shards_[index];
    }
}

common::IoResult<void> CatClientCore::start() noexcept {
    if (!loop_->in_loop()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    CatClientState expected = CatClientState::Created;
    if (!state_.compare_exchange_strong(expected, CatClientState::Running, std::memory_order_acq_rel)) {
        return std::unexpected(common::IoErr::Already);
    }
    const std::uint64_t previous = outstanding_state_.fetch_and(~kClosedMask, std::memory_order_acq_rel);
    FIBER_ASSERT((previous & ~kClosedMask) == 0);
    if (state() != CatClientState::Running) {
        outstanding_state_.fetch_or(kClosedMask, std::memory_order_acq_rel);
    }
    control_done_.add();
    std::shared_ptr<CatClientCore> self = shared_from_this();
    async::spawn([self = std::move(self)]() { return self->run_control(); });
    loop_->post_at<CatClientCore, &CatClientCore::aggregate_timer_, &CatClientCore::on_aggregate_timer>(
            loop_->now() + options_.aggregation_flush_interval, *this);
    if (options_.enable_heartbeat) {
        loop_->post_at<CatClientCore, &CatClientCore::heartbeat_timer_, &CatClientCore::on_heartbeat_timer>(
                loop_->now() + options_.heartbeat_initial_delay, *this);
    }
    return {};
}

async::Task<void> CatClientCore::shutdown() noexcept {
    if (loop_->in_loop() && state() == CatClientState::Running) {
        (void) co_await detach_aggregation_shard(*loop_);
    }
    begin_stop();
    co_await control_done_.join();
}

void CatClientCore::begin_stop() noexcept {
    CatClientState current = state_.load(std::memory_order_acquire);
    for (;;) {
        if (current == CatClientState::Created) {
            if (state_.compare_exchange_weak(current, CatClientState::Stopped, std::memory_order_acq_rel)) {
                outstanding_state_.fetch_or(kClosedMask, std::memory_order_acq_rel);
                std::lock_guard lock(aggregation_mutex_);
                return;
            }
            continue;
        }
        if (current == CatClientState::Running) {
            if (state_.compare_exchange_weak(current, CatClientState::Stopping, std::memory_order_acq_rel)) {
                outstanding_state_.fetch_or(kClosedMask, std::memory_order_acq_rel);
                {
                    std::lock_guard lock(aggregation_mutex_);
                }
                notify_control();
                return;
            }
            continue;
        }
        outstanding_state_.fetch_or(kClosedMask, std::memory_order_acq_rel);
        return;
    }
}

CatClientStats CatClientCore::stats() const noexcept {
    const std::uint64_t budget = outstanding_state_.load(std::memory_order_acquire);
    const std::uint64_t system_budget = system_outstanding_state_.load(std::memory_order_acquire);
    return {
            .queued_messages = budget_messages(budget),
            .queued_bytes = budget_bytes(budget),
            .system_queued_messages = budget_messages(system_budget),
            .system_queued_bytes = budget_bytes(system_budget),
            .submitted_messages = stats_.submitted_messages.load(std::memory_order_relaxed),
            .sent_messages = stats_.sent_messages.load(std::memory_order_relaxed),
            .sent_bytes = stats_.sent_bytes.load(std::memory_order_relaxed),
            .dropped_queue_full = stats_.dropped_queue_full.load(std::memory_order_relaxed),
            .dropped_unavailable = stats_.dropped_unavailable.load(std::memory_order_relaxed),
            .dropped_sampled = stats_.dropped_sampled.load(std::memory_order_relaxed),
            .dropped_partial_frame = stats_.dropped_partial_frame.load(std::memory_order_relaxed),
            .encode_failures = stats_.encode_failures.load(std::memory_order_relaxed),
            .router_successes = stats_.router_successes.load(std::memory_order_relaxed),
            .router_failures = stats_.router_failures.load(std::memory_order_relaxed),
            .connect_successes = stats_.connect_successes.load(std::memory_order_relaxed),
            .connect_failures = stats_.connect_failures.load(std::memory_order_relaxed),
            .write_would_block = stats_.write_would_block.load(std::memory_order_relaxed),
            .write_failures = stats_.write_failures.load(std::memory_order_relaxed),
            .message_id_failures = stats_.message_id_failures.load(std::memory_order_relaxed),
            .context_failures = stats_.context_failures.load(std::memory_order_relaxed),
            .invalid_contexts = stats_.invalid_contexts.load(std::memory_order_relaxed),
            .sampled_trees = stats_.sampled_trees.load(std::memory_order_relaxed),
            .forced_problem_trees = stats_.forced_problem_trees.load(std::memory_order_relaxed),
            .aggregated_trees = stats_.aggregated_trees.load(std::memory_order_relaxed),
            .aggregation_overflow = stats_.aggregation_overflow.load(std::memory_order_relaxed),
            .aggregate_submitted = stats_.aggregate_submitted.load(std::memory_order_relaxed),
            .aggregate_dropped = stats_.aggregate_dropped.load(std::memory_order_relaxed),
            .aggregate_retry_failures = stats_.aggregate_retry_failures.load(std::memory_order_relaxed),
            .aggregate_encode_failures = stats_.aggregate_encode_failures.load(std::memory_order_relaxed),
            .metric_observations = stats_.metric_observations.load(std::memory_order_relaxed),
            .metric_overflow = stats_.metric_overflow.load(std::memory_order_relaxed),
            .metric_submitted = stats_.metric_submitted.load(std::memory_order_relaxed),
            .metric_dropped = stats_.metric_dropped.load(std::memory_order_relaxed),
            .metric_retry_failures = stats_.metric_retry_failures.load(std::memory_order_relaxed),
            .heartbeat_submitted = stats_.heartbeat_submitted.load(std::memory_order_relaxed),
            .heartbeat_sent = stats_.heartbeat_sent.load(std::memory_order_relaxed),
            .heartbeat_skipped = stats_.heartbeat_skipped.load(std::memory_order_relaxed),
            .heartbeat_dropped = stats_.heartbeat_dropped.load(std::memory_order_relaxed),
            .heartbeat_encode_failures = stats_.heartbeat_encode_failures.load(std::memory_order_relaxed),
            .heartbeat_provider_failures = stats_.heartbeat_provider_failures.load(std::memory_order_relaxed),
            .truncated_trees = stats_.truncated_trees.load(std::memory_order_relaxed),
            .truncated_messages = stats_.truncated_messages.load(std::memory_order_relaxed),
            .truncated_data_bytes = stats_.truncated_data_bytes.load(std::memory_order_relaxed),
            .router_blocks = stats_.router_blocks.load(std::memory_order_relaxed),
            .router_unblocks = stats_.router_unblocks.load(std::memory_order_relaxed),
            .router_sample_changes = stats_.router_sample_changes.load(std::memory_order_relaxed),
            .collector_set_changes = stats_.collector_set_changes.load(std::memory_order_relaxed),
            .stale_connection_switches = stats_.stale_connection_switches.load(std::memory_order_relaxed),
    };
}

ClientEncodeContext CatClientCore::encode_context() const noexcept {
    return {
            .app_key = config_.app_key(),
            .hostname = config_.hostname(),
            .ip = config_.ip(),
            .thread_group_name = config_.thread_group_name(),
            .thread_id = config_.thread_id(),
            .thread_name = config_.thread_name(),
    };
}

std::expected<mem::IoBuf, EncodeError> CatClientCore::encode(const MessageTraceData &trace) const noexcept {
    return encode_message_tree(trace, encode_context(), options_.encoder);
}

bool CatClientCore::accepts_messages() const noexcept {
    return state() == CatClientState::Running &&
           (outstanding_state_.load(std::memory_order_acquire) & kClosedMask) == 0 &&
           !blocked_.load(std::memory_order_acquire);
}

std::expected<GeneratedMessageId, RecordError> CatClientCore::create_message_id(std::string_view domain) noexcept {
    if (state() != CatClientState::Running) {
        stats_.message_id_failures.fetch_add(1, std::memory_order_relaxed);
        return std::unexpected(RecordError::Completed);
    }
    if (domain.empty()) {
        domain = config_.app_key();
    }
    auto generated = message_id_generator_.next(domain, std::chrono::system_clock::now());
    if (!generated) {
        on_context_failure(generated.error());
        if (generated.error() != RecordError::InvalidContext) {
            stats_.message_id_failures.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return generated;
}

void CatClientCore::on_context_failure(RecordError error) noexcept {
    if (error == RecordError::InvalidContext) {
        stats_.invalid_contexts.fetch_add(1, std::memory_order_relaxed);
    } else {
        stats_.context_failures.fetch_add(1, std::memory_order_relaxed);
    }
}

TraceDisposition CatClientCore::trace_disposition(bool has_problem) noexcept {
    if (!accepts_messages()) {
        stats_.dropped_unavailable.fetch_add(1, std::memory_order_relaxed);
        return TraceDisposition::Drop;
    }
    if (has_problem) {
        stats_.forced_problem_trees.fetch_add(1, std::memory_order_relaxed);
        return TraceDisposition::Problem;
    }
    if (sampled_in()) {
        return TraceDisposition::Detailed;
    }
    stats_.sampled_trees.fetch_add(1, std::memory_order_relaxed);
    return TraceDisposition::Aggregate;
}

AggregationShard *CatClientCore::aggregation_shard(event::EventLoop &owner) noexcept {
    if (state() != CatClientState::Running) {
        return nullptr;
    }
    std::lock_guard lock(aggregation_mutex_);
    if (state() != CatClientState::Running) {
        return nullptr;
    }
    for (std::size_t index = 0; index < aggregation_shard_count_; ++index) {
        if (&aggregation_shards_[index]->owner() == &owner) {
            return aggregation_shards_[index];
        }
    }
    if (aggregation_shard_count_ >= options_.max_aggregation_shards ||
        aggregation_shard_count_ >= aggregation_shards_.size()) {
        stats_.aggregation_overflow.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    AggregationShard *shard =
            AggregationShard::create(owner, options_.max_aggregate_keys_per_shard, options_.max_aggregate_key_bytes,
                                     options_.max_aggregate_bytes_per_shard, options_.max_duration_buckets_per_key);
    if (!shard) {
        stats_.aggregation_overflow.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    aggregation_shards_[aggregation_shard_count_++] = shard;
    return shard;
}

async::Task<RecordError> CatClientCore::detach_aggregation_shard(event::EventLoop &owner) noexcept {
    if (!owner.in_loop()) {
        co_return RecordError::WrongEventLoop;
    }
    AggregationShard *shard = nullptr;
    {
        std::lock_guard lock(aggregation_mutex_);
        std::size_t index = 0;
        while (index < aggregation_shard_count_ && &aggregation_shards_[index]->owner() != &owner) {
            ++index;
        }
        if (index == aggregation_shard_count_) {
            co_return RecordError::None;
        }
        shard = aggregation_shards_[index];
        for (std::size_t move = index + 1; move < aggregation_shard_count_; ++move) {
            aggregation_shards_[move - 1] = aggregation_shards_[move];
        }
        aggregation_shards_[--aggregation_shard_count_] = nullptr;
    }

    if (state() == CatClientState::Running) {
        shard->flush(*this);
    }
    shard->discard_pending(*this);
    co_await NotifyDrainAwaiter{};
    delete shard;
    co_return RecordError::None;
}

void CatClientCore::aggregate_trace(const MessageTraceData &trace) noexcept {
    if (!trace.aggregation_shard) {
        stats_.aggregation_overflow.fetch_add(trace.message_count, std::memory_order_relaxed);
        stats_.aggregate_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::size_t dropped = trace.aggregation_shard->aggregate(trace);
    stats_.aggregated_trees.fetch_add(1, std::memory_order_relaxed);
    if (dropped != 0) {
        stats_.aggregation_overflow.fetch_add(dropped, std::memory_order_relaxed);
    }
}

CatClientCore::ReserveResult CatClientCore::reserve_budget(std::size_t bytes, FramePriority priority) noexcept {
    if (bytes > options_.max_queued_bytes || bytes > kBudgetUnitMask) {
        return ReserveResult::Full;
    }
    const bool system_reserved = priority != FramePriority::System || reserve_system_budget(bytes);
    if (!system_reserved) {
        return ReserveResult::Full;
    }
    const auto bytes32 = static_cast<std::uint32_t>(bytes);
    std::uint64_t current = outstanding_state_.load(std::memory_order_relaxed);
    for (;;) {
        if (state() != CatClientState::Running || (current & kClosedMask) != 0) {
            if (priority == FramePriority::System) {
                release_system_budget(bytes);
            }
            return ReserveResult::Closed;
        }
        const std::uint32_t messages = budget_messages(current);
        const std::uint32_t queued_bytes = budget_bytes(current);
        std::size_t message_limit = options_.max_queued_messages;
        std::size_t byte_limit = options_.max_queued_bytes;
        if (priority == FramePriority::Normal) {
            if (message_limit > options_.problem_reserve_messages) {
                message_limit -= options_.problem_reserve_messages;
            }
            if (byte_limit > options_.problem_reserve_bytes) {
                byte_limit -= options_.problem_reserve_bytes;
            }
        }
        if (messages >= message_limit || bytes > byte_limit || queued_bytes > byte_limit - bytes) {
            if (priority == FramePriority::System) {
                release_system_budget(bytes);
            }
            return ReserveResult::Full;
        }
        const std::uint64_t next = pack_budget(messages + 1, queued_bytes + bytes32);
        if (outstanding_state_.compare_exchange_weak(current, next, std::memory_order_acq_rel,
                                                     std::memory_order_relaxed)) {
            return ReserveResult::Reserved;
        }
    }
}

bool CatClientCore::reserve_system_budget(std::size_t bytes) noexcept {
    if (bytes > options_.max_system_queued_bytes || bytes > kBudgetUnitMask) {
        return false;
    }
    const auto bytes32 = static_cast<std::uint32_t>(bytes);
    std::uint64_t current = system_outstanding_state_.load(std::memory_order_relaxed);
    for (;;) {
        const std::uint32_t messages = budget_messages(current);
        const std::uint32_t queued_bytes = budget_bytes(current);
        if (messages >= options_.max_system_queued_messages ||
            queued_bytes > options_.max_system_queued_bytes - bytes) {
            return false;
        }
        const std::uint64_t next = pack_budget(messages + 1, queued_bytes + bytes32);
        if (system_outstanding_state_.compare_exchange_weak(current, next, std::memory_order_acq_rel,
                                                            std::memory_order_relaxed)) {
            return true;
        }
    }
}

void CatClientCore::release_budget(std::size_t bytes, FramePriority priority) noexcept {
    FIBER_ASSERT(bytes <= kBudgetUnitMask);
    const std::uint64_t delta = pack_budget(1, static_cast<std::uint32_t>(bytes));
    const std::uint64_t previous = outstanding_state_.fetch_sub(delta, std::memory_order_acq_rel);
    FIBER_ASSERT(budget_messages(previous) > 0);
    FIBER_ASSERT(budget_bytes(previous) >= bytes);
    if (priority == FramePriority::System) {
        release_system_budget(bytes);
    }
}

void CatClientCore::release_system_budget(std::size_t bytes) noexcept {
    const std::uint64_t delta = pack_budget(1, static_cast<std::uint32_t>(bytes));
    const std::uint64_t previous = system_outstanding_state_.fetch_sub(delta, std::memory_order_acq_rel);
    FIBER_ASSERT(budget_messages(previous) > 0);
    FIBER_ASSERT(budget_bytes(previous) >= bytes);
}

bool CatClientCore::sampled_in() noexcept {
    const std::uint64_t cutoff = sample_cutoff_.load(std::memory_order_acquire);
    if (cutoff == 0) {
        return false;
    }
    if (cutoff == std::numeric_limits<std::uint64_t>::max()) {
        return true;
    }
    const std::uint64_t sequence = sample_sequence_.fetch_add(1, std::memory_order_relaxed);
    return splitmix64(sequence) <= cutoff;
}

void CatClientCore::request_aggregate_flushes() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    std::shared_ptr<CatClientCore> self = shared_from_this();
    std::lock_guard lock(aggregation_mutex_);
    for (std::size_t index = 0; index < aggregation_shard_count_; ++index) {
        aggregation_shards_[index]->request_flush(self);
    }
}

void CatClientCore::on_aggregate_timer(CatClientCore *client) noexcept {
    if (client->state() != CatClientState::Running) {
        return;
    }
    client->request_aggregate_flushes();
    client->loop_->post_at<CatClientCore, &CatClientCore::aggregate_timer_, &CatClientCore::on_aggregate_timer>(
            client->loop_->now() + client->options_.aggregation_flush_interval, *client);
}

void CatClientCore::on_heartbeat_timer(CatClientCore *client) noexcept {
    if (client->state() != CatClientState::Running) {
        return;
    }
    if (!client->startup_submitted_) {
        client->submit_startup();
    }
    client->submit_heartbeat();
    client->loop_->post_at<CatClientCore, &CatClientCore::heartbeat_timer_, &CatClientCore::on_heartbeat_timer>(
            client->loop_->now() + client->options_.heartbeat_interval, *client);
}

void CatClientCore::submit_startup() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (blocked_.load(std::memory_order_acquire)) {
        return;
    }
    auto id = create_message_id();
    if (!id) {
        return;
    }
    auto encoded = encode_startup_nt1(encode_context(), id->view(), config_.ip(), "fiber2-cat/1.0", options_.encoder);
    if (!encoded) {
        stats_.encode_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    startup_submitted_ = submit_encoded(std::move(*encoded), FramePriority::System, FrameCategory::Startup) ==
                         SubmitResult::Submitted;
}

void CatClientCore::submit_heartbeat() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (blocked_.load(std::memory_order_acquire) || heartbeat_outstanding_) {
        stats_.heartbeat_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto id = create_message_id();
    if (!id) {
        stats_.heartbeat_encode_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::size_t event_loop_count = 0;
    {
        std::lock_guard lock(aggregation_mutex_);
        event_loop_count = aggregation_shard_count_;
    }
    const auto uptime = std::chrono::duration_cast<std::chrono::milliseconds>(loop_->now() - process_start_steady_);
    HeartbeatInfo info{
            .app_key = config_.app_key(),
            .hostname = config_.hostname(),
            .ip = config_.ip(),
            .client_version = "fiber2-cat/1.0",
            .process_id = static_cast<std::uint64_t>(::getpid()),
            .process_start_millis = process_start_wall_millis_,
            .uptime_millis = uptime.count() < 0 ? 0 : static_cast<std::uint64_t>(uptime.count()),
            .event_loop_count = event_loop_count,
            .collector_count = collectors_.size(),
            .router_last_success_millis = router_last_success_millis_,
            .sample_cutoff = sample_cutoff_.load(std::memory_order_acquire),
            .collector_connected = stream_ != nullptr,
            .blocked = blocked_.load(std::memory_order_acquire),
            .stats = stats(),
    };
    auto encoded = encode_heartbeat_nt1(encode_context(), id->view(), info, options_.max_heartbeat_fields,
                                        options_.max_heartbeat_data_bytes, options_.encoder);
    if (!encoded) {
        stats_.heartbeat_encode_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    heartbeat_outstanding_ = true;
    const SubmitResult submitted = submit_encoded(std::move(*encoded), FramePriority::System, FrameCategory::Heartbeat);
    if (submitted == SubmitResult::Submitted) {
        stats_.heartbeat_submitted.fetch_add(1, std::memory_order_relaxed);
    } else {
        heartbeat_outstanding_ = false;
        stats_.heartbeat_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

void CatClientCore::frame_finished(FrameCategory category, bool sent) noexcept {
    if (category != FrameCategory::Heartbeat) {
        return;
    }
    FIBER_ASSERT(loop_->in_loop());
    heartbeat_outstanding_ = false;
    if (sent) {
        stats_.heartbeat_sent.fetch_add(1, std::memory_order_relaxed);
    } else {
        stats_.heartbeat_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

SubmitResult CatClientCore::submit_encoded(mem::IoBuf message, FramePriority priority,
                                           FrameCategory category) noexcept {
    active_submitters_.fetch_add(1, std::memory_order_acq_rel);
    if ((outstanding_state_.load(std::memory_order_acquire) & kClosedMask) != 0 ||
        blocked_.load(std::memory_order_acquire)) {
        stats_.dropped_unavailable.fetch_add(1, std::memory_order_relaxed);
        active_submitters_.fetch_sub(1, std::memory_order_acq_rel);
        return SubmitResult::Unavailable;
    }

    const std::size_t bytes = message.readable();
    if (bytes == 0) {
        stats_.dropped_queue_full.fetch_add(1, std::memory_order_relaxed);
        active_submitters_.fetch_sub(1, std::memory_order_acq_rel);
        return SubmitResult::Invalid;
    }
    const ReserveResult reserved = reserve_budget(bytes, priority);
    if (reserved != ReserveResult::Reserved) {
        if (reserved == ReserveResult::Full) {
            stats_.dropped_queue_full.fetch_add(1, std::memory_order_relaxed);
        } else {
            stats_.dropped_unavailable.fetch_add(1, std::memory_order_relaxed);
        }
        active_submitters_.fetch_sub(1, std::memory_order_acq_rel);
        return reserved == ReserveResult::Full ? SubmitResult::Full : SubmitResult::Unavailable;
    }
    auto *frame = new (std::nothrow) OutboundFrame(*this, std::move(message), priority, category);
    if (!frame) {
        release_budget(bytes, priority);
        stats_.dropped_unavailable.fetch_add(1, std::memory_order_relaxed);
        active_submitters_.fetch_sub(1, std::memory_order_acq_rel);
        return SubmitResult::Unavailable;
    }
    if (loop_->in_loop()) {
        handle_frame_notify(frame);
    } else {
        loop_->post<OutboundFrame, &OutboundFrame::notify_entry, &OutboundFrame::on_notify>(*frame);
    }
    stats_.submitted_messages.fetch_add(1, std::memory_order_relaxed);
    active_submitters_.fetch_sub(1, std::memory_order_acq_rel);
    return SubmitResult::Submitted;
}

bool CatClientCore::submit_aggregate(mem::IoBuf message) noexcept {
    const SubmitResult result = submit_encoded(std::move(message), FramePriority::System, FrameCategory::Aggregate);
    if (result == SubmitResult::Submitted) {
        stats_.aggregate_submitted.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    stats_.aggregate_retry_failures.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool CatClientCore::submit_metric_aggregate(mem::IoBuf message) noexcept {
    const SubmitResult result = submit_encoded(std::move(message), FramePriority::System, FrameCategory::Metric);
    if (result == SubmitResult::Submitted) {
        stats_.metric_submitted.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    stats_.metric_retry_failures.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void CatClientCore::on_aggregate_encode_failure() noexcept {
    stats_.aggregate_encode_failures.fetch_add(1, std::memory_order_relaxed);
}

void CatClientCore::on_aggregate_drop(std::size_t count) noexcept {
    stats_.aggregate_dropped.fetch_add(count, std::memory_order_relaxed);
}

void CatClientCore::on_metric_drop(std::size_t count) noexcept {
    stats_.metric_dropped.fetch_add(count, std::memory_order_relaxed);
}

void CatClientCore::on_metric_observation(RecordError result) noexcept {
    if (result == RecordError::None) {
        stats_.metric_observations.fetch_add(1, std::memory_order_relaxed);
    } else if (result == RecordError::LimitExceeded || result == RecordError::NoMemory) {
        stats_.metric_overflow.fetch_add(1, std::memory_order_relaxed);
    }
}

void CatClientCore::on_trace_truncated(const MessageTraceData &trace) noexcept {
    stats_.truncated_trees.fetch_add(1, std::memory_order_relaxed);
    stats_.truncated_messages.fetch_add(trace.dropped_message_count, std::memory_order_relaxed);
    stats_.truncated_data_bytes.fetch_add(trace.dropped_data_bytes, std::memory_order_relaxed);
}

void CatClientCore::on_encode_failure(EncodeError /*error*/) noexcept {
    stats_.encode_failures.fetch_add(1, std::memory_order_relaxed);
}

void CatClientCore::OutboundFrame::on_notify(OutboundFrame *frame) noexcept {
    FIBER_ASSERT(frame);
    FIBER_ASSERT(frame->core);
    frame->core->handle_frame_notify(frame);
}

void CatClientCore::handle_frame_notify(OutboundFrame *frame) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(frame);
    FIBER_ASSERT(frame->core == this);
    if (blocked_.load(std::memory_order_acquire)) {
        drop_detached_frame(frame);
        return;
    }
    append_local(frame);
    schedule_pump();
}

void CatClientCore::append_local(OutboundFrame *frame) noexcept {
    FIBER_ASSERT(frame);
    FIBER_ASSERT(frame->local_next == nullptr);
    OutboundFrame *&head = frame->priority == FramePriority::Normal ? local_head_ : priority_head_;
    OutboundFrame *&tail = frame->priority == FramePriority::Normal ? local_tail_ : priority_tail_;
    if (tail) {
        tail->local_next = frame;
    } else {
        head = frame;
    }
    tail = frame;
}

void CatClientCore::schedule_pump() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    loop_->post_local<CatClientCore, &CatClientCore::pump_defer_entry_, &CatClientCore::on_pump_deferred>(*this);
}

void CatClientCore::on_pump_deferred(CatClientCore *client) noexcept {
    if (client->blocked_.load(std::memory_order_acquire)) {
        client->drop_all_frames();
        return;
    }
    client->drive_write();
    if (!client->stream_ && client->has_local_frames() && client->state() == CatClientState::Running) {
        client->notify_control();
    }
}

void CatClientCore::drive_write() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (!stream_ || !has_local_frames() || write_callback_armed_) {
        return;
    }

    std::size_t pump_bytes = 0;
    std::size_t pump_calls = 0;
    while (stream_ && has_local_frames() && pump_bytes < options_.max_send_bytes_per_pump &&
           pump_calls < options_.max_send_calls_per_pump) {
        std::array<iovec, kWriteIovCapacity> iov{};
        std::size_t count = 0;
        std::size_t batch_bytes = 0;
        const std::size_t message_limit = connection_stale_ ? 1 : options_.max_batch_messages;
        for (OutboundFrame *frame = front_frame(); frame && count < message_limit; frame = frame->local_next) {
            const std::size_t readable = frame->message.readable();
            if (count != 0 &&
                (batch_bytes >= options_.max_batch_bytes || readable > options_.max_batch_bytes - batch_bytes)) {
                break;
            }
            iov[count].iov_base = frame->message.readable_data();
            iov[count].iov_len = readable;
            batch_bytes += readable;
            ++count;
        }
        FIBER_ASSERT(count > 0);

        auto written = stream_->try_writev(iov.data(), static_cast<int>(count));
        ++pump_calls;
        if (!written) {
            if (written.error() == common::IoErr::WouldBlock) {
                stats_.write_would_block.fetch_add(1, std::memory_order_relaxed);
                arm_write_wait();
            } else {
                fail_connection(written.error());
            }
            return;
        }
        if (*written == 0) {
            fail_connection(common::IoErr::BrokenPipe);
            return;
        }
        pump_bytes += *written;
        consume_written(*written);
    }

    if (stream_ && has_local_frames() && !write_callback_armed_) {
        schedule_pump();
    }
}

void CatClientCore::consume_written(std::size_t bytes) noexcept {
    stats_.sent_bytes.fetch_add(bytes, std::memory_order_relaxed);
    std::size_t remaining = bytes;
    while (remaining > 0) {
        const bool priority = priority_front_selected();
        OutboundFrame *&head = priority ? priority_head_ : local_head_;
        OutboundFrame *&tail = priority ? priority_tail_ : local_tail_;
        FIBER_ASSERT(head);
        const std::size_t readable = head->message.readable();
        const std::size_t consumed = std::min(readable, remaining);
        head->message.consume(consumed);
        remaining -= consumed;
        if (consumed != readable) {
            break;
        }

        OutboundFrame *completed = head;
        head = completed->local_next;
        if (!head) {
            tail = nullptr;
        }
        release_budget(completed->original_size, completed->priority);
        stats_.sent_messages.fetch_add(1, std::memory_order_relaxed);
        frame_finished(completed->category, true);
        delete completed;
        if (connection_stale_) {
            FIBER_ASSERT(remaining == 0);
            close_connection();
            stats_.stale_connection_switches.fetch_add(1, std::memory_order_relaxed);
            notify_control();
            return;
        }
    }
}

void CatClientCore::arm_write_wait() noexcept {
    FIBER_ASSERT(stream_);
    FIBER_ASSERT(!write_callback_armed_);
    const common::IoErr result = stream_->set_write_callback(&CatClientCore::on_write_ready, this);
    if (result != common::IoErr::None) {
        fail_connection(result);
        return;
    }
    write_callback_armed_ = true;
    loop_->post_at<CatClientCore, &CatClientCore::write_timer_, &CatClientCore::on_write_timeout>(
            loop_->now() + options_.collector_write_timeout, *this);
}

void CatClientCore::clear_write_wait() noexcept {
    if (write_timer_.is_in_heap()) {
        loop_->cancel<CatClientCore, &CatClientCore::write_timer_>(*this);
    }
    if (write_callback_armed_ && stream_) {
        (void) stream_->clear_write_callback(&CatClientCore::on_write_ready, this);
    }
    write_callback_armed_ = false;
}

void CatClientCore::on_write_ready(void *ctx, common::IoErr error) noexcept {
    auto *client = static_cast<CatClientCore *>(ctx);
    client->clear_write_wait();
    if (error != common::IoErr::None) {
        client->fail_connection(error);
        return;
    }
    client->drive_write();
}

void CatClientCore::on_write_timeout(CatClientCore *client) noexcept {
    client->clear_write_wait();
    client->fail_connection(common::IoErr::TimedOut);
}

void CatClientCore::fail_connection(common::IoErr /*error*/) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    stats_.write_failures.fetch_add(1, std::memory_order_relaxed);
    if (front_frame() && front_frame()->message.readable() != front_frame()->original_size) {
        drop_front_frame(true);
    }
    close_connection();
    notify_control();
}

void CatClientCore::install_connection(std::unique_ptr<net::TcpStream> stream) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    close_connection();
    stream_ = std::move(stream);
    connection_stale_ = false;
    schedule_pump();
}

void CatClientCore::close_connection() noexcept {
    clear_write_wait();
    if (stream_) {
        stream_->close();
        stream_.reset();
    }
    connection_stale_ = false;
}

void CatClientCore::drop_detached_frame(OutboundFrame *frame) noexcept {
    FIBER_ASSERT(frame);
    FIBER_ASSERT(frame->core == this);
    FIBER_ASSERT(frame->local_next == nullptr);
    release_budget(frame->original_size, frame->priority);
    stats_.dropped_unavailable.fetch_add(1, std::memory_order_relaxed);
    frame_finished(frame->category, false);
    delete frame;
}

void CatClientCore::drop_front_frame(bool partial) noexcept {
    const bool priority = priority_front_selected();
    OutboundFrame *&head = priority ? priority_head_ : local_head_;
    OutboundFrame *&tail = priority ? priority_tail_ : local_tail_;
    FIBER_ASSERT(head);
    OutboundFrame *dropped = head;
    head = dropped->local_next;
    if (!head) {
        tail = nullptr;
    }
    release_budget(dropped->original_size, dropped->priority);
    if (partial) {
        stats_.dropped_partial_frame.fetch_add(1, std::memory_order_relaxed);
    } else {
        stats_.dropped_unavailable.fetch_add(1, std::memory_order_relaxed);
    }
    frame_finished(dropped->category, false);
    delete dropped;
}

void CatClientCore::drop_all_frames() noexcept {
    while (has_local_frames()) {
        drop_front_frame(front_frame()->message.readable() != front_frame()->original_size);
    }
}

void CatClientCore::notify_control() noexcept {
    const std::uint64_t generation = control_generation_.fetch_add(1, std::memory_order_relaxed) + 1;
    control_publisher_->publish(generation);
}

async::Task<void> CatClientCore::wait_control(std::chrono::steady_clock::duration delay,
                                              async::Watch<std::uint64_t>::Subscriber &wake,
                                              std::uint64_t &version) noexcept {
    if (delay <= std::chrono::steady_clock::duration::zero()) {
        co_return;
    }
    auto result = co_await async::timeout_for([&wake, version]() { return wake.next(version); }, delay);
    if (result) {
        version = result->version;
    } else {
        FIBER_ASSERT(result.error() == common::IoErr::TimedOut);
    }
}

async::DetachedTask CatClientCore::run_control() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    auto wake = control_wake_.subscribe();
    std::uint64_t wake_version = wake.current().version;
    auto reconnect_delay = options_.reconnect_initial_delay;
    auto next_connect_at = loop_->now();
    auto next_router_at = config_.routers().empty() ? std::chrono::steady_clock::time_point::max() : loop_->now();

    while (state() == CatClientState::Running) {
        auto now = loop_->now();
        if (!config_.routers().empty() && now >= next_router_at) {
            const bool refreshed = co_await refresh_router();
            now = loop_->now();
            next_router_at = now + (refreshed ? options_.router_refresh_interval : options_.reconnect_max_delay);
            if (refreshed) {
                reconnect_delay = options_.reconnect_initial_delay;
                next_connect_at = now;
            }
        }
        if (state() != CatClientState::Running) {
            break;
        }

        if (!blocked_.load(std::memory_order_acquire) && !stream_ && !collectors_.empty() && now >= next_connect_at) {
            const bool connected = co_await connect_collector();
            now = loop_->now();
            if (connected) {
                reconnect_delay = options_.reconnect_initial_delay;
            } else {
                next_connect_at = now + reconnect_delay;
                reconnect_delay = grow_backoff(reconnect_delay, options_.reconnect_max_delay);
            }
        }
        if (state() != CatClientState::Running) {
            break;
        }

        now = loop_->now();
        auto next_action = next_router_at;
        if (!blocked_.load(std::memory_order_acquire) && !stream_ && !collectors_.empty()) {
            next_action = std::min(next_action, next_connect_at);
        }
        if (next_action == std::chrono::steady_clock::time_point::max()) {
            next_action = now + options_.router_refresh_interval;
        }
        co_await wait_control(std::max(next_action - now, std::chrono::steady_clock::duration::zero()), wake,
                              wake_version);
    }

    co_await finish_shutdown();
    state_.store(CatClientState::Stopped, std::memory_order_release);
    control_done_.done();
}

async::Task<std::optional<std::vector<net::SocketAddress>>>
CatClientCore::resolve_endpoint(std::string_view host, std::uint16_t port) noexcept {
    net::IpAddress literal;
    if (net::IpAddress::parse(host, literal) && !literal.is_unspecified()) {
        std::vector<net::SocketAddress> result;
        result.emplace_back(literal, port);
        co_return result;
    }
    if (!resolver_ || !resolver_->valid()) {
        co_return std::optional<std::vector<net::SocketAddress>>{};
    }
    dns::EndpointResolveResult resolved;
    if (!resolved.init({.max_records = 16, .max_name_storage = 512})) {
        co_return std::optional<std::vector<net::SocketAddress>>{};
    }
    auto status = co_await resolver_->resolve(host, port, resolved);
    if (!status || *status != dns::ResolveStatus::Success || resolved.record_count() == 0) {
        co_return std::optional<std::vector<net::SocketAddress>>{};
    }
    std::vector<net::SocketAddress> result;
    result.reserve(resolved.record_count());
    for (std::uint16_t index = 0; index < resolved.record_count(); ++index) {
        result.push_back(resolved.records()[index]);
    }
    co_return result;
}

async::Task<std::optional<std::string>> CatClientCore::fetch_router_body(const CatRouterEndpoint &router) noexcept {
    auto endpoints = co_await resolve_endpoint(router.host, router.port);
    if (!endpoints) {
        co_return std::optional<std::string>{};
    }

    std::string target = "/cat/s/router?op=json&domain=";
    util::form_encode(config_.app_key(), target);
    target.append("&ip=");
    util::form_encode(config_.ip(), target);
    target.append("&hostname=");
    util::form_encode(config_.hostname(), target);

    net::IpAddress router_literal;
    const bool router_is_v6 = net::IpAddress::parse(router.host, router_literal) && router_literal.is_v6();
    std::string host_header;
    if (router_is_v6) {
        host_header.push_back('[');
        host_header.append(router.host);
        host_header.push_back(']');
    } else {
        host_header = router.host;
    }
    if (router.port != 80) {
        host_header.push_back(':');
        host_header.append(std::to_string(router.port));
    }

    for (const net::SocketAddress &endpoint: *endpoints) {
        http::Http1ClientConnectionOptions connection_options;
        connection_options.peer_addr = endpoint;
        http::Http1ClientConnection connection(*loop_, std::move(connection_options));
        auto connected = co_await connection.connect(options_.router_connect_timeout);
        if (!connected) {
            continue;
        }

        std::optional<std::string> body;
        {
            mem::BufPool pool;
            http::HttpHeaders headers(pool);
            if (!headers.add_view("host", host_header) || !headers.add_view("connection", "close")) {
                connection.close();
                co_return std::optional<std::string>{};
            }
            http::ClientHttp1Exchange exchange(connection, pool);
            http::Http1RequestHead request{
                    .method = http::HttpMethod::Get,
                    .target = target,
                    .headers = &headers,
                    .body = http::HttpBodySpec::None(),
            };
            auto sent = co_await exchange.send_header(request, true, options_.router_request_timeout);
            if (sent) {
                auto response = co_await exchange.read_header(options_.router_request_timeout);
                if (response && (*response)->status_code == 200) {
                    std::string collected;
                    bool failed = false;
                    for (;;) {
                        const std::size_t remaining = collected.size() <= options_.max_router_response_bytes
                                                              ? options_.max_router_response_bytes - collected.size()
                                                              : 0;
                        auto chunk = co_await exchange.read_body(std::min<std::size_t>(remaining + 1, 16 * 1024),
                                                                 options_.router_request_timeout);
                        if (!chunk) {
                            failed = true;
                            break;
                        }
                        const bool complete = chunk->complete();
                        while (auto *front = chunk->front()) {
                            if (front->readable() == 0) {
                                chunk->drop_empty_front();
                                continue;
                            }
                            if (front->readable() >
                                options_.max_router_response_bytes -
                                        std::min(collected.size(), options_.max_router_response_bytes)) {
                                failed = true;
                                break;
                            }
                            collected.append(reinterpret_cast<const char *>(front->readable_data()), front->readable());
                            chunk->consume_and_compact(front->readable());
                        }
                        if (failed || collected.size() > options_.max_router_response_bytes) {
                            failed = true;
                            break;
                        }
                        if (complete) {
                            break;
                        }
                    }
                    if (!failed) {
                        body.emplace(std::move(collected));
                    } else {
                        (void) exchange.abort(common::IoErr::MessageTooLarge);
                    }
                } else if (response) {
                    (void) co_await exchange.discard_response_body(options_.router_request_timeout);
                }
            }
        }
        connection.close();
        if (body) {
            co_return body;
        }
    }
    co_return std::optional<std::string>{};
}

async::Task<bool> CatClientCore::refresh_router() noexcept {
    const std::size_t count = config_.routers().size();
    for (std::size_t offset = 0; offset < count && state() == CatClientState::Running; ++offset) {
        const std::size_t index = (router_index_ + offset) % count;
        auto body = co_await fetch_router_body(config_.routers()[index]);
        if (!body) {
            continue;
        }
        auto snapshot = parse_router_response(*body, options_.max_collectors);
        if (!snapshot) {
            continue;
        }

        const auto same_address = [](const net::SocketAddress &left, const net::SocketAddress &right) noexcept {
            return left.ip() == right.ip() && left.port() == right.port();
        };
        bool collector_set_changed = collectors_.size() != snapshot->collectors.size();
        if (!collector_set_changed) {
            for (std::size_t collector = 0; collector < collectors_.size(); ++collector) {
                if (!same_address(collectors_[collector], snapshot->collectors[collector])) {
                    collector_set_changed = true;
                    break;
                }
            }
        }
        std::optional<std::size_t> current_collector;
        if (stream_) {
            for (std::size_t collector = 0; collector < snapshot->collectors.size(); ++collector) {
                if (same_address(stream_->remote_addr(), snapshot->collectors[collector])) {
                    current_collector = collector;
                    break;
                }
            }
        }

        const bool was_blocked = blocked_.load(std::memory_order_acquire);
        const std::uint64_t previous_cutoff = sample_cutoff_.load(std::memory_order_acquire);
        const std::uint64_t next_cutoff = sample_cutoff(snapshot->sample);
        router_index_ = (index + 1) % count;
        collectors_ = std::move(snapshot->collectors);
        collector_index_ = current_collector ? (*current_collector + 1) % collectors_.size() : 0;
        sample_cutoff_.store(next_cutoff, std::memory_order_release);
        blocked_.store(snapshot->block, std::memory_order_release);
        if (collector_set_changed) {
            stats_.collector_set_changes.fetch_add(1, std::memory_order_relaxed);
        }
        if (previous_cutoff != next_cutoff) {
            stats_.router_sample_changes.fetch_add(1, std::memory_order_relaxed);
        }
        if (!was_blocked && snapshot->block) {
            stats_.router_blocks.fetch_add(1, std::memory_order_relaxed);
        } else if (was_blocked && !snapshot->block) {
            stats_.router_unblocks.fetch_add(1, std::memory_order_relaxed);
        }
        if (snapshot->block) {
            close_connection();
            drop_all_frames();
        } else if (stream_ && !current_collector) {
            OutboundFrame *front = front_frame();
            if (front && front->message.readable() != front->original_size) {
                connection_stale_ = true;
            } else {
                close_connection();
                stats_.stale_connection_switches.fetch_add(1, std::memory_order_relaxed);
                notify_control();
            }
        }
        stats_.router_successes.fetch_add(1, std::memory_order_relaxed);
        const auto wall_millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count();
        router_last_success_millis_ = wall_millis < 0 ? 0 : static_cast<std::uint64_t>(wall_millis);
        co_return true;
    }
    stats_.router_failures.fetch_add(1, std::memory_order_relaxed);
    co_return false;
}

async::Task<bool> CatClientCore::connect_collector() noexcept {
    const std::size_t count = collectors_.size();
    for (std::size_t offset = 0; offset < count && state() == CatClientState::Running; ++offset) {
        const std::size_t index = (collector_index_ + offset) % count;
        auto connected =
                co_await net::TcpStream::connect(*loop_, collectors_[index], options_.collector_connect_timeout);
        if (!connected) {
            stats_.connect_failures.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        auto *raw_stream = new (std::nothrow) net::TcpStream(std::move(*connected));
        if (!raw_stream) {
            stats_.connect_failures.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        std::unique_ptr<net::TcpStream> stream(raw_stream);
        const common::IoErr configured = stream->apply_socket_options(options_.collector_tcp);
        if (configured != common::IoErr::None) {
            stream->close();
            stats_.connect_failures.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        collector_index_ = (index + 1) % count;
        install_connection(std::move(stream));
        stats_.connect_successes.fetch_add(1, std::memory_order_relaxed);
        co_return true;
    }
    co_return false;
}

async::Task<void> CatClientCore::finish_shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (aggregate_timer_.is_in_heap()) {
        loop_->cancel<CatClientCore, &CatClientCore::aggregate_timer_>(*this);
    }
    if (heartbeat_timer_.is_in_heap()) {
        loop_->cancel<CatClientCore, &CatClientCore::heartbeat_timer_>(*this);
    }
    while (active_submitters_.load(std::memory_order_acquire) != 0) {
        co_await async::sleep(std::chrono::milliseconds(1));
    }
    co_await NotifyDrainAwaiter{};

    const auto deadline = loop_->now() + options_.shutdown_drain_timeout;
    while ((outstanding_state_.load(std::memory_order_acquire) & ~kClosedMask) != 0 && loop_->now() < deadline) {
        if (stream_ && !write_callback_armed_) {
            schedule_pump();
        }
        co_await async::sleep(std::chrono::milliseconds(1));
    }

    loop_->cancel<CatClientCore, &CatClientCore::pump_defer_entry_>(*this);
    close_connection();
    drop_all_frames();
    FIBER_ASSERT((outstanding_state_.load(std::memory_order_acquire) & ~kClosedMask) == 0);
}

} // namespace fiber::cat::detail
