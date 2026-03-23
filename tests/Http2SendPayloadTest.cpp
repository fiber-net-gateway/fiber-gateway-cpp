#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>

#include "common/mem/IoBuf.h"

#define private public
#include "http/Http2SendPayload.h"
#undef private

namespace {

using fiber::http::Http2SendPayload;
using fiber::mem::IoBuf;
using fiber::mem::IoBufChain;

std::string chain_to_string(const IoBufChain &chain) {
    std::array<iovec, 16> iov{};
    int count = chain.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
    std::string out;
    for (int i = 0; i < count; ++i) {
        out.append(static_cast<const char *>(iov[i].iov_base), iov[i].iov_len);
    }
    return out;
}

TEST(Http2SendPayloadTest, SplitPrefixToMovesWholeIoBufChainNodes) {
    IoBuf first = IoBuf::allocate(4);
    IoBuf second = IoBuf::allocate(5);
    IoBuf third = IoBuf::allocate(4);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_TRUE(third);

    std::memcpy(first.writable_data(), "ab", 2);
    first.commit(2);
    std::memcpy(second.writable_data(), "cd", 2);
    second.commit(2);
    std::memcpy(third.writable_data(), "efg", 3);
    third.commit(3);

    IoBufChain chain;
    ASSERT_TRUE(chain.append(std::move(first)));
    ASSERT_TRUE(chain.append(std::move(second)));
    ASSERT_TRUE(chain.append(std::move(third)));

    Http2SendPayload payload;
    payload.set_chain(std::move(chain));

    Http2SendPayload prefix;
    ASSERT_TRUE(payload.split_prefix_to(4, prefix));

    ASSERT_EQ(payload.kind(), Http2SendPayload::Kind::IoBufChain);
    ASSERT_EQ(prefix.kind(), Http2SendPayload::Kind::IoBufChain);
    EXPECT_EQ(chain_to_string(payload.chain()), "efg");
    EXPECT_EQ(chain_to_string(prefix.chain()), "abcd");
    EXPECT_EQ(payload.chain().size(), 1u);
    EXPECT_EQ(prefix.chain().size(), 2u);
    EXPECT_EQ(payload.chain().writable_bytes(), 1u);
    EXPECT_EQ(prefix.chain().writable_bytes(), 5u);
    ASSERT_NE(prefix.chain().front(), nullptr);
    EXPECT_EQ(prefix.chain().front()->use_count(), 1u);
}

} // namespace
