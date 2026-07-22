#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "CatSystemStats.h"

namespace {

using fiber::cat::detail::RawSystemStats;
using fiber::cat::detail::SystemStatsCollector;

TEST(CatSystemStatsTest, ParsesLinuxProcSnapshots) {
    RawSystemStats stats;
    EXPECT_TRUE(fiber::cat::detail::parse_load_average("1.25 0.50 0.10 2/100 42\n", stats));
    EXPECT_TRUE(fiber::cat::detail::parse_proc_stat(R"(cpu  100 20 30 400 50 6 7 8 9 10
intr 900 1 2 3
ctxt 1000
procs_running 3
procs_blocked 1
)",
                                                    stats));
    EXPECT_TRUE(fiber::cat::detail::parse_meminfo(R"(MemTotal:       1024 kB
MemFree:         256 kB
Cached:           64 kB
SwapTotal:       128 kB
SwapFree:         32 kB
)",
                                                  stats));
    EXPECT_TRUE(
            fiber::cat::detail::parse_process_stat("123 (worker ) name) R 1 2 3 4 5 6 7 8 9 10 120 30 0 0\n", stats));
    EXPECT_TRUE(fiber::cat::detail::parse_process_statm("100 25 5 1 0 10 0\n", 4096, stats));

    EXPECT_TRUE(stats.load_valid);
    EXPECT_DOUBLE_EQ(stats.load_1min, 1.25);
    EXPECT_DOUBLE_EQ(stats.load_5min, 0.50);
    EXPECT_DOUBLE_EQ(stats.load_15min, 0.10);
    EXPECT_TRUE(stats.cpu_valid);
    EXPECT_EQ(stats.cpu.user, 100);
    EXPECT_EQ(stats.cpu.softirq, 7);
    EXPECT_TRUE(stats.scheduler_valid);
    EXPECT_EQ(stats.interrupts, 900);
    EXPECT_EQ(stats.context_switches, 1000);
    EXPECT_EQ(stats.processes_running, 3);
    EXPECT_EQ(stats.processes_blocked, 1);
    EXPECT_TRUE(stats.memory_valid);
    EXPECT_EQ(stats.memory_total_bytes, 1024U * 1024U);
    EXPECT_EQ(stats.memory_free_bytes, 256U * 1024U);
    EXPECT_EQ(stats.memory_cached_bytes, 64U * 1024U);
    EXPECT_EQ(stats.swap_total_bytes, 128U * 1024U);
    EXPECT_EQ(stats.swap_free_bytes, 32U * 1024U);
    EXPECT_TRUE(stats.process_cpu_valid);
    EXPECT_EQ(stats.process_cpu.user, 120);
    EXPECT_EQ(stats.process_cpu.system, 30);
    EXPECT_TRUE(stats.process_memory_valid);
    EXPECT_EQ(stats.process_virtual_bytes, 100U * 4096U);
    EXPECT_EQ(stats.process_rss_bytes, 25U * 4096U);
}

TEST(CatSystemStatsTest, ComputesCpuDeltasAfterTheFirstObservation) {
    SystemStatsCollector collector;
    RawSystemStats first{
            .load_1min = 1.0,
            .load_5min = 2.0,
            .load_15min = 3.0,
            .cpu = {.user = 100, .nice = 20, .system = 30, .idle = 400, .iowait = 50, .irq = 6, .softirq = 7},
            .process_cpu = {.user = 40, .system = 10},
            .context_switches = 1000,
            .interrupts = 500,
            .processes_running = 2,
            .processes_blocked = 1,
            .memory_total_bytes = 1000,
            .memory_free_bytes = 250,
            .memory_cached_bytes = 100,
            .swap_total_bytes = 200,
            .swap_free_bytes = 150,
            .process_virtual_bytes = 800,
            .process_rss_bytes = 300,
            .load_valid = true,
            .cpu_valid = true,
            .scheduler_valid = true,
            .memory_valid = true,
            .process_cpu_valid = true,
            .process_memory_valid = true,
    };
    auto initial = collector.observe(first);
    EXPECT_TRUE(initial.load_valid);
    EXPECT_TRUE(initial.scheduler_valid);
    EXPECT_TRUE(initial.memory_valid);
    EXPECT_TRUE(initial.process_memory_valid);
    EXPECT_FALSE(initial.cpu_valid);
    EXPECT_FALSE(initial.process_cpu_valid);
    EXPECT_FALSE(initial.scheduler_delta_valid);
    EXPECT_DOUBLE_EQ(initial.memory_free_percent, 25.0);
    EXPECT_DOUBLE_EQ(initial.memory_used_percent, 75.0);

    RawSystemStats second = first;
    second.cpu = {.user = 110, .nice = 25, .system = 35, .idle = 480, .iowait = 50, .irq = 6, .softirq = 7};
    second.process_cpu = {.user = 44, .system = 11};
    second.context_switches = 1100;
    second.interrupts = 530;
    auto current = collector.observe(second);

    ASSERT_TRUE(current.cpu_valid);
    EXPECT_EQ(current.cpu_delta.user, 10);
    EXPECT_EQ(current.cpu_delta.nice, 5);
    EXPECT_EQ(current.cpu_delta.system, 5);
    EXPECT_EQ(current.cpu_delta.idle, 80);
    EXPECT_DOUBLE_EQ(current.cpu_user_percent, 10.0);
    EXPECT_DOUBLE_EQ(current.cpu_nice_percent, 5.0);
    EXPECT_DOUBLE_EQ(current.cpu_system_percent, 5.0);
    EXPECT_DOUBLE_EQ(current.cpu_idle_percent, 80.0);
    ASSERT_TRUE(current.process_cpu_valid);
    EXPECT_DOUBLE_EQ(current.process_cpu_user_percent, 4.0);
    EXPECT_DOUBLE_EQ(current.process_cpu_system_percent, 1.0);
    EXPECT_DOUBLE_EQ(current.process_cpu_total_percent, 5.0);
    ASSERT_TRUE(current.scheduler_delta_valid);
    EXPECT_EQ(current.context_switches_delta, 100);
    EXPECT_EQ(current.interrupts_delta, 30);
}

TEST(CatSystemStatsTest, OmitsInvalidSourcesAndRebasesRolledBackCounters) {
    SystemStatsCollector collector;
    RawSystemStats first{
            .cpu = {.user = 100, .idle = 100},
            .process_cpu = {.user = 50},
            .cpu_valid = true,
            .process_cpu_valid = true,
    };
    (void) collector.observe(first);

    RawSystemStats rolled_back{
            .cpu = {.user = 10, .idle = 10},
            .process_cpu = {.user = 5},
            .cpu_valid = true,
            .process_cpu_valid = true,
            .provider_failure = true,
    };
    auto failed = collector.observe(rolled_back);
    EXPECT_TRUE(failed.provider_failure);
    EXPECT_FALSE(failed.cpu_valid);
    EXPECT_FALSE(failed.process_cpu_valid);

    RawSystemStats recovered = rolled_back;
    recovered.cpu = {.user = 20, .idle = 100};
    recovered.process_cpu = {.user = 10};
    recovered.provider_failure = false;
    auto current = collector.observe(recovered);
    ASSERT_TRUE(current.cpu_valid);
    EXPECT_DOUBLE_EQ(current.cpu_user_percent, 10.0);
    EXPECT_DOUBLE_EQ(current.cpu_idle_percent, 90.0);
    ASSERT_TRUE(current.process_cpu_valid);
    EXPECT_DOUBLE_EQ(current.process_cpu_total_percent, 5.0);
}

TEST(CatSystemStatsTest, RejectsMalformedAndOverflowingSnapshots) {
    RawSystemStats stats;
    EXPECT_FALSE(fiber::cat::detail::parse_load_average("bad 1 2", stats));
    EXPECT_FALSE(fiber::cat::detail::parse_proc_stat("cpu 1 2\n", stats));
    EXPECT_FALSE(fiber::cat::detail::parse_meminfo("MemTotal: 0 kB\n", stats));
    EXPECT_FALSE(fiber::cat::detail::parse_process_stat("12 no-parenthesis", stats));
    EXPECT_FALSE(fiber::cat::detail::parse_process_statm("2 1", std::numeric_limits<std::uint64_t>::max(), stats));
}

} // namespace
