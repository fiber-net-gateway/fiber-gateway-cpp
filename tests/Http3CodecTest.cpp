#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "common/mem/IoBuf.h"
#include "common/mem/IoBufChain.h"
#include "http/Http3Codec.h"
#include "http/Http3Protocol.h"
#include "quic/QuicCursor.h"
#include "quic/QuicTransportCodec.h"

namespace {

void append_bytes(fiber::mem::IoBufChain &chain, const std::vector<std::uint8_t> &bytes, std::size_t offset,
                  std::size_t len) {
    fiber::mem::IoBuf buf = fiber::mem::IoBuf::allocate(len);
    ASSERT_TRUE(buf);
    if (len != 0) {
        std::memcpy(buf.writable_data(), bytes.data() + offset, len);
        buf.commit(len);
    }
    ASSERT_TRUE(chain.append(std::move(buf)));
}

void append_varint(std::vector<std::uint8_t> &out, std::uint64_t value) {
    std::array<std::uint8_t, 8> buf{};
    fiber::quic::QuicWriteCursor cursor(buf.data(), buf.size());
    ASSERT_TRUE(fiber::quic::quic_write_varint(cursor, value).has_value());
    out.insert(out.end(), buf.data(), buf.data() + cursor.offset());
}

} // namespace

TEST(Http3CodecTest, VarintParserSupportsSplitInput) {
    std::vector<std::uint8_t> bytes;
    append_varint(bytes, 15293);

    fiber::mem::IoBufNodePool pool;
    fiber::mem::IoBufChain chain(pool);
    fiber::http::Http3VarintParser parser;

    append_bytes(chain, bytes, 0, 1);
    EXPECT_EQ(parser.parse(chain), fiber::http::Http3ParseStatus::NeedMore);

    append_bytes(chain, bytes, 1, bytes.size() - 1);
    EXPECT_EQ(parser.parse(chain), fiber::http::Http3ParseStatus::Done);
    EXPECT_EQ(parser.value(), 15293U);
    EXPECT_EQ(chain.readable_bytes(), 0U);
}

TEST(Http3CodecTest, SettingsParserRejectsDuplicateParameter) {
    std::vector<std::uint8_t> payload;
    append_varint(payload, static_cast<std::uint64_t>(fiber::http::Http3SettingId::QpackBlockedStreams));
    append_varint(payload, 1);
    append_varint(payload, static_cast<std::uint64_t>(fiber::http::Http3SettingId::QpackBlockedStreams));
    append_varint(payload, 2);

    fiber::mem::IoBufNodePool pool;
    fiber::mem::IoBufChain chain(pool);
    append_bytes(chain, payload, 0, payload.size());

    fiber::http::Http3SettingsParser parser;
    parser.start(payload.size());
    EXPECT_EQ(parser.parse(chain), fiber::http::Http3ParseStatus::Error);
    EXPECT_EQ(parser.error().h3_error, fiber::http::Http3ErrorCode::SettingsError);
}

TEST(Http3CodecTest, SettingsParserWaitsForSplitValue) {
    std::vector<std::uint8_t> payload;
    append_varint(payload, static_cast<std::uint64_t>(fiber::http::Http3SettingId::QpackBlockedStreams));
    append_varint(payload, 128);

    fiber::mem::IoBufNodePool pool;
    fiber::mem::IoBufChain chain(pool);
    fiber::http::Http3SettingsParser parser;
    parser.start(payload.size());

    append_bytes(chain, payload, 0, payload.size() - 1);
    EXPECT_EQ(parser.parse(chain), fiber::http::Http3ParseStatus::NeedMore);

    append_bytes(chain, payload, payload.size() - 1, 1);
    EXPECT_EQ(parser.parse(chain), fiber::http::Http3ParseStatus::Done);
    EXPECT_EQ(parser.settings().qpack_blocked_streams, 128U);
}
