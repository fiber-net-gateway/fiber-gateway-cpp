#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <fiber/common/mem/IoBufChain.h>
#include <fiber/http/Http3Protocol.h>
#include "http/Http3ControlStreamEncoder.h"

namespace {

std::vector<std::uint8_t> collect(fiber::mem::IoBufChain &chain) {
    std::vector<std::uint8_t> out;
    out.reserve(chain.readable_bytes());
    while (fiber::mem::IoBuf *buf = chain.first_readable()) {
        const std::size_t bytes = buf->readable();
        out.insert(out.end(), buf->readable_data(), buf->readable_data() + bytes);
        chain.consume(bytes);
    }
    return out;
}

} // namespace

TEST(Http3ControlStreamEncoderTest, EncodesEmptySettingsPreface) {
    fiber::mem::IoBufNodePool pool;
    fiber::http::Http3Settings settings{};

    auto encoded = fiber::http::encode_http3_control_stream_preface(settings, pool);

    ASSERT_TRUE(encoded.has_value()) << static_cast<int>(encoded.error());
    const auto bytes = collect(*encoded);
    EXPECT_EQ(bytes, (std::vector<std::uint8_t>{0x00, 0x04, 0x00}));
}

TEST(Http3ControlStreamEncoderTest, EncodesNonDefaultSettingsPreface) {
    fiber::mem::IoBufNodePool pool;
    fiber::http::Http3Settings settings{};
    settings.qpack_blocked_streams = 8;
    settings.enable_connect_protocol = true;

    auto encoded = fiber::http::encode_http3_control_stream_preface(settings, pool);

    ASSERT_TRUE(encoded.has_value()) << static_cast<int>(encoded.error());
    const auto bytes = collect(*encoded);
    EXPECT_EQ(bytes, (std::vector<std::uint8_t>{0x00, 0x04, 0x04, 0x07, 0x08, 0x08, 0x01}));
}

TEST(Http3ControlStreamEncoderTest, EncodesGoaway) {
    fiber::mem::IoBufNodePool pool;

    auto encoded = fiber::http::encode_http3_goaway_frame(0, pool);

    ASSERT_TRUE(encoded.has_value()) << static_cast<int>(encoded.error());
    const auto bytes = collect(*encoded);
    EXPECT_EQ(bytes, (std::vector<std::uint8_t>{0x07, 0x01, 0x00}));
}
