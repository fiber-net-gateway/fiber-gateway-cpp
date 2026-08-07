#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <utility>

#include <fiber/common/mem/BufPool.h>

namespace {

static_assert(std::is_nothrow_constructible_v<fiber::mem::BufPool>);
static_assert(std::is_nothrow_destructible_v<fiber::mem::BufPool>);
static_assert(noexcept(std::declval<fiber::mem::BufPool &>().alloc(1)));
static_assert(noexcept(std::declval<fiber::mem::BufPool &>().alloc<unsigned char>()));

TEST(BufPoolTest, PageSizedAllocationCanBeFullyWritten) {
    fiber::mem::BufPool pool;

    constexpr std::size_t kSize = 4096;
    auto *ptr = static_cast<unsigned char *>(pool.alloc(kSize));
    ASSERT_NE(ptr, nullptr);

    std::memset(ptr, 0xab, kSize);
    EXPECT_EQ(ptr[0], 0xab);
    EXPECT_EQ(ptr[kSize - 1], 0xab);
}

TEST(BufPoolTest, SmallBlockCapacityExcludesBlockHeader) {
    fiber::mem::BufPool pool(64);
    std::array<unsigned char *, 64> allocations{};

    for (auto *&ptr: allocations) {
        ptr = static_cast<unsigned char *>(pool.alloc(8));
        ASSERT_NE(ptr, nullptr);
        std::memset(ptr, 0xcd, 8);
    }

    for (const auto *ptr: allocations) {
        EXPECT_EQ(ptr[0], 0xcd);
        EXPECT_EQ(ptr[7], 0xcd);
    }
}

TEST(BufPoolTest, ResetReleasesStorageAndAllowsReuse) {
    fiber::mem::BufPool pool(64);
    auto *first = static_cast<unsigned char *>(pool.alloc(256));
    ASSERT_NE(first, nullptr);
    std::memset(first, 0xaa, 256);

    pool.reset();

    auto *second = static_cast<unsigned char *>(pool.alloc(256));
    ASSERT_NE(second, nullptr);
    std::memset(second, 0xbb, 256);
    EXPECT_EQ(second[0], 0xbb);
    EXPECT_EQ(second[255], 0xbb);
}

} // namespace
