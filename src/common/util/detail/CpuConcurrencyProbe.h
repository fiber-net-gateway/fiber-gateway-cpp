#ifndef FIBER_UTIL_DETAIL_CPU_CONCURRENCY_PROBE_H
#define FIBER_UTIL_DETAIL_CPU_CONCURRENCY_PROBE_H

#include <fiber/common/util/CpuConcurrency.h>

#include <cstddef>

namespace fiber::util::detail {

struct CpuConcurrencyProbeOptions {
    const char *proc_self_cgroup_path = "/proc/self/cgroup";
    const char *proc_self_mountinfo_path = "/proc/self/mountinfo";
    std::size_t affinity_count = 0;
    std::size_t hardware_concurrency = 0;
    bool use_supplied_cpu_counts = false;
};

[[nodiscard]] CpuConcurrency detect_cpu_concurrency(const CpuConcurrencyProbeOptions &options) noexcept;

} // namespace fiber::util::detail

#endif // FIBER_UTIL_DETAIL_CPU_CONCURRENCY_PROBE_H
