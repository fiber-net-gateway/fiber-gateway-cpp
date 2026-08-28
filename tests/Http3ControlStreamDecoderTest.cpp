#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include <fiber/common/mem/IoBuf.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/http/Http3Protocol.h>
#include <fiber/quic/QuicCursor.h>
#include "http/Http3ControlStreamDecoder.h"
#include "quic/QuicTransportCodec.h"

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

void append_varint(std::vector<std::uint8_t> &out, std::uint64_t value) {
    std::array<std::uint8_t, 8> buf{};
    fiber::quic::QuicWriteCursor cursor(buf.data(), buf.size());
    ASSERT_TRUE(fiber::quic::quic_write_varint(cursor, value).has_value());
    out.insert(out.end(), buf.data(), buf.data() + cursor.offset());
}

void append_frame(std::vector<std::uint8_t> &out, std::uint64_t type, const std::vector<std::uint8_t> &payload = {}) {
    append_varint(out, type);
    append_varint(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
}

std::vector<std::uint8_t> settings_payload(std::uint64_t blocked_streams) {
    std::vector<std::uint8_t> payload;
    append_varint(payload, static_cast<std::uint64_t>(fiber::http::Http3SettingId::QpackBlockedStreams));
    append_varint(payload, blocked_streams);
    return payload;
}

} // namespace

TEST(Http3ControlStreamDecoderTest, ParsesInitialSettings) {
    std::vector<std::uint8_t> bytes;
    append_frame(bytes, static_cast<std::uint64_t>(fiber::http::Http3FrameType::Settings), settings_payload(8));

    fiber::mem::IoBufNodePool pool;
    fiber::mem::IoBufChain chain(pool);
    append_chain(chain, bytes);

    fiber::http::Http3ControlStreamDecoder decoder;
    fiber::http::Http3ControlStreamEvent event;
    ASSERT_EQ(decoder.parse(chain, event), fiber::http::Http3ParseStatus::Done);
    EXPECT_EQ(event.type, fiber::http::Http3ControlStreamEventType::Settings);
    EXPECT_EQ(event.settings.qpack_blocked_streams, 8U);
}

TEST(Http3ControlStreamDecoderTest, RejectsMissingInitialSettings) {
    std::vector<std::uint8_t> bytes;
    append_frame(bytes, static_cast<std::uint64_t>(fiber::http::Http3FrameType::Data));

    fiber::mem::IoBufNodePool pool;
    fiber::mem::IoBufChain chain(pool);
    append_chain(chain, bytes);

    fiber::http::Http3ControlStreamDecoder decoder;
    fiber::http::Http3ControlStreamEvent event;
    ASSERT_EQ(decoder.parse(chain, event), fiber::http::Http3ParseStatus::Error);
    EXPECT_EQ(decoder.error().h3_error, fiber::http::Http3ErrorCode::MissingSettings);
}

TEST(Http3ControlStreamDecoderTest, RejectsDuplicateSettingsFrame) {
    std::vector<std::uint8_t> bytes;
    append_frame(bytes, static_cast<std::uint64_t>(fiber::http::Http3FrameType::Settings));
    append_frame(bytes, static_cast<std::uint64_t>(fiber::http::Http3FrameType::Settings));

    fiber::mem::IoBufNodePool pool;
    fiber::mem::IoBufChain chain(pool);
    append_chain(chain, bytes);

    fiber::http::Http3ControlStreamDecoder decoder;
    fiber::http::Http3ControlStreamEvent event;
    ASSERT_EQ(decoder.parse(chain, event), fiber::http::Http3ParseStatus::Done);
    ASSERT_EQ(decoder.parse(chain, event), fiber::http::Http3ParseStatus::Error);
    EXPECT_EQ(decoder.error().h3_error, fiber::http::Http3ErrorCode::FrameUnexpected);
}

TEST(Http3ControlStreamDecoderTest, DrainsUnknownFrameAfterSettings) {
    std::vector<std::uint8_t> bytes;
    append_frame(bytes, static_cast<std::uint64_t>(fiber::http::Http3FrameType::Settings));
    append_frame(bytes, 0x21, {1, 2, 3});

    fiber::mem::IoBufNodePool pool;
    fiber::mem::IoBufChain chain(pool);
    append_chain(chain, bytes);

    fiber::http::Http3ControlStreamDecoder decoder;
    fiber::http::Http3ControlStreamEvent event;
    ASSERT_EQ(decoder.parse(chain, event), fiber::http::Http3ParseStatus::Done);
    EXPECT_EQ(decoder.parse(chain, event), fiber::http::Http3ParseStatus::NeedMore);
    EXPECT_EQ(chain.readable_bytes(), 0U);
}

TEST(Http3ControlStreamDecoderTest, ParsesGoawayAndMaxPushId) {
    std::vector<std::uint8_t> bytes;
    append_frame(bytes, static_cast<std::uint64_t>(fiber::http::Http3FrameType::Settings));
    std::vector<std::uint8_t> goaway_payload;
    append_varint(goaway_payload, 12);
    append_frame(bytes, static_cast<std::uint64_t>(fiber::http::Http3FrameType::Goaway), goaway_payload);
    std::vector<std::uint8_t> max_push_id_payload;
    append_varint(max_push_id_payload, 7);
    append_frame(bytes, static_cast<std::uint64_t>(fiber::http::Http3FrameType::MaxPushId), max_push_id_payload);

    fiber::mem::IoBufNodePool pool;
    fiber::mem::IoBufChain chain(pool);
    append_chain(chain, bytes);

    fiber::http::Http3ControlStreamDecoder decoder;
    fiber::http::Http3ControlStreamEvent event;
    ASSERT_EQ(decoder.parse(chain, event), fiber::http::Http3ParseStatus::Done);
    ASSERT_EQ(decoder.parse(chain, event), fiber::http::Http3ParseStatus::Done);
    EXPECT_EQ(event.type, fiber::http::Http3ControlStreamEventType::Goaway);
    EXPECT_EQ(event.id, 12U);
    ASSERT_EQ(decoder.parse(chain, event), fiber::http::Http3ParseStatus::Done);
    EXPECT_EQ(event.type, fiber::http::Http3ControlStreamEventType::MaxPushId);
    EXPECT_EQ(event.id, 7U);
}

TEST(Http3ControlStreamDecoderTest, RejectsMalformedGoawayPayload) {
    std::vector<std::uint8_t> bytes;
    append_frame(bytes, static_cast<std::uint64_t>(fiber::http::Http3FrameType::Settings));
    append_frame(bytes, static_cast<std::uint64_t>(fiber::http::Http3FrameType::Goaway), {0x00, 0x00});

    fiber::mem::IoBufNodePool pool;
    fiber::mem::IoBufChain chain(pool);
    append_chain(chain, bytes);

    fiber::http::Http3ControlStreamDecoder decoder;
    fiber::http::Http3ControlStreamEvent event;
    ASSERT_EQ(decoder.parse(chain, event), fiber::http::Http3ParseStatus::Done);
    ASSERT_EQ(decoder.parse(chain, event), fiber::http::Http3ParseStatus::Error);
    EXPECT_EQ(decoder.error().h3_error, fiber::http::Http3ErrorCode::FrameError);
}
