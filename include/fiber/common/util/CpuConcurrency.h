#ifndef FIBER_UTIL_CPU_CONCURRENCY_H
#define FIBER_UTIL_CPU_CONCURRENCY_H

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fiber::util {

enum class CpuConcurrencySource : std::uint8_t {
    Fallback,
    HardwareConcurrency,
    Affinity,
    CgroupV1Quota,
    CgroupV2Quota,
};

struct CpuConcurrency {
    std::size_t effective_count = 1;
    std::size_t affinity_count = 0;
    std::size_t quota_count = 0;
    std::uint64_t quota_us = 0;
    std::uint64_t period_us = 0;
    CpuConcurrencySource source = CpuConcurrencySource::Fallback;
    bool cgroup_probe_failed = false;
};

[[nodiscard]] CpuConcurrency detect_cpu_concurrency() noexcept;
[[nodiscard]] std::string_view cpu_concurrency_source_name(CpuConcurrencySource source) noexcept;

} // namespace fiber::util

#endif // FIBER_UTIL_CPU_CONCURRENCY_H
