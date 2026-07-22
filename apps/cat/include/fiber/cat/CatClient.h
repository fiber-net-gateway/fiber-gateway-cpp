#ifndef FIBER_CAT_CLIENT_H
#define FIBER_CAT_CLIENT_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>
#include <utility>

#include <async/Task.h>
#include <common/IoError.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <event/EventLoop.h>

#include "CatClientConfig.h"
#include "Message.h"
#include "PropagationContext.h"

namespace fiber::dns {
class AddressResolver;
}

namespace fiber::cat {

namespace detail {
class CatClientCore;
} // namespace detail

class MessageTrace;
class Metric;

enum class CatClientState : std::uint8_t {
    Created,
    Running,
    Stopping,
    Stopped,
};

enum class CatClientCreateError : std::uint8_t {
    InvalidOptions,
    InvalidResolver,
    NoMemory,
};

struct CatClientStats {
    std::size_t queued_messages = 0;
    std::size_t queued_bytes = 0;
    std::size_t system_queued_messages = 0;
    std::size_t system_queued_bytes = 0;
    std::uint64_t submitted_messages = 0;
    std::uint64_t sent_messages = 0;
    std::uint64_t sent_bytes = 0;
    std::uint64_t dropped_queue_full = 0;
    std::uint64_t dropped_unavailable = 0;
    std::uint64_t dropped_sampled = 0;
    std::uint64_t dropped_partial_frame = 0;
    std::uint64_t encode_failures = 0;
    std::uint64_t router_successes = 0;
    std::uint64_t router_failures = 0;
    std::uint64_t connect_successes = 0;
    std::uint64_t connect_failures = 0;
    std::uint64_t write_would_block = 0;
    std::uint64_t write_failures = 0;
    std::uint64_t message_id_failures = 0;
    std::uint64_t context_failures = 0;
    std::uint64_t invalid_contexts = 0;
    std::uint64_t sampled_trees = 0;
    std::uint64_t forced_problem_trees = 0;
    std::uint64_t aggregated_trees = 0;
    std::uint64_t aggregation_overflow = 0;
    std::uint64_t aggregate_submitted = 0;
    std::uint64_t aggregate_dropped = 0;
    std::uint64_t aggregate_retry_failures = 0;
    std::uint64_t aggregate_encode_failures = 0;
    std::uint64_t metric_observations = 0;
    std::uint64_t metric_overflow = 0;
    std::uint64_t metric_submitted = 0;
    std::uint64_t metric_dropped = 0;
    std::uint64_t metric_retry_failures = 0;
    std::uint64_t heartbeat_submitted = 0;
    std::uint64_t heartbeat_sent = 0;
    std::uint64_t heartbeat_skipped = 0;
    std::uint64_t heartbeat_dropped = 0;
    std::uint64_t heartbeat_encode_failures = 0;
    std::uint64_t heartbeat_provider_failures = 0;
    std::uint64_t truncated_trees = 0;
    std::uint64_t truncated_messages = 0;
    std::uint64_t truncated_data_bytes = 0;
    std::uint64_t router_blocks = 0;
    std::uint64_t router_unblocks = 0;
    std::uint64_t router_sample_changes = 0;
    std::uint64_t collector_set_changes = 0;
    std::uint64_t stale_connection_switches = 0;
};

class CatClient : public common::NonCopyable, public common::NonMovable {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<CatClient>, CatClientCreateError>
    create(event::EventLoop &sender_loop, CatClientConfig config, CatClientOptions options = {},
           dns::AddressResolver *resolver = nullptr) noexcept;

    ~CatClient();

    [[nodiscard]] common::IoResult<void> start() noexcept;
    [[nodiscard]] async::Task<void> shutdown() noexcept;
    [[nodiscard]] async::Task<RecordError> detach_current_event_loop() noexcept;

    [[nodiscard]] CatClientState state() const noexcept;
    [[nodiscard]] CatClientStats stats() const noexcept;
    [[nodiscard]] event::EventLoop &sender_loop() const noexcept;
    [[nodiscard]] std::expected<PropagationContext, RecordError>
    create_remote_context(const PropagationContext &current, std::string_view remote_domain) noexcept;

private:
    friend class MessageTrace;
    friend class Metric;

    explicit CatClient(std::shared_ptr<detail::CatClientCore> core) noexcept : core_(std::move(core)) {}

    [[nodiscard]] std::shared_ptr<detail::CatClientCore> core() const noexcept;

    std::shared_ptr<detail::CatClientCore> core_;
};

} // namespace fiber::cat

#endif // FIBER_CAT_CLIENT_H
