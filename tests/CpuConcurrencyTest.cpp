#include <gtest/gtest.h>

#include "common/util/CpuConcurrency.h"
#include "common/util/detail/CpuConcurrencyProbe.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <cstdlib>

namespace {

class CpuCgroupFixture {
public:
    CpuCgroupFixture() {
        char pattern[] = "/tmp/fiber_cpu_concurrency_XXXXXX";
        char *created = ::mkdtemp(pattern);
        if (created) {
            root_ = created;
        }
    }

    ~CpuCgroupFixture() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    CpuCgroupFixture(const CpuCgroupFixture &) = delete;
    CpuCgroupFixture &operator=(const CpuCgroupFixture &) = delete;

    [[nodiscard]] bool valid() const noexcept { return !root_.empty(); }

    [[nodiscard]] std::filesystem::path path(std::string_view relative) const {
        return root_ / std::filesystem::path(relative);
    }

    bool write(std::string_view relative, std::string_view contents) const {
        const std::filesystem::path target = path(relative);
        std::error_code error;
        std::filesystem::create_directories(target.parent_path(), error);
        if (error) {
            return false;
        }
        std::ofstream output(target, std::ios::binary);
        output << contents;
        return output.good();
    }

    fiber::util::detail::CpuConcurrencyProbeOptions options(std::size_t affinity, std::size_t hardware = 128) const {
        cgroup_path_ = path("proc-self-cgroup").string();
        mountinfo_path_ = path("proc-self-mountinfo").string();
        return {
                .proc_self_cgroup_path = cgroup_path_.c_str(),
                .proc_self_mountinfo_path = mountinfo_path_.c_str(),
                .affinity_count = affinity,
                .hardware_concurrency = hardware,
                .use_supplied_cpu_counts = true,
        };
    }

    bool configure_v2(std::string_view membership, std::string_view mount_root = "/") const {
        const std::string mount_point = path("cgroup2").string();
        return write("proc-self-cgroup", std::string("0::") + std::string(membership) + "\n") &&
               write("proc-self-mountinfo", "29 23 0:26 " + std::string(mount_root) + " " + mount_point +
                                                    " rw,nosuid,nodev,noexec - cgroup2 cgroup rw\n");
    }

    bool configure_v1(std::string_view membership) const {
        const std::string mount_point = path("cgroup-v1-cpu").string();
        return write("proc-self-cgroup", std::string("2:cpu,cpuacct:") + std::string(membership) + "\n") &&
               write("proc-self-mountinfo",
                     "31 23 0:28 / " + mount_point + " rw,nosuid,nodev,noexec - cgroup cgroup rw,cpu,cpuacct\n");
    }

private:
    std::filesystem::path root_;
    mutable std::string cgroup_path_;
    mutable std::string mountinfo_path_;
};

TEST(CpuConcurrencyTest, PublicProbeNeverReturnsZero) {
    const auto cpu = fiber::util::detect_cpu_concurrency();
    EXPECT_GE(cpu.effective_count, 1U);
}

TEST(CpuConcurrencyTest, UsesAffinityWhenV2QuotaIsUnlimited) {
    CpuCgroupFixture fixture;
    ASSERT_TRUE(fixture.valid());
    ASSERT_TRUE(fixture.configure_v2("/service"));
    ASSERT_TRUE(fixture.write("cgroup2/service/cpu.max", "max 100000\n"));

    const auto cpu = fiber::util::detail::detect_cpu_concurrency(fixture.options(4));

    EXPECT_EQ(cpu.effective_count, 4U);
    EXPECT_EQ(cpu.affinity_count, 4U);
    EXPECT_EQ(cpu.quota_count, 0U);
    EXPECT_EQ(cpu.source, fiber::util::CpuConcurrencySource::Affinity);
    EXPECT_FALSE(cpu.cgroup_probe_failed);
}

TEST(CpuConcurrencyTest, RoundsFractionalV2QuotaUp) {
    CpuCgroupFixture fixture;
    ASSERT_TRUE(fixture.valid());
    ASSERT_TRUE(fixture.configure_v2("/service"));
    ASSERT_TRUE(fixture.write("cgroup2/service/cpu.max", "250000 100000\n"));

    const auto cpu = fiber::util::detail::detect_cpu_concurrency(fixture.options(64));

    EXPECT_EQ(cpu.effective_count, 3U);
    EXPECT_EQ(cpu.quota_count, 3U);
    EXPECT_EQ(cpu.quota_us, 250000U);
    EXPECT_EQ(cpu.period_us, 100000U);
    EXPECT_EQ(cpu.source, fiber::util::CpuConcurrencySource::CgroupV2Quota);
}

TEST(CpuConcurrencyTest, KeepsAtLeastOneWorkerForSubCpuQuota) {
    CpuCgroupFixture fixture;
    ASSERT_TRUE(fixture.valid());
    ASSERT_TRUE(fixture.configure_v2("/service"));
    ASSERT_TRUE(fixture.write("cgroup2/service/cpu.max", "50000 100000\n"));

    const auto cpu = fiber::util::detail::detect_cpu_concurrency(fixture.options(64));

    EXPECT_EQ(cpu.effective_count, 1U);
    EXPECT_EQ(cpu.quota_count, 1U);
}

TEST(CpuConcurrencyTest, UsesTighterV2AncestorQuota) {
    CpuCgroupFixture fixture;
    ASSERT_TRUE(fixture.valid());
    ASSERT_TRUE(fixture.configure_v2("/parent/service"));
    ASSERT_TRUE(fixture.write("cgroup2/parent/service/cpu.max", "max 100000\n"));
    ASSERT_TRUE(fixture.write("cgroup2/parent/cpu.max", "200000 100000\n"));

    const auto cpu = fiber::util::detail::detect_cpu_concurrency(fixture.options(64));

    EXPECT_EQ(cpu.effective_count, 2U);
    EXPECT_EQ(cpu.quota_count, 2U);
    EXPECT_EQ(cpu.source, fiber::util::CpuConcurrencySource::CgroupV2Quota);
}

TEST(CpuConcurrencyTest, ResolvesCgroupNamespaceRootToMountedSubtree) {
    CpuCgroupFixture fixture;
    ASSERT_TRUE(fixture.valid());
    ASSERT_TRUE(fixture.configure_v2("/service", "/system.slice/container.scope"));
    ASSERT_TRUE(fixture.write("cgroup2/service/cpu.max", "300000 100000\n"));

    const auto cpu = fiber::util::detail::detect_cpu_concurrency(fixture.options(64));

    EXPECT_EQ(cpu.effective_count, 3U);
    EXPECT_EQ(cpu.quota_count, 3U);
    EXPECT_EQ(cpu.source, fiber::util::CpuConcurrencySource::CgroupV2Quota);
    EXPECT_FALSE(cpu.cgroup_probe_failed);
}

TEST(CpuConcurrencyTest, AffinityWinsWhenItIsTighterThanQuota) {
    CpuCgroupFixture fixture;
    ASSERT_TRUE(fixture.valid());
    ASSERT_TRUE(fixture.configure_v2("/service"));
    ASSERT_TRUE(fixture.write("cgroup2/service/cpu.max", "800000 100000\n"));

    const auto cpu = fiber::util::detail::detect_cpu_concurrency(fixture.options(2));

    EXPECT_EQ(cpu.effective_count, 2U);
    EXPECT_EQ(cpu.quota_count, 8U);
    EXPECT_EQ(cpu.source, fiber::util::CpuConcurrencySource::Affinity);
}

TEST(CpuConcurrencyTest, SupportsV1CpuQuota) {
    CpuCgroupFixture fixture;
    ASSERT_TRUE(fixture.valid());
    ASSERT_TRUE(fixture.configure_v1("/service"));
    ASSERT_TRUE(fixture.write("cgroup-v1-cpu/service/cpu.cfs_quota_us", "150000\n"));
    ASSERT_TRUE(fixture.write("cgroup-v1-cpu/service/cpu.cfs_period_us", "100000\n"));

    const auto cpu = fiber::util::detail::detect_cpu_concurrency(fixture.options(64));

    EXPECT_EQ(cpu.effective_count, 2U);
    EXPECT_EQ(cpu.quota_count, 2U);
    EXPECT_EQ(cpu.source, fiber::util::CpuConcurrencySource::CgroupV1Quota);
}

TEST(CpuConcurrencyTest, TreatsV1NegativeQuotaAsUnlimited) {
    CpuCgroupFixture fixture;
    ASSERT_TRUE(fixture.valid());
    ASSERT_TRUE(fixture.configure_v1("/service"));
    ASSERT_TRUE(fixture.write("cgroup-v1-cpu/service/cpu.cfs_quota_us", "-1\n"));
    ASSERT_TRUE(fixture.write("cgroup-v1-cpu/service/cpu.cfs_period_us", "100000\n"));

    const auto cpu = fiber::util::detail::detect_cpu_concurrency(fixture.options(6));

    EXPECT_EQ(cpu.effective_count, 6U);
    EXPECT_EQ(cpu.quota_count, 0U);
    EXPECT_EQ(cpu.source, fiber::util::CpuConcurrencySource::Affinity);
}

TEST(CpuConcurrencyTest, ReportsMalformedQuotaAndFallsBackToAffinity) {
    CpuCgroupFixture fixture;
    ASSERT_TRUE(fixture.valid());
    ASSERT_TRUE(fixture.configure_v2("/service"));
    ASSERT_TRUE(fixture.write("cgroup2/service/cpu.max", "invalid\n"));

    const auto cpu = fiber::util::detail::detect_cpu_concurrency(fixture.options(4));

    EXPECT_EQ(cpu.effective_count, 4U);
    EXPECT_TRUE(cpu.cgroup_probe_failed);
}

TEST(CpuConcurrencyTest, FallsBackToHardwareConcurrencyWhenAffinityFails) {
    CpuCgroupFixture fixture;
    ASSERT_TRUE(fixture.valid());

    const auto cpu = fiber::util::detail::detect_cpu_concurrency(fixture.options(0, 12));

    EXPECT_EQ(cpu.effective_count, 12U);
    EXPECT_EQ(cpu.source, fiber::util::CpuConcurrencySource::HardwareConcurrency);
}

} // namespace
