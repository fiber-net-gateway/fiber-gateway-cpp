#include <gtest/gtest.h>

#include <cstring>
#include <string_view>
#include <type_traits>

#include "common/mem/IoBuf.h"

namespace {

using fiber::mem::IoBuf;

std::string_view readable_view(const IoBuf &buf) {
    return {reinterpret_cast<const char *>(buf.readable_data()), buf.readable()};
}

static_assert(std::is_copy_constructible_v<IoBuf>);
static_assert(std::is_copy_assignable_v<IoBuf>);
static_assert(std::is_move_constructible_v<IoBuf>);
static_assert(std::is_move_assignable_v<IoBuf>);

TEST(IoBufTest, AllocateCommitConsumeCopyAndMove) {
    IoBuf buf = IoBuf::allocate(32);
    ASSERT_TRUE(buf);
    EXPECT_EQ(buf.capacity(), 32u);
    EXPECT_EQ(buf.readable(), 0u);
    EXPECT_EQ(buf.writable(), 32u);
    EXPECT_TRUE(buf.unique());
    EXPECT_EQ(buf.use_count(), 1u);

    std::memcpy(buf.writable_data(), "hello", 5);
    buf.commit(5);
    EXPECT_EQ(readable_view(buf), "hello");
    EXPECT_EQ(buf.readable(), 5u);
    EXPECT_EQ(buf.writable(), 27u);

    IoBuf copy(buf);
    ASSERT_TRUE(copy);
    EXPECT_EQ(copy.use_count(), 2u);
    EXPECT_EQ(copy.data(), buf.data());
    EXPECT_EQ(copy.readable_data(), buf.readable_data());
    EXPECT_EQ(readable_view(copy), "hello");

    buf.consume(2);
    EXPECT_EQ(readable_view(buf), "llo");
    EXPECT_EQ(buf.headroom(), 2u);
    EXPECT_EQ(readable_view(copy), "hello");

    IoBuf assigned;
    assigned = buf;
    ASSERT_TRUE(assigned);
    EXPECT_EQ(buf.use_count(), 3u);
    EXPECT_EQ(assigned.data(), buf.data());
    EXPECT_EQ(assigned.readable_data(), buf.readable_data());
    EXPECT_EQ(readable_view(assigned), "llo");

    buf.clear();
    EXPECT_EQ(buf.readable(), 0u);
    EXPECT_EQ(buf.writable(), 32u);

    IoBuf moved = std::move(buf);
    EXPECT_FALSE(buf.valid());
    ASSERT_TRUE(moved);
    EXPECT_EQ(moved.capacity(), 32u);
    EXPECT_EQ(moved.use_count(), 3u);
}

TEST(IoBufTest, RetainSliceSharesStorageWithoutCopy) {
    IoBuf buf = IoBuf::allocate(32);
    ASSERT_TRUE(buf);

    std::memcpy(buf.writable_data(), "abcdef", 6);
    buf.commit(6);

    IoBuf slice = buf.retain_slice(1, 3);
    ASSERT_TRUE(slice);
    EXPECT_EQ(buf.use_count(), 2u);
    EXPECT_EQ(slice.use_count(), 2u);
    EXPECT_FALSE(buf.unique());
    EXPECT_EQ(readable_view(slice), "bcd");
    EXPECT_EQ(slice.writable(), 0u);

    slice.consume(1);
    EXPECT_EQ(readable_view(slice), "cd");
    EXPECT_EQ(readable_view(buf), "abcdef");
}

TEST(IoBufTest, UnsafeRetainSliceUsesSameStorage) {
    IoBuf buf = IoBuf::allocate(16);
    ASSERT_TRUE(buf);

    std::memcpy(buf.writable_data(), "payload", 7);
    buf.commit(7);

    IoBuf slice = buf.unsafe_retain_slice(2, 4);
    ASSERT_TRUE(slice);
    EXPECT_EQ(buf.use_count(), 2u);
    EXPECT_EQ(readable_view(slice), "yloa");
    EXPECT_EQ(slice.data(), buf.data());
}

TEST(IoBufTest, RetainStorageSliceBuildsReadableViewAtAbsoluteOffset) {
    IoBuf storage = IoBuf::allocate(16);
    ASSERT_TRUE(storage);

    std::memcpy(storage.data() + 5, "abc", 3);
    IoBuf slice = storage.retain_storage_slice(5, 3);

    ASSERT_TRUE(slice);
    EXPECT_TRUE(storage.same_storage(slice));
    EXPECT_EQ(readable_view(slice), "abc");
    EXPECT_EQ(slice.writable(), 0u);
    EXPECT_EQ(slice.readable_data(), storage.data() + 5);
}

TEST(IoBufTest, TryMergeAdjacentExtendsSameStorageReadableView) {
    IoBuf storage = IoBuf::allocate(16);
    ASSERT_TRUE(storage);

    std::memcpy(storage.data() + 4, "abcd", 4);
    IoBuf first = storage.retain_storage_slice(4, 2);
    IoBuf second = storage.retain_storage_slice(6, 2);

    ASSERT_TRUE(first.try_merge_adjacent(std::move(second)));
    EXPECT_EQ(readable_view(first), "abcd");
    EXPECT_FALSE(second.valid());

    IoBuf other = IoBuf::allocate(4);
    ASSERT_TRUE(other);
    std::memcpy(other.writable_data(), "zz", 2);
    other.commit(2);
    EXPECT_FALSE(first.try_merge_adjacent(std::move(other)));
    EXPECT_TRUE(other.valid());
}

} // namespace
