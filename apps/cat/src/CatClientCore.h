#ifndef FIBER_CAT_CLIENT_CORE_H
#define FIBER_CAT_CLIENT_CORE_H

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <async/Spawn.h>
#include <async/Task.h>
#include <async/WaitGroup.h>
#include <async/Watch.h>
#include <common/IoError.h>
#include <event/EventLoop.h>
#include <fiber/cat/CatClient.h>
#include <net/SocketAddress.h>
#include <net/TcpStream.h>

#include "CatEncoder.h"
#include "CatMessageId.h"

namespace fiber::dns {
class AddressResolver;
}

namespace fiber::cat::detail {

class AggregationShard;
struct MessageTraceData;

enum class TraceDisposition : std::uint8_t {
    Detailed,
    Problem,
    Aggregate,
    Drop,
};

enum class FramePriority : std::uint8_t {
    Normal,
    Problem,
    System,
};

enum class FrameCategory : std::uint8_t {
    Detailed,
    Aggregate,
    Metric,
    Heartbeat,
    Startup,
};

enum class SubmitResult : std::uint8_t {
    Submitted,
    Unavailable,
    Full,
    Invalid,
};

class CatClientCore final : public std::enable_shared_from_this<CatClientCore> {
public:
    CatClientCore(event::EventLoop &sender_loop, CatClientConfig config, CatClientOptions options,
                  dns::AddressResolver *resolver) noexcept;
    ~CatClientCore();

    CatClientCore(const CatClientCore &) = delete;
    CatClientCore &operator=(const CatClientCore &) = delete;

    [[nodiscard]] common::IoResult<void> start() noexcept;
    [[nodiscard]] async::Task<void> shutdown() noexcept;
    void begin_stop() noexcept;

    [[nodiscard]] CatClientState state() const noexcept { return state_.load(std::memory_order_acquire); }
    [[nodiscard]] CatClientStats stats() const noexcept;
    [[nodiscard]] event::EventLoop &sender_loop() const noexcept { return *loop_; }

    [[nodiscard]] ClientEncodeContext encode_context() const noexcept;
    [[nodiscard]] CatEncoderType encoder_type() const noexcept { return options_.encoder; }
    [[nodiscard]] std::expected<mem::IoBuf, EncodeError> encode(const MessageTraceData &trace) const noexcept;
    [[nodiscard]] bool accepts_messages() const noexcept;
    [[nodiscard]] std::expected<GeneratedMessageId, RecordError>
    create_message_id(std::string_view domain = {}) noexcept;
    void on_context_failure(RecordError error) noexcept;
    [[nodiscard]] TraceDisposition trace_disposition(bool has_problem) noexcept;
    [[nodiscard]] AggregationShard *aggregation_shard(event::EventLoop &owner) noexcept;
    [[nodiscard]] async::Task<RecordError> detach_aggregation_shard(event::EventLoop &owner) noexcept;
    void aggregate_trace(const MessageTraceData &trace) noexcept;
    SubmitResult submit_encoded(mem::IoBuf message, FramePriority priority = FramePriority::Normal,
                                FrameCategory category = FrameCategory::Detailed) noexcept;
    [[nodiscard]] bool submit_aggregate(mem::IoBuf message) noexcept;
    [[nodiscard]] bool submit_metric_aggregate(mem::IoBuf message) noexcept;
    void on_aggregate_encode_failure() noexcept;
    void on_aggregate_drop(std::size_t count = 1) noexcept;
    void on_metric_drop(std::size_t count = 1) noexcept;
    void on_metric_observation(RecordError result) noexcept;
    void on_trace_truncated(const MessageTraceData &trace) noexcept;
    void on_encode_failure(EncodeError error) noexcept;

private:
    static constexpr std::uint64_t kClosedMask = std::uint64_t{1} << 63;

    struct OutboundFrame {
        OutboundFrame(CatClientCore &owner, mem::IoBuf value, FramePriority frame_priority,
                      FrameCategory frame_category) noexcept :
            core(&owner), message(std::move(value)), original_size(message.readable()), priority(frame_priority),
            category(frame_category) {}

        CatClientCore *core = nullptr;
        mem::IoBuf message;
        std::size_t original_size = 0;
        FramePriority priority = FramePriority::Normal;
        FrameCategory category = FrameCategory::Detailed;
        event::EventLoop::NotifyEntry notify_entry{};
        OutboundFrame *local_next = nullptr;

        static void on_notify(OutboundFrame *frame) noexcept;
    };

    enum class ReserveResult : std::uint8_t {
        Reserved,
        Closed,
        Full,
    };

    struct AtomicStats {
        std::atomic<std::uint64_t> submitted_messages{0};
        std::atomic<std::uint64_t> sent_messages{0};
        std::atomic<std::uint64_t> sent_bytes{0};
        std::atomic<std::uint64_t> dropped_queue_full{0};
        std::atomic<std::uint64_t> dropped_unavailable{0};
        std::atomic<std::uint64_t> dropped_sampled{0};
        std::atomic<std::uint64_t> dropped_partial_frame{0};
        std::atomic<std::uint64_t> encode_failures{0};
        std::atomic<std::uint64_t> router_successes{0};
        std::atomic<std::uint64_t> router_failures{0};
        std::atomic<std::uint64_t> connect_successes{0};
        std::atomic<std::uint64_t> connect_failures{0};
        std::atomic<std::uint64_t> write_would_block{0};
        std::atomic<std::uint64_t> write_failures{0};
        std::atomic<std::uint64_t> message_id_failures{0};
        std::atomic<std::uint64_t> context_failures{0};
        std::atomic<std::uint64_t> invalid_contexts{0};
        std::atomic<std::uint64_t> sampled_trees{0};
        std::atomic<std::uint64_t> forced_problem_trees{0};
        std::atomic<std::uint64_t> aggregated_trees{0};
        std::atomic<std::uint64_t> aggregation_overflow{0};
        std::atomic<std::uint64_t> aggregate_submitted{0};
        std::atomic<std::uint64_t> aggregate_dropped{0};
        std::atomic<std::uint64_t> aggregate_retry_failures{0};
        std::atomic<std::uint64_t> aggregate_encode_failures{0};
        std::atomic<std::uint64_t> metric_observations{0};
        std::atomic<std::uint64_t> metric_overflow{0};
        std::atomic<std::uint64_t> metric_submitted{0};
        std::atomic<std::uint64_t> metric_dropped{0};
        std::atomic<std::uint64_t> metric_retry_failures{0};
        std::atomic<std::uint64_t> heartbeat_submitted{0};
        std::atomic<std::uint64_t> heartbeat_sent{0};
        std::atomic<std::uint64_t> heartbeat_skipped{0};
        std::atomic<std::uint64_t> heartbeat_dropped{0};
        std::atomic<std::uint64_t> heartbeat_encode_failures{0};
        std::atomic<std::uint64_t> heartbeat_provider_failures{0};
        std::atomic<std::uint64_t> truncated_trees{0};
        std::atomic<std::uint64_t> truncated_messages{0};
        std::atomic<std::uint64_t> truncated_data_bytes{0};
        std::atomic<std::uint64_t> router_blocks{0};
        std::atomic<std::uint64_t> router_unblocks{0};
        std::atomic<std::uint64_t> router_sample_changes{0};
        std::atomic<std::uint64_t> collector_set_changes{0};
        std::atomic<std::uint64_t> stale_connection_switches{0};
    };

    [[nodiscard]] ReserveResult reserve_budget(std::size_t bytes, FramePriority priority) noexcept;
    [[nodiscard]] bool reserve_system_budget(std::size_t bytes) noexcept;
    void release_budget(std::size_t bytes, FramePriority priority) noexcept;
    void release_system_budget(std::size_t bytes) noexcept;
    [[nodiscard]] bool sampled_in() noexcept;
    void request_aggregate_flushes() noexcept;
    static void on_aggregate_timer(CatClientCore *client) noexcept;
    static void on_heartbeat_timer(CatClientCore *client) noexcept;
    void submit_startup() noexcept;
    void submit_heartbeat() noexcept;
    void frame_finished(FrameCategory category, bool sent) noexcept;

    void handle_frame_notify(OutboundFrame *frame) noexcept;
    void append_local(OutboundFrame *frame) noexcept;
    [[nodiscard]] bool has_local_frames() const noexcept { return priority_head_ || local_head_; }
    [[nodiscard]] bool priority_front_selected() const noexcept {
        if (priority_head_ && priority_head_->message.readable() != priority_head_->original_size) {
            return true;
        }
        if (local_head_ && local_head_->message.readable() != local_head_->original_size) {
            return false;
        }
        return priority_head_ != nullptr;
    }
    [[nodiscard]] OutboundFrame *front_frame() const noexcept {
        return priority_front_selected() ? priority_head_ : local_head_;
    }
    void schedule_pump() noexcept;

    static void on_pump_deferred(CatClientCore *client) noexcept;
    void drive_write() noexcept;
    void consume_written(std::size_t bytes) noexcept;
    void arm_write_wait() noexcept;
    void clear_write_wait() noexcept;
    static void on_write_ready(void *ctx, common::IoErr error) noexcept;
    static void on_write_timeout(CatClientCore *client) noexcept;
    void fail_connection(common::IoErr error) noexcept;
    void install_connection(std::unique_ptr<net::TcpStream> stream) noexcept;
    void close_connection() noexcept;
    void drop_detached_frame(OutboundFrame *frame) noexcept;
    void drop_front_frame(bool partial) noexcept;
    void drop_all_frames() noexcept;

    void notify_control() noexcept;
    [[nodiscard]] async::DetachedTask run_control() noexcept;
    [[nodiscard]] async::Task<void> wait_control(std::chrono::steady_clock::duration delay,
                                                 async::Watch<std::uint64_t>::Subscriber &wake,
                                                 std::uint64_t &version) noexcept;
    [[nodiscard]] async::Task<bool> refresh_router() noexcept;
    [[nodiscard]] async::Task<std::optional<std::vector<net::SocketAddress>>>
    resolve_endpoint(std::string_view host, std::uint16_t port) noexcept;
    [[nodiscard]] async::Task<std::optional<std::string>> fetch_router_body(const CatRouterEndpoint &router) noexcept;
    [[nodiscard]] async::Task<bool> connect_collector() noexcept;
    [[nodiscard]] async::Task<void> finish_shutdown() noexcept;

    event::EventLoop *loop_ = nullptr;
    CatClientConfig config_;
    CatClientOptions options_;
    dns::AddressResolver *resolver_ = nullptr;
    MessageIdGenerator message_id_generator_;

    std::atomic<CatClientState> state_{CatClientState::Created};
    std::atomic_bool blocked_{false};
    std::atomic<std::uint64_t> sample_cutoff_{~std::uint64_t{0}};
    std::atomic<std::uint64_t> sample_sequence_{0};
    std::atomic<std::uint32_t> active_submitters_{0};
    std::atomic<std::uint64_t> outstanding_state_{kClosedMask};
    std::atomic<std::uint64_t> system_outstanding_state_{0};
    AtomicStats stats_;

    event::EventLoop::DeferEntry pump_defer_entry_{};
    event::EventLoop::TimerEntry aggregate_timer_{};
    event::EventLoop::TimerEntry heartbeat_timer_{};
    OutboundFrame *local_head_ = nullptr;
    OutboundFrame *local_tail_ = nullptr;
    OutboundFrame *priority_head_ = nullptr;
    OutboundFrame *priority_tail_ = nullptr;

    std::unique_ptr<net::TcpStream> stream_;
    event::EventLoop::TimerEntry write_timer_{};
    bool write_callback_armed_ = false;
    bool connection_stale_ = false;

    async::Watch<std::uint64_t> control_wake_{0};
    std::optional<async::Watch<std::uint64_t>::Publisher> control_publisher_;
    std::atomic<std::uint64_t> control_generation_{0};
    async::WaitGroup control_done_;
    std::vector<net::SocketAddress> collectors_;
    std::size_t collector_index_ = 0;
    std::size_t router_index_ = 0;

    std::mutex aggregation_mutex_;
    std::array<AggregationShard *, 64> aggregation_shards_{};
    std::size_t aggregation_shard_count_ = 0;
    std::chrono::steady_clock::time_point process_start_steady_{};
    std::uint64_t process_start_wall_millis_ = 0;
    std::uint64_t router_last_success_millis_ = 0;
    bool startup_submitted_ = false;
    bool heartbeat_outstanding_ = false;
};

} // namespace fiber::cat::detail

#endif // FIBER_CAT_CLIENT_CORE_H
