#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <string_view>

#include "common/mem/IoBuf.h"
#include "common/mem/IoBufChain.h"
#include "grpc/GrpcFraming.h"

namespace {

using fiber::common::IoErr;
using fiber::grpc::frame;
using fiber::grpc::GrpcFrameReader;
using fiber::mem::IoBuf;
using fiber::mem::IoBufChain;
using fiber::mem::IoBufNodePool;

IoBufChain make_payload(IoBufNodePool &pool, std::string_view data) {
    IoBufChain chain(pool);
    if (!data.empty()) {
        IoBuf buf = IoBuf::allocate(data.size());
        std::memcpy(buf.writable_data(), data.data(), data.size());
        buf.commit(data.size());
        chain.append(std::move(buf));
    }
    return chain;
}

IoBufChain make_multi_payload(IoBufNodePool &pool, std::string_view a, std::string_view b) {
    IoBufChain chain(pool);
    IoBuf first = IoBuf::allocate(a.size());
    std::memcpy(first.writable_data(), a.data(), a.size());
    first.commit(a.size());
    chain.append(std::move(first));
    IoBuf second = IoBuf::allocate(b.size());
    std::memcpy(second.writable_data(), b.data(), b.size());
    second.commit(b.size());
    chain.append(std::move(second));
    return chain;
}

TEST(GrpcFramingTest, FrameHeaderIsCorrectBigEndian) {
    IoBufNodePool pool;
    const std::string payload(300, 'x'); // length needs 2 bytes
    auto framed = frame(make_payload(pool, payload));
    ASSERT_TRUE(framed.has_value());
    ASSERT_EQ(framed->readable_bytes(), 5u + payload.size());

    const IoBuf *head = framed->front();
    ASSERT_NE(head, nullptr);
    const auto *p = head->readable_data();
    EXPECT_EQ(p[0], 0); // no compression
    EXPECT_EQ(p[1], 0);
    EXPECT_EQ(p[2], 0);
    EXPECT_EQ(p[3], 0x01);
    EXPECT_EQ(p[4], 0x2c); // 300 = 0x012c
}

TEST(GrpcFramingTest, FrameAndDeframeRoundTrip) {
    IoBufNodePool pool;
    const std::string payload(100, 'y');
    auto framed = frame(make_payload(pool, payload));
    ASSERT_TRUE(framed.has_value());

    GrpcFrameReader reader;
    ASSERT_TRUE(reader.append(*framed).has_value());
    EXPECT_EQ(reader.buffered_bytes(), 5u + payload.size());

    std::string out;
    auto r = reader.next_payload(out);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(*r);
    EXPECT_EQ(out, payload);

    auto r2 = reader.next_payload(out);
    ASSERT_TRUE(r2.has_value());
    EXPECT_FALSE(*r2);
    EXPECT_EQ(reader.buffered_bytes(), 0u);
}

TEST(GrpcFramingTest, EmptyMessageFrame) {
    IoBufNodePool pool;
    auto framed = frame(make_payload(pool, ""));
    ASSERT_TRUE(framed.has_value());
    ASSERT_EQ(framed->readable_bytes(), 5u);

    GrpcFrameReader reader;
    ASSERT_TRUE(reader.append(*framed).has_value());
    std::string out;
    auto r = reader.next_payload(out);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(*r);
    EXPECT_TRUE(out.empty());
}

TEST(GrpcFramingTest, MultiSegmentAppendDeframes) {
    IoBufNodePool pool;
    const std::string payload("the quick brown fox");
    // Split the framed bytes across two nodes in one chain.
    // Build the full framed wire bytes (5-byte header + payload), then split
    // them across two nodes so the multi-segment append path is exercised.
    const std::uint32_t plen = static_cast<std::uint32_t>(payload.size());
    std::string framed_bytes;
    framed_bytes.push_back('\x00');
    framed_bytes.push_back(static_cast<char>((plen >> 24) & 0xff));
    framed_bytes.push_back(static_cast<char>((plen >> 16) & 0xff));
    framed_bytes.push_back(static_cast<char>((plen >> 8) & 0xff));
    framed_bytes.push_back(static_cast<char>(plen & 0xff));
    framed_bytes.append(payload);
    const auto *base = reinterpret_cast<const std::uint8_t *>(framed_bytes.data());
    const std::size_t total = framed_bytes.size();

    IoBufChain split(pool);
    const std::size_t half = total / 2;
    IoBuf a = IoBuf::allocate(half);
    std::memcpy(a.writable_data(), base, half);
    a.commit(half);
    IoBuf b = IoBuf::allocate(total - half);
    std::memcpy(b.writable_data(), base + half, total - half);
    b.commit(total - half);
    split.append(std::move(a));
    split.append(std::move(b));
    ASSERT_EQ(split.size(), 2u);
    ASSERT_NE(split.first_readable()->readable(), split.readable_bytes());

    GrpcFrameReader reader;
    ASSERT_TRUE(reader.append(split).has_value());
    std::string out;
    auto r = reader.next_payload(out);
    ASSERT_TRUE(r.has_value() && *r);
    EXPECT_EQ(out, payload);
}

TEST(GrpcFramingTest, MultipleFramesInOneBuffer) {
    IoBufNodePool pool;
    const std::string p1("first");
    const std::string p2("second-longer");

    GrpcFrameReader reader;
    ASSERT_TRUE(reader.append(*frame(make_payload(pool, p1))).has_value());
    ASSERT_TRUE(reader.append(*frame(make_payload(pool, p2))).has_value());

    std::string out;
    auto r1 = reader.next_payload(out);
    ASSERT_TRUE(r1.has_value() && *r1);
    EXPECT_EQ(out, p1);
    auto r2 = reader.next_payload(out);
    ASSERT_TRUE(r2.has_value() && *r2);
    EXPECT_EQ(out, p2);
    auto r3 = reader.next_payload(out);
    ASSERT_TRUE(r3.has_value());
    EXPECT_FALSE(*r3);
}

TEST(GrpcFramingTest, PartialFrameNeedsMore) {
    GrpcFrameReader reader;
    std::string out;
    // 3 bytes of header only.
    ASSERT_TRUE(reader.append(std::string_view("\x00\x00\x00", 3)).has_value());
    auto r = reader.next_payload(out);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(*r);

    // Remaining header byte + length=2 + payload "hi".
    ASSERT_TRUE(reader.append(std::string_view("\x00\x02hi", 4)).has_value());
    auto r2 = reader.next_payload(out);
    ASSERT_TRUE(r2.has_value());
    EXPECT_TRUE(*r2);
    EXPECT_EQ(out, "hi");
}

TEST(GrpcFramingTest, RejectsCompressedFlag) {
    GrpcFrameReader reader;
    std::string bad;
    bad.push_back('\x01'); // compressed
    bad.append("\x00\x00\x00\x00", 4);
    ASSERT_TRUE(reader.append(bad).has_value());
    std::string out;
    auto r = reader.next_payload(out);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), IoErr::NotSupported);
}

TEST(GrpcFramingTest, OversizedLengthWaitsForBytes) {
    GrpcFrameReader reader;
    // flag=0, length=0x10 but no payload bytes yet.
    ASSERT_TRUE(reader.append(std::string_view("\x00\x00\x00\x00\x10", 5)).has_value());
    std::string out;
    auto r = reader.next_payload(out);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(*r); // need 16 payload bytes
}

TEST(GrpcFramingTest, FrameRequiresBoundChain) {
    IoBufChain unbound; // default-constructed, not bound to a node pool
    auto r = frame(std::move(unbound));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), IoErr::Invalid);
}

TEST(GrpcFramingTest, FramePreservesMultiNodePayload) {
    IoBufNodePool pool;
    const std::string a("aaaa");
    const std::string b("bbbb");
    auto framed = frame(make_multi_payload(pool, a, b));
    ASSERT_TRUE(framed.has_value());
    EXPECT_EQ(framed->readable_bytes(), 5u + a.size() + b.size());
    EXPECT_EQ(framed->size(), 3u); // header + two payload nodes

    GrpcFrameReader reader;
    ASSERT_TRUE(reader.append(*framed).has_value());
    std::string out;
    auto r = reader.next_payload(out);
    ASSERT_TRUE(r.has_value() && *r);
    EXPECT_EQ(out, a + b);
}

} // namespace
