#ifndef FIBER_AI_SERVER_PROCESS_METRICS_H
#define FIBER_AI_SERVER_PROCESS_METRICS_H

#include <cstdint>
#include <optional>

namespace fiber::ai_server {

struct ProcessMetricsSnapshot {
    std::optional<std::uint64_t> cpu_time_nanoseconds;
    std::optional<std::uint64_t> resident_memory_bytes;
    std::optional<std::uint64_t> virtual_memory_bytes;
    std::optional<std::uint64_t> start_time_nanoseconds;
    std::optional<std::uint64_t> open_fds;
    std::optional<std::uint64_t> max_fds;
};

[[nodiscard]] ProcessMetricsSnapshot collect_process_metrics() noexcept;

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_PROCESS_METRICS_H
