#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "common/mem/IoBuf.h"
#include "common/mem/IoBufChain.h"
#include "http/Http3QpackControlStreamDecoder.h"

namespace {

void append_chain(fiber::mem::IoBufChain &chain, const std::vector<std::uint8_t> &bytes) {
    fiber::mem::IoBuf buf = fiber::mem::IoBuf::allocate(bytes.size());
    ASSERT_TRUE(buf);
    if (!bytes.empty()) {
        std::memcpy(buf.writable_data(), bytes.data(), bytes.size());
        buf.commit(bytes.size());
    }
    ASSERT_TRUE(chain.append(std::move(buf)));
}

void append_prefixed_integer(std::vector<std::uint8_t> &out, std::uint8_t prefix_mask, std::uint8_t prefix_bits,
                             std::uint64_t value) {
    const std::uint8_t prefix_max = static_cast<std::uint8_t>((1U << prefix_bits) - 1U);
    if (value < prefix_max) {
        out.push_back(static_cast<std::uint8_t>(prefix_mask | value));
        return;
    }

    out.push_back(static_cast<std::uint8_t>(prefix_mask | prefix_max));
    value -= prefix_max;
    while (value >= 0x80U) {
        out.push_back(static_cast<std::uint8_t>((value & 0x7fU) | 0x80U));
        value >>= 7U;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

} // namespace

TEST(Http3QpackControlStreamDecoderTest, EncoderAcceptsCapacityZero) {
    fiber::mem::IoBufNodePool pool;
    fiber::mem::IoBufChain chain(pool);
    append_chain(chain, {0x20});

    fiber::http::Http3QpackEncoderStreamDecoder decoder;
    EXPECT_EQ(decoder.parse(chain), fiber::http::Http3ParseStatus::NeedMore);
    EXPECT_EQ(chain.readable_bytes(), 0U);
}

TEST(Http3QpackControlStreamDecoderTest, EncoderRejectsCapacityNonZero) {
    fiber::mem::IoBufNodePool pool;
    fiber::mem::IoBufChain chain(pool);
    append_chain(chain, {0x21});

    fiber::http::Http3QpackEncoderStreamDecoder decoder;
    ASSERT_EQ(decoder.parse(chain), fiber::http::Http3ParseStatus::Error);
    EXPECT_EQ(decoder.error().h3_error, fiber::http::Http3ErrorCode::QpackEncoderStreamError);
}

TEST(Http3QpackControlStreamDecoderTest, EncoderRejectsInsertInstruction) {
    fiber::mem::IoBufNodePool pool;
    fiber::mem::IoBufChain chain(pool);
    append_chain(chain, {0x80});

    fiber::http::Http3QpackEncoderStreamDecoder decoder;
    ASSERT_EQ(decoder.parse(chain), fiber::http::Http3ParseStatus::Error);
    EXPECT_EQ(decoder.error().h3_error, fiber::http::Http3ErrorCode::QpackEncoderStreamError);
}

TEST(Http3QpackControlStreamDecoderTest, DecoderAcceptsStreamCancellation) {
    std::vector<std::uint8_t> bytes;
    append_prefixed_integer(bytes, 0x40, 6, 67);

    fiber::mem::IoBufNodePool pool;
    fiber::mem::IoBufChain chain(pool);
    append_chain(chain, bytes);

    fiber::http::Http3QpackDecoderStreamDecoder decoder;
    fiber::http::Http3QpackDecoderStreamEvent event;
    ASSERT_EQ(decoder.parse(chain, event), fiber::http::Http3ParseStatus::Done);
    EXPECT_EQ(event.type, fiber::http::Http3QpackDecoderStreamEventType::StreamCancellation);
    EXPECT_EQ(event.stream_id, 67U);
}

TEST(Http3QpackControlStreamDecoderTest, DecoderRejectsSectionAcknowledgement) {
    fiber::mem::IoBufNodePool pool;
    fiber::mem::IoBufChain chain(pool);
    append_chain(chain, {0x80});

    fiber::http::Http3QpackDecoderStreamDecoder decoder;
    fiber::http::Http3QpackDecoderStreamEvent event;
    ASSERT_EQ(decoder.parse(chain, event), fiber::http::Http3ParseStatus::Error);
    EXPECT_EQ(decoder.error().h3_error, fiber::http::Http3ErrorCode::QpackDecoderStreamError);
}

TEST(Http3QpackControlStreamDecoderTest, DecoderRejectsInsertCountIncrement) {
    fiber::mem::IoBufNodePool pool;
    fiber::mem::IoBufChain chain(pool);
    append_chain(chain, {0x00});

    fiber::http::Http3QpackDecoderStreamDecoder decoder;
    fiber::http::Http3QpackDecoderStreamEvent event;
    ASSERT_EQ(decoder.parse(chain, event), fiber::http::Http3ParseStatus::Error);
    EXPECT_EQ(decoder.error().h3_error, fiber::http::Http3ErrorCode::QpackDecoderStreamError);
}
