#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <string_view>
#include <sys/uio.h>

#include <google/protobuf/message_lite.h>

#include "common/mem/IoBuf.h"
#include "common/mem/IoBufChain.h"
#include "grpc/ProtoCodec.h"
#include "helloworld.pb.h"

namespace {

using fiber::grpc::decode;
using fiber::grpc::encode;
using fiber::mem::IoBuf;
using fiber::mem::IoBufChain;
using fiber::mem::IoBufNodePool;

TEST(GrpcProtoCodecTest, EncodeDecodeRoundTrip) {
    helloworld::HelloRequest req;
    req.set_name("fiber");
    req.set_num(42);
    req.add_items("a");
    req.add_items("bb");
    req.add_items("ccc");

    IoBufNodePool node_pool;
    auto enc = encode(node_pool, req);
    ASSERT_TRUE(enc.has_value()) << "encode failed";
    ASSERT_EQ(enc->size(), 1u); // single-node chain
    EXPECT_GT(enc->readable_bytes(), 0u);

    helloworld::HelloRequest parsed;
    auto dec = decode(*enc, parsed);
    ASSERT_TRUE(dec.has_value()) << "decode failed";
    EXPECT_EQ(parsed.name(), "fiber");
    EXPECT_EQ(parsed.num(), 42);
    ASSERT_EQ(parsed.items_size(), 3);
    EXPECT_EQ(parsed.items(0), "a");
    EXPECT_EQ(parsed.items(1), "bb");
    EXPECT_EQ(parsed.items(2), "ccc");
}

TEST(GrpcProtoCodecTest, EncodeEmptyMessage) {
    helloworld::HelloRequest req; // default => ByteSizeLong() == 0
    IoBufNodePool node_pool;
    auto enc = encode(node_pool, req);
    ASSERT_TRUE(enc.has_value());
    EXPECT_EQ(enc->readable_bytes(), 0u);

    helloworld::HelloRequest parsed;
    auto dec = decode(*enc, parsed);
    ASSERT_TRUE(dec.has_value());
    EXPECT_EQ(parsed.name(), "");
    EXPECT_EQ(parsed.items_size(), 0);
}

TEST(GrpcProtoCodecTest, DecodeContiguousBytes) {
    helloworld::HelloReply rep;
    rep.set_message("hello");
    rep.set_count(12345);
    std::string raw;
    ASSERT_TRUE(rep.SerializeToString(&raw));

    helloworld::HelloReply parsed;
    auto dec = decode(std::string_view(raw), parsed);
    ASSERT_TRUE(dec.has_value());
    EXPECT_EQ(parsed.message(), "hello");
    EXPECT_EQ(parsed.count(), 12345);
}

TEST(GrpcProtoCodecTest, DecodeRejectsGarbage) {
    helloworld::HelloReply parsed;
    auto dec = decode(std::string_view("\xff\xff\xff\xff not protobuf"), parsed);
    EXPECT_FALSE(dec.has_value());
}

TEST(GrpcProtoCodecTest, MultiSegmentChainDecode) {
    helloworld::HelloRequest req;
    req.set_name("multi-segment");
    req.set_num(7);
    IoBufNodePool node_pool;
    auto enc = encode(node_pool, req);
    ASSERT_TRUE(enc.has_value());
    ASSERT_EQ(enc->size(), 1u);

    const IoBuf *src = enc->front();
    ASSERT_NE(src, nullptr);
    const std::size_t total = src->readable();
    ASSERT_GE(total, 2u);
    const std::uint8_t *base = src->readable_data();
    const std::size_t half = total / 2;

    // Split the single encoded node into two separately-allocated nodes so the
    // multi-segment coalescing path in decode() is exercised.
    IoBufChain multi(node_pool);
    IoBuf a = IoBuf::allocate(half);
    ASSERT_TRUE(a);
    std::memcpy(a.writable_data(), base, half);
    a.commit(half);
    IoBuf b = IoBuf::allocate(total - half);
    ASSERT_TRUE(b);
    std::memcpy(b.writable_data(), base + half, total - half);
    b.commit(total - half);
    ASSERT_TRUE(multi.append(std::move(a)));
    ASSERT_TRUE(multi.append(std::move(b)));
    ASSERT_EQ(multi.size(), 2u);
    ASSERT_EQ(multi.readable_bytes(), total);
    ASSERT_NE(multi.first_readable()->readable(), total); // confirms >1 segment

    helloworld::HelloRequest parsed;
    auto dec = decode(multi, parsed);
    ASSERT_TRUE(dec.has_value()) << "multi-segment decode failed";
    EXPECT_EQ(parsed.name(), "multi-segment");
    EXPECT_EQ(parsed.num(), 7);
}

TEST(GrpcProtoCodecTest, LargeMessageRoundTrip) {
    helloworld::HelloRequest req;
    req.set_name(std::string(128 * 1024, 'x')); // 128 KiB name
    for (int i = 0; i < 4; ++i) {
        req.add_items(std::string(32 * 1024, 'y'));
    }

    IoBufNodePool node_pool;
    auto enc = encode(node_pool, req);
    ASSERT_TRUE(enc.has_value());
    EXPECT_EQ(enc->readable_bytes(), req.ByteSizeLong());

    helloworld::HelloRequest parsed;
    auto dec = decode(*enc, parsed);
    ASSERT_TRUE(dec.has_value());
    EXPECT_EQ(parsed.name(), std::string(128 * 1024, 'x'));
    ASSERT_EQ(parsed.items_size(), 4);
    EXPECT_EQ(parsed.items(0), std::string(32 * 1024, 'y'));
}

} // namespace
