#ifndef FIBER_CAT_SYSTEM_STATS_H
#define FIBER_CAT_SYSTEM_STATS_H

#include <cstdint>
#include <string_view>

namespace fiber::cat::detail {

struct SystemCpuCounters {
    std::uint64_t user = 0;
    std::uint64_t nice = 0;
    std::uint64_t system = 0;
    std::uint64_t idle = 0;
    std::uint64_t iowait = 0;
    std::uint64_t irq = 0;
    std::uint64_t softirq = 0;
};

struct ProcessCpuCounters {
    std::uint64_t user = 0;
    std::uint64_t system = 0;
};

struct RawSystemStats {
    double load_1min = 0.0;
    double load_5min = 0.0;
    double load_15min = 0.0;
    SystemCpuCounters cpu;
    ProcessCpuCounters process_cpu;
    std::uint64_t context_switches = 0;
    std::uint64_t interrupts = 0;
    std::uint64_t processes_running = 0;
    std::uint64_t processes_blocked = 0;
    std::uint64_t memory_total_bytes = 0;
    std::uint64_t memory_free_bytes = 0;
    std::uint64_t memory_cached_bytes = 0;
    std::uint64_t swap_total_bytes = 0;
    std::uint64_t swap_free_bytes = 0;
    std::uint64_t process_virtual_bytes = 0;
    std::uint64_t process_rss_bytes = 0;
    bool load_valid = false;
    bool cpu_valid = false;
    bool scheduler_valid = false;
    bool memory_valid = false;
    bool process_cpu_valid = false;
    bool process_memory_valid = false;
    bool provider_failure = false;
};

struct HeartbeatSystemStats {
    double load_1min = 0.0;
    double load_5min = 0.0;
    double load_15min = 0.0;
    SystemCpuCounters cpu_delta;
    std::uint64_t context_switches_delta = 0;
    std::uint64_t interrupts_delta = 0;
    std::uint64_t processes_running = 0;
    std::uint64_t processes_blocked = 0;
    std::uint64_t memory_total_bytes = 0;
    std::uint64_t memory_free_bytes = 0;
    std::uint64_t memory_cached_bytes = 0;
    std::uint64_t swap_total_bytes = 0;
    std::uint64_t swap_free_bytes = 0;
    std::uint64_t process_virtual_bytes = 0;
    std::uint64_t process_rss_bytes = 0;
    double cpu_user_percent = 0.0;
    double cpu_nice_percent = 0.0;
    double cpu_system_percent = 0.0;
    double cpu_idle_percent = 0.0;
    double cpu_iowait_percent = 0.0;
    double cpu_irq_percent = 0.0;
    double cpu_softirq_percent = 0.0;
    double process_cpu_user_percent = 0.0;
    double process_cpu_system_percent = 0.0;
    double process_cpu_total_percent = 0.0;
    double memory_free_percent = 0.0;
    double memory_used_percent = 0.0;
    bool load_valid = false;
    bool cpu_valid = false;
    bool scheduler_valid = false;
    bool scheduler_delta_valid = false;
    bool memory_valid = false;
    bool process_cpu_valid = false;
    bool process_memory_valid = false;
    bool provider_failure = false;

    [[nodiscard]] bool has_values() const noexcept {
        return load_valid || cpu_valid || scheduler_valid || memory_valid || process_cpu_valid || process_memory_valid;
    }
};

[[nodiscard]] bool parse_load_average(std::string_view text, RawSystemStats &stats) noexcept;
[[nodiscard]] bool parse_proc_stat(std::string_view text, RawSystemStats &stats) noexcept;
[[nodiscard]] bool parse_meminfo(std::string_view text, RawSystemStats &stats) noexcept;
[[nodiscard]] bool parse_process_stat(std::string_view text, RawSystemStats &stats) noexcept;
[[nodiscard]] bool parse_process_statm(std::string_view text, std::uint64_t page_size, RawSystemStats &stats) noexcept;

class SystemStatsCollector {
public:
    [[nodiscard]] HeartbeatSystemStats collect() noexcept;
    [[nodiscard]] HeartbeatSystemStats observe(const RawSystemStats &current) noexcept;

private:
    RawSystemStats previous_;
    bool has_previous_ = false;
};

} // namespace fiber::cat::detail

#endif // FIBER_CAT_SYSTEM_STATS_H
