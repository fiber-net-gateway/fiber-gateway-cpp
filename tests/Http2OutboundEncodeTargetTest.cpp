#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include <fiber/common/mem/IoBuf.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/http/Http2Outbound.h>

#include "LoopTestSupport.h"

namespace {

using fiber::common::IoErr;
using fiber::http::Http2OutboundEncodeTarget;
using fiber::mem::IoBuf;
using fiber::mem::IoBufChain;

// A buffer of `capacity` holding `readable` bytes of `fill`, with the rest
// of its room set to 'z' so stray writes show up.
IoBuf make_buf(std::size_t capacity, std::size_t readable, char fill) {
    IoBuf buf = IoBuf::allocate(capacity);
    std::memset(buf.writable_data(), 'z', capacity);
    std::memset(buf.writable_data(), fill, readable);
    buf.commit(readable);
    return buf;
}

std::string chain_bytes(const IoBufChain &chain) {
    std::string out;
    for (const fiber::mem::IoBufNode *node = chain.front_node(); node != nullptr; node = node->next) {
        out.append(reinterpret_cast<const char *>(node->buf.readable_data()), node->buf.readable());
    }
    return out;
}

TEST(Http2OutboundEncodeTargetTest, PacksIntoUniqueTailRoom) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        IoBufChain chain;
        ASSERT_TRUE(chain.append(make_buf(64, 4, 'a')));
        Http2OutboundEncodeTarget target(chain);

        EXPECT_EQ(target.append_copy("bcd", 3), IoErr::None);

        EXPECT_EQ(chain.size(), 1U);
        EXPECT_EQ(target.total_bytes(), 3U);
        EXPECT_EQ(chain_bytes(chain), "aaaabcd");
    });
}

TEST(Http2OutboundEncodeTargetTest, LeavesSharedTailUntouched) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        IoBufChain chain;
        IoBuf tail = make_buf(64, 4, 'a');
        const IoBuf other_view = tail;
        ASSERT_TRUE(chain.append(std::move(tail)));
        Http2OutboundEncodeTarget target(chain);

        EXPECT_EQ(target.append_copy("bcd", 3), IoErr::None);

        EXPECT_EQ(chain.size(), 2U);
        EXPECT_EQ(chain_bytes(chain), "aaaabcd");
        EXPECT_EQ(other_view.readable_data()[other_view.readable()], 'z');
    });
}

TEST(Http2OutboundEncodeTargetTest, SliceTailHasNoRoom) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        IoBuf whole = make_buf(64, 8, 'a');
        IoBuf slice = whole.retain_slice(0, 4);
        whole = {};
        ASSERT_TRUE(slice.unique());
        IoBufChain chain;
        ASSERT_TRUE(chain.append(std::move(slice)));
        Http2OutboundEncodeTarget target(chain);

        EXPECT_EQ(target.append_copy("bcd", 3), IoErr::None);

        EXPECT_EQ(chain.size(), 2U);
        EXPECT_EQ(chain_bytes(chain), "aaaabcd");
    });
}

TEST(Http2OutboundEncodeTargetTest, FreshBufferTakesCapacityHintAndJoinsOnCommit) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        IoBufChain chain;
        Http2OutboundEncodeTarget target(chain);

        std::uint8_t *dst = nullptr;
        std::size_t room = 0;
        ASSERT_EQ(target.acquire(3, 256, dst, room), IoErr::None);
        EXPECT_GE(room, 256U);
        EXPECT_TRUE(chain.empty());
        std::memcpy(dst, "abc", 3);
        target.commit(3);
        ASSERT_EQ(chain.size(), 1U);

        // The next small write packs behind it.
        EXPECT_EQ(target.append_copy("de", 2), IoErr::None);
        EXPECT_EQ(chain.size(), 1U);
        EXPECT_EQ(chain_bytes(chain), "abcde");
    });
}

TEST(Http2OutboundEncodeTargetTest, UncommittedRoomNeverReachesTheChain) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        IoBufChain chain;
        {
            Http2OutboundEncodeTarget target(chain);
            std::uint8_t *dst = nullptr;
            std::size_t room = 0;
            ASSERT_EQ(target.acquire(8, 64, dst, room), IoErr::None);
        }
        // An empty node would make the chain look non-empty with nothing to write.
        EXPECT_TRUE(chain.empty());
    });
}

TEST(Http2OutboundEncodeTargetTest, RollbackRestoresBorrowedTailAndDropsNewNodes) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        IoBufChain chain;
        ASSERT_TRUE(chain.append(make_buf(64, 4, 'a')));
        Http2OutboundEncodeTarget target(chain);

        ASSERT_EQ(target.append_copy("bcd", 3), IoErr::None);
        IoBufChain payload;
        ASSERT_TRUE(payload.append(make_buf(16, 16, 'p')));
        ASSERT_TRUE(payload.append(make_buf(16, 16, 'q')));
        ASSERT_EQ(target.append_chain(std::move(payload)), IoErr::None);
        std::uint8_t *dst = nullptr;
        std::size_t room = 0;
        ASSERT_EQ(target.acquire(4, 4, dst, room), IoErr::None);

        target.rollback();

        EXPECT_EQ(target.total_bytes(), 0U);
        ASSERT_EQ(chain.size(), 1U);
        EXPECT_EQ(chain_bytes(chain), "aaaa");
        EXPECT_EQ(chain.back()->writable(), 60U);
    });
}

TEST(Http2OutboundEncodeTargetTest, AppendChainDropsCompletionMarker) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        IoBufChain chain;
        Http2OutboundEncodeTarget target(chain);
        IoBufChain payload;
        ASSERT_TRUE(payload.append(make_buf(8, 8, 'p')));
        payload.mark_complete();

        ASSERT_EQ(target.append_chain(std::move(payload)), IoErr::None);

        EXPECT_FALSE(chain.complete());
        EXPECT_EQ(chain_bytes(chain), "pppppppp");
    });
}

} // namespace
