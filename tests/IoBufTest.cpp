#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

#include "common/mem/IoBuf.h"

namespace {

using fiber::mem::IoBuf;
using fiber::mem::IoBufChain;
using fiber::mem::IoBufNodePool;

std::string_view readable_view(const IoBuf &buf) {
    return {reinterpret_cast<const char *>(buf.readable_data()), buf.readable()};
}

std::string readable_string(const IoBufChain &chain) {
    std::array<iovec, 16> iov{};
    int count = chain.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
    std::string out;
    for (int i = 0; i < count; ++i) {
        out.append(static_cast<const char *>(iov[i].iov_base), iov[i].iov_len);
    }
    return out;
}

static_assert(std::is_copy_constructible_v<IoBuf>);
static_assert(std::is_copy_assignable_v<IoBuf>);
static_assert(std::is_move_constructible_v<IoBuf>);
static_assert(std::is_move_assignable_v<IoBuf>);
static_assert(!std::is_copy_constructible_v<IoBufChain>);
static_assert(std::is_move_constructible_v<IoBufChain>);

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

TEST(IoBufTest, ChainExportsReadableAndWritableIovecs) {
    IoBuf a = IoBuf::allocate(8);
    IoBuf b = IoBuf::allocate(8);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);

    std::memcpy(a.writable_data(), "ab", 2);
    a.commit(2);
    std::memcpy(b.writable_data(), "cdef", 4);
    b.commit(4);

    IoBufChain chain;
    ASSERT_TRUE(chain.append(std::move(a)));
    ASSERT_TRUE(chain.append(std::move(b)));
    EXPECT_EQ(chain.size(), 2u);
    EXPECT_EQ(chain.readable_bytes(), 6u);

    std::array<iovec, 4> iov{};
    int count = chain.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
    ASSERT_EQ(count, 2);
    EXPECT_EQ(std::string_view(static_cast<const char *>(iov[0].iov_base), iov[0].iov_len), "ab");
    EXPECT_EQ(std::string_view(static_cast<const char *>(iov[1].iov_base), iov[1].iov_len), "cdef");

    chain.consume(3);
    EXPECT_EQ(chain.readable_bytes(), 3u);
    EXPECT_EQ(chain.size(), 2u);
    count = chain.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
    ASSERT_EQ(count, 1);
    EXPECT_EQ(std::string_view(static_cast<const char *>(iov[0].iov_base), iov[0].iov_len), "def");
}

TEST(IoBufTest, NodePoolResetsNodeOnReuse) {
    IoBufNodePool pool;
    auto *node = pool.alloc();
    ASSERT_NE(node, nullptr);

    IoBuf buf = IoBuf::allocate(8);
    ASSERT_TRUE(buf);
    std::memcpy(buf.writable_data(), "abc", 3);
    buf.commit(3);

    node->offset = 42;
    node->state = 7;
    node->buf = std::move(buf);
    node->next = node;
    pool.release(node);
    EXPECT_EQ(pool.cached_count(), 1u);

    auto *reused = pool.alloc();
    ASSERT_EQ(reused, node);
    EXPECT_EQ(reused->offset, 0u);
    EXPECT_EQ(reused->state, 0u);
    EXPECT_FALSE(reused->buf.valid());
    EXPECT_EQ(reused->next, nullptr);
    pool.release(reused);
}

TEST(IoBufTest, ChainAppendNodeTakesOwnershipAndResetsOwnerFields) {
    IoBufNodePool pool;
    auto *node = pool.alloc();
    ASSERT_NE(node, nullptr);

    IoBuf buf = IoBuf::allocate(8);
    ASSERT_TRUE(buf);
    std::memcpy(buf.writable_data(), "abc", 3);
    buf.commit(3);

    node->offset = 99;
    node->state = 3;
    node->buf = std::move(buf);
    node->next = node;

    IoBufChain chain;
    ASSERT_TRUE(chain.append_node(node));
    EXPECT_EQ(node->offset, 0u);
    EXPECT_EQ(node->state, 0u);
    EXPECT_EQ(node->next, nullptr);
    EXPECT_EQ(chain.size(), 1u);
    EXPECT_EQ(chain.readable_bytes(), 3u);
    EXPECT_EQ(readable_string(chain), "abc");
}

TEST(IoBufTest, DropEmptyFrontRemovesOnlyDrainedPrefix) {
    IoBuf a = IoBuf::allocate(4);
    IoBuf b = IoBuf::allocate(4);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);

    std::memcpy(a.writable_data(), "ab", 2);
    a.commit(2);
    std::memcpy(b.writable_data(), "cd", 2);
    b.commit(2);

    IoBufChain chain;
    ASSERT_TRUE(chain.append(std::move(a)));
    ASSERT_TRUE(chain.append(std::move(b)));

    chain.consume(2);
    ASSERT_NE(chain.front(), nullptr);
    EXPECT_EQ(chain.size(), 2u);
    EXPECT_EQ(chain.front()->readable(), 0u);

    chain.drop_empty_front();
    ASSERT_NE(chain.front(), nullptr);
    EXPECT_EQ(chain.size(), 1u);
    EXPECT_EQ(readable_view(*chain.front()), "cd");
    EXPECT_EQ(chain.writable_bytes(), 2u);
}

TEST(IoBufTest, ConsumeAndCompactDropsFullyConsumedFrontNodes) {
    IoBuf a = IoBuf::allocate(4);
    IoBuf b = IoBuf::allocate(4);
    IoBuf c = IoBuf::allocate(4);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    ASSERT_TRUE(c);

    std::memcpy(a.writable_data(), "ab", 2);
    a.commit(2);
    std::memcpy(b.writable_data(), "cd", 2);
    b.commit(2);
    std::memcpy(c.writable_data(), "ef", 2);
    c.commit(2);

    IoBufChain chain;
    ASSERT_TRUE(chain.append(std::move(a)));
    ASSERT_TRUE(chain.append(std::move(b)));
    ASSERT_TRUE(chain.append(std::move(c)));

    chain.consume_and_compact(3);
    ASSERT_NE(chain.front(), nullptr);
    EXPECT_EQ(chain.size(), 2u);
    EXPECT_EQ(readable_view(*chain.front()), "d");
    EXPECT_EQ(chain.readable_bytes(), 3u);
    EXPECT_EQ(chain.writable_bytes(), 4u);

    chain.consume_and_compact(3);
    EXPECT_TRUE(chain.empty());
    EXPECT_EQ(chain.size(), 0u);
    EXPECT_EQ(chain.front(), nullptr);
}

TEST(IoBufTest, ChainCommitBuildsWritableIovecs) {
    IoBuf a = IoBuf::allocate(4);
    IoBuf b = IoBuf::allocate(3);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);

    IoBufChain chain;
    ASSERT_TRUE(chain.append(std::move(a)));
    ASSERT_TRUE(chain.append(std::move(b)));
    EXPECT_EQ(chain.writable_bytes(), 7u);

    std::array<iovec, 4> iov{};
    int count = chain.fill_read_iov(iov.data(), static_cast<int>(iov.size()));
    ASSERT_EQ(count, 2);
    EXPECT_EQ(iov[0].iov_len, 4u);
    EXPECT_EQ(iov[1].iov_len, 3u);

    std::memcpy(iov[0].iov_base, "wxyz", 4);
    std::memcpy(iov[1].iov_base, "12", 2);
    chain.commit(6);

    EXPECT_EQ(chain.readable_bytes(), 6u);

    count = chain.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
    ASSERT_EQ(count, 2);
    EXPECT_EQ(std::string_view(static_cast<const char *>(iov[0].iov_base), iov[0].iov_len), "wxyz");
    EXPECT_EQ(std::string_view(static_cast<const char *>(iov[1].iov_base), iov[1].iov_len), "12");
}

TEST(IoBufTest, TakePrefixMovesWholeNodesAndAppendsToExistingDestination) {
    IoBuf a = IoBuf::allocate(4);
    IoBuf b = IoBuf::allocate(5);
    IoBuf c = IoBuf::allocate(4);
    IoBuf dst_buf = IoBuf::allocate(4);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    ASSERT_TRUE(c);
    ASSERT_TRUE(dst_buf);

    std::memcpy(a.writable_data(), "ab", 2);
    a.commit(2);
    std::memcpy(b.writable_data(), "cd", 2);
    b.commit(2);
    std::memcpy(c.writable_data(), "efg", 3);
    c.commit(3);
    std::memcpy(dst_buf.writable_data(), "xy", 2);
    dst_buf.commit(2);

    IoBufChain src;
    IoBufChain dst;
    ASSERT_TRUE(src.append(std::move(a)));
    ASSERT_TRUE(src.append(std::move(b)));
    ASSERT_TRUE(src.append(std::move(c)));
    ASSERT_TRUE(dst.append(std::move(dst_buf)));

    ASSERT_TRUE(src.take_prefix(4, dst));

    EXPECT_EQ(readable_string(src), "efg");
    EXPECT_EQ(readable_string(dst), "xyabcd");
    EXPECT_EQ(src.size(), 1u);
    EXPECT_EQ(dst.size(), 3u);
    EXPECT_EQ(src.readable_bytes(), 3u);
    EXPECT_EQ(dst.readable_bytes(), 6u);
    EXPECT_EQ(src.writable_bytes(), 1u);
    EXPECT_EQ(dst.writable_bytes(), 7u);
}

TEST(IoBufTest, TakePrefixSplitsBoundaryNodeUsingRetainedSlice) {
    IoBuf buf = IoBuf::allocate(8);
    ASSERT_TRUE(buf);

    std::memcpy(buf.writable_data(), "abcdef", 6);
    buf.commit(6);

    IoBufChain src;
    IoBufChain dst;
    ASSERT_TRUE(src.append(std::move(buf)));

    ASSERT_TRUE(src.take_prefix(3, dst));

    ASSERT_NE(src.front(), nullptr);
    ASSERT_NE(dst.front(), nullptr);
    EXPECT_EQ(readable_string(src), "def");
    EXPECT_EQ(readable_string(dst), "abc");
    EXPECT_EQ(src.size(), 1u);
    EXPECT_EQ(dst.size(), 1u);
    EXPECT_EQ(src.readable_bytes(), 3u);
    EXPECT_EQ(dst.readable_bytes(), 3u);
    EXPECT_EQ(src.writable_bytes(), 2u);
    EXPECT_EQ(dst.writable_bytes(), 0u);
    EXPECT_EQ(src.front()->use_count(), 2u);
    EXPECT_EQ(dst.front()->use_count(), 2u);
}

TEST(IoBufTest, TakePrefixSkipsEmptyReadableNodes) {
    IoBuf empty = IoBuf::allocate(4);
    IoBuf full = IoBuf::allocate(4);
    IoBuf tail = IoBuf::allocate(4);
    ASSERT_TRUE(empty);
    ASSERT_TRUE(full);
    ASSERT_TRUE(tail);

    std::memcpy(full.writable_data(), "ab", 2);
    full.commit(2);
    std::memcpy(tail.writable_data(), "cd", 2);
    tail.commit(2);

    IoBufChain src;
    IoBufChain dst;
    ASSERT_TRUE(src.append(std::move(empty)));
    ASSERT_TRUE(src.append(std::move(full)));
    ASSERT_TRUE(src.append(std::move(tail)));

    ASSERT_TRUE(src.take_prefix(3, dst));

    EXPECT_EQ(readable_string(src), "d");
    EXPECT_EQ(readable_string(dst), "abc");
    EXPECT_EQ(src.size(), 2u);
    EXPECT_EQ(dst.size(), 2u);
    EXPECT_EQ(src.readable_bytes(), 1u);
    EXPECT_EQ(dst.readable_bytes(), 3u);
    EXPECT_EQ(src.writable_bytes(), 6u);
    EXPECT_EQ(dst.writable_bytes(), 2u);
}

} // namespace
