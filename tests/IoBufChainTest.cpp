#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

#include <fiber/common/mem/IoBufChain.h>
#include "LoopTestSupport.h"

namespace {

using fiber::mem::IoBuf;
using fiber::mem::IoBufChain;
using fiber::mem::IoBufNodePool;

std::string readable_string(const IoBufChain &chain) {
    std::array<iovec, 16> iov{};
    int count = chain.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
    std::string out;
    for (int i = 0; i < count; ++i) {
        out.append(static_cast<const char *>(iov[i].iov_base), iov[i].iov_len);
    }
    return out;
}

static_assert(!std::is_copy_constructible_v<IoBufChain>);
static_assert(std::is_move_constructible_v<IoBufChain>);

TEST(IoBufChainTest, ChainExportsReadableAndWritableIovecs) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
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
    });
}

TEST(IoBufChainTest, NodePoolResetsNodeOnReuse) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
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
    });
}

TEST(IoBufChainTest, ChainAppendNodeTakesOwnershipAndResetsOwnerFields) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
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
    });
}

TEST(IoBufChainTest, AppendChainMovesNodesAndCompletion) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        IoBuf first = IoBuf::allocate(4);
        IoBuf second = IoBuf::allocate(5);
        IoBuf prefix = IoBuf::allocate(4);
        ASSERT_TRUE(first);
        ASSERT_TRUE(second);
        ASSERT_TRUE(prefix);

        std::memcpy(prefix.writable_data(), "xy", 2);
        prefix.commit(2);
        std::memcpy(first.writable_data(), "ab", 2);
        first.commit(2);
        std::memcpy(second.writable_data(), "cde", 3);
        second.commit(3);

        IoBufChain src;
        IoBufChain dst;
        ASSERT_TRUE(src.append(std::move(first)));
        ASSERT_TRUE(src.append(std::move(second)));
        ASSERT_TRUE(dst.append(std::move(prefix)));
        src.mark_complete();

        ASSERT_TRUE(dst.append_chain(std::move(src)));

        EXPECT_EQ(readable_string(dst), "xyabcde");
        EXPECT_EQ(dst.size(), 3u);
        EXPECT_EQ(dst.readable_bytes(), 7u);
        EXPECT_TRUE(dst.complete());
        EXPECT_TRUE(src.empty());
        EXPECT_EQ(src.readable_bytes(), 0u);
        EXPECT_FALSE(src.complete());
    });
}

TEST(IoBufChainTest, AppendChainTransfersEmptyCompletionMarker) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        IoBufChain src;
        IoBufChain dst;
        src.mark_complete();

        ASSERT_TRUE(dst.append_chain(std::move(src)));

        EXPECT_TRUE(dst.empty());
        EXPECT_TRUE(dst.complete());
        EXPECT_FALSE(src.complete());
    });
}

TEST(IoBufChainTest, AppendChainIntoEmptyDestinationMovesNodes) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        IoBuf buf = IoBuf::allocate(4);
        ASSERT_TRUE(buf);
        std::memcpy(buf.writable_data(), "abc", 3);
        buf.commit(3);

        IoBufChain src;
        IoBufChain dst;
        ASSERT_TRUE(src.append(std::move(buf)));

        ASSERT_TRUE(dst.append_chain(std::move(src)));

        EXPECT_EQ(readable_string(dst), "abc");
    });
}

TEST(IoBufChainTest, AppendChainRejectsCompletedDestination) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        IoBuf buf = IoBuf::allocate(4);
        ASSERT_TRUE(buf);
        std::memcpy(buf.writable_data(), "ab", 2);
        buf.commit(2);

        IoBufChain src;
        IoBufChain completed;
        ASSERT_TRUE(src.append(std::move(buf)));
        completed.mark_complete();

        EXPECT_FALSE(completed.append_chain(std::move(src)));
        EXPECT_EQ(readable_string(src), "ab");
    });
}

TEST(IoBufChainTest, DropEmptyFrontRemovesOnlyDrainedPrefix) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
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
        EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(chain.front()->readable_data()),
                                   chain.front()->readable()),
                  "cd");
        EXPECT_EQ(chain.writable_bytes(), 2u);
    });
}

TEST(IoBufChainTest, ConsumeAndCompactDropsFullyConsumedFrontNodes) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
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
        EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(chain.front()->readable_data()),
                                   chain.front()->readable()),
                  "d");
        EXPECT_EQ(chain.size(), 2u);
        EXPECT_EQ(chain.readable_bytes(), 3u);
        EXPECT_EQ(chain.writable_bytes(), 4u);

        chain.consume_and_compact(3);
        EXPECT_TRUE(chain.empty());
        EXPECT_EQ(chain.size(), 0u);
        EXPECT_EQ(chain.front(), nullptr);
    });
}

TEST(IoBufChainTest, ChainCommitBuildsWritableIovecs) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
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
    });
}

TEST(IoBufChainTest, CommitBackOnlyCommitsPhysicalTail) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        IoBuf first = IoBuf::allocate(4);
        IoBuf last = IoBuf::allocate(4);
        ASSERT_TRUE(first);
        ASSERT_TRUE(last);

        IoBufChain chain;
        ASSERT_TRUE(chain.append(std::move(first)));
        ASSERT_TRUE(chain.append(std::move(last)));
        ASSERT_NE(chain.front(), nullptr);
        ASSERT_NE(chain.back(), nullptr);
        ASSERT_NE(chain.front(), chain.back());

        std::memcpy(chain.back()->writable_data(), "xy", 2);
        chain.commit_back(2);

        EXPECT_EQ(chain.front()->readable(), 0u);
        EXPECT_EQ(chain.front()->writable(), 4u);
        EXPECT_EQ(chain.back()->readable(), 2u);
        EXPECT_EQ(chain.back()->writable(), 2u);
        EXPECT_EQ(chain.readable_bytes(), 2u);
        EXPECT_EQ(chain.writable_bytes(), 6u);
        EXPECT_EQ(readable_string(chain), "xy");
    });
}

TEST(IoBufChainTest, TakePrefixMovesWholeNodesAndAppendsToExistingDestination) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
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
    });
}

TEST(IoBufChainTest, TakePrefixIntoEmptyDestinationSplicesNodes) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        IoBuf buf = IoBuf::allocate(4);
        ASSERT_TRUE(buf);

        std::memcpy(buf.writable_data(), "abc", 3);
        buf.commit(3);

        IoBufChain src;
        IoBufChain dst;
        ASSERT_TRUE(src.append(std::move(buf)));

        ASSERT_TRUE(src.take_prefix(2, dst));

        EXPECT_EQ(readable_string(src), "c");
        EXPECT_EQ(readable_string(dst), "ab");
    });
}

TEST(IoBufChainTest, RetainPrefixIntoEmptyOutputSplicesNodes) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        IoBuf buf = IoBuf::allocate(8);
        ASSERT_TRUE(buf);

        std::memcpy(buf.writable_data(), "abcd", 4);
        buf.commit(4);

        IoBufChain src;
        IoBufChain out;
        ASSERT_TRUE(src.append(std::move(buf)));

        ASSERT_TRUE(src.retain_prefix(3, out));

        EXPECT_EQ(readable_string(src), "abcd");
        EXPECT_EQ(readable_string(out), "abc");
    });
}

TEST(IoBufChainTest, TakePrefixSplitsBoundaryNodeUsingRetainedSlice) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
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
    });
}

TEST(IoBufChainTest, TakePrefixSkipsEmptyReadableNodes) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
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
    });
}

TEST(IoBufChainTest, TakePrefixTransfersCompleteOnlyWithFullReadableRange) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        IoBuf buf = IoBuf::allocate(8);
        ASSERT_TRUE(buf);
        std::memcpy(buf.writable_data(), "abcdef", 6);
        buf.commit(6);

        IoBufChain src;
        IoBufChain first;
        IoBufChain second;
        ASSERT_TRUE(src.append(std::move(buf)));
        src.mark_complete();

        ASSERT_TRUE(src.take_prefix(3, first));
        EXPECT_FALSE(first.complete());
        EXPECT_TRUE(src.complete());

        ASSERT_TRUE(src.take_prefix(3, second));
        EXPECT_TRUE(second.complete());
        EXPECT_FALSE(src.complete());
        EXPECT_EQ(readable_string(first), "abc");
        EXPECT_EQ(readable_string(second), "def");
    });
}

TEST(IoBufChainTest, EmptyCompleteCanBeTransferred) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        IoBufChain src;
        IoBufChain dst;
        src.mark_complete();

        ASSERT_TRUE(src.take_prefix(0, dst));
        EXPECT_FALSE(src.complete());
        EXPECT_TRUE(dst.complete());
    });
}

TEST(IoBufChainTest, TrimEndShrinksTailNodeInPlace) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        IoBufChain chain;
        ASSERT_TRUE(chain.append(IoBuf::allocate(16)));
        chain.commit_back(8); // "committed" node: 8 readable, 8 writable-accounted

        chain.trim_end(3);
        EXPECT_EQ(chain.readable_bytes(), 5u);
        EXPECT_EQ(chain.size(), 1u);
        EXPECT_EQ(chain.back()->readable(), 5u);
        // Trimmed tail returns to the writable accounting.
        EXPECT_EQ(chain.writable_bytes(), 11u);
    });
}

TEST(IoBufChainTest, TrimEndReleasesFullyTrimmedTailNodes) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        IoBufChain chain;
        IoBuf buf1 = IoBuf::allocate(8);
        IoBuf buf2 = IoBuf::allocate(8);
        IoBuf buf3 = IoBuf::allocate(8);
        std::memcpy(buf1.writable_data(), "aaaa", 4);
        std::memcpy(buf2.writable_data(), "bbbb", 4);
        std::memcpy(buf3.writable_data(), "cccc", 4);
        buf1.commit(4);
        buf2.commit(4);
        buf3.commit(4);
        ASSERT_TRUE(chain.append(std::move(buf1)));
        ASSERT_TRUE(chain.append(std::move(buf2)));
        ASSERT_TRUE(chain.append(std::move(buf3)));

        // Trim 4 (all of node3) + 2 (half of node2).
        chain.trim_end(6);
        EXPECT_EQ(readable_string(chain), "aaaabb");
        EXPECT_EQ(chain.size(), 2u);
        EXPECT_EQ(chain.back()->readable(), 2u);

        // Trim everything: the chain empties and releases all nodes.
        chain.trim_end(6);
        EXPECT_TRUE(chain.empty());
        EXPECT_EQ(chain.readable_bytes(), 0u);
        EXPECT_EQ(chain.size(), 0u);
        EXPECT_EQ(chain.front(), nullptr);
        EXPECT_EQ(chain.back(), nullptr);
        EXPECT_EQ(readable_string(chain), "");
    });
}

TEST(IoBufChainTest, CommitTailroomPublishesBytesWrittenOutOfBand) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        IoBufChain chain;
        // A node delivered by a transport read: all bytes committed before the
        // node entered the chain; its tailroom is in the writable accounting.
        IoBuf buf = IoBuf::allocate(16);
        std::memcpy(buf.writable_data(), "payload", 7);
        buf.commit(7);
        ASSERT_TRUE(chain.append(std::move(buf)));
        ASSERT_EQ(chain.readable_bytes(), 7u);
        ASSERT_EQ(chain.writable_bytes(), 9u);

        // Write into the tailroom out-of-band (e.g. a seal in place), then
        // publish those bytes as readable.
        std::memcpy(chain.back()->writable_data(), "XY", 2);
        chain.commit_tailroom(2);

        EXPECT_EQ(readable_string(chain), "payloadXY");
        EXPECT_EQ(chain.readable_bytes(), 9u);
        EXPECT_EQ(chain.writable_bytes(), 7u);
    });
}

TEST(IoBufChainTest, FrontNodeWalksReadableSpansInOrder) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        IoBufChain empty;
        EXPECT_EQ(empty.front_node(), nullptr);

        IoBufChain chain;
        for (const char *piece: {"aaaa", "bb", "cccc"}) {
            IoBuf buf = IoBuf::allocate(std::strlen(piece));
            std::memcpy(buf.writable_data(), piece, std::strlen(piece));
            buf.commit(std::strlen(piece));
            ASSERT_TRUE(chain.append(std::move(buf)));
        }

        // The walk via front_node()/next covers exactly the readable bytes,
        // in chain order.
        std::size_t nodes = 0;
        std::string walked;
        for (const fiber::mem::IoBufNode *node = chain.front_node(); node != nullptr; node = node->next) {
            walked.append(reinterpret_cast<const char *>(node->buf.readable_data()), node->buf.readable());
            ++nodes;
        }
        EXPECT_EQ(nodes, chain.size());
        EXPECT_EQ(walked, "aaaabbcccc");
    });
}

} // namespace
