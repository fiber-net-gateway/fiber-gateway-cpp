#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstring>

#include "common/mem/BufPool.h"

namespace {

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

} // namespace
