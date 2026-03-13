#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "http/Http2HpackHuffman.h"

namespace {

std::vector<std::uint8_t> as_bytes(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t *>(text.data()),
            reinterpret_cast<const std::uint8_t *>(text.data()) + text.size()};
}

std::vector<std::uint8_t> encode_text(std::string_view text,
                                      fiber::http::Http2HuffmanLowerMode lower_mode =
                                          fiber::http::Http2HuffmanLowerMode::None) {
    std::vector<std::uint8_t> input = as_bytes(text);
    const std::size_t encoded_len =
        fiber::http::http2_huffman_encoded_length(input.data(), input.size(), lower_mode);
    std::vector<std::uint8_t> encoded(encoded_len);

    const auto result = fiber::http::http2_huffman_encode(input.data(), input.size(), encoded.data(), encoded.size(),
                                                          lower_mode);
    EXPECT_EQ(result.code, fiber::http::Http2HuffmanCode::Ok);
    EXPECT_EQ(result.written, encoded.size());

    return encoded;
}

std::vector<std::uint8_t> decode_bytes(const std::vector<std::uint8_t> &encoded) {
    fiber::http::Http2HuffmanDecodeState state;
    std::vector<std::uint8_t> decoded(encoded.size() * 8U);

    const auto result =
        fiber::http::http2_huffman_decode(state, encoded.data(), encoded.size(), decoded.data(), decoded.size(), true);
    EXPECT_EQ(result.code, fiber::http::Http2HuffmanCode::Ok);
    decoded.resize(result.written);
    return decoded;
}

} // namespace

TEST(Http2HpackHuffmanTest, EncodesRfcExample) {
    constexpr std::string_view text = "www.example.com";
    const std::vector<std::uint8_t> expected = {
        0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xff,
    };

    const std::vector<std::uint8_t> encoded = encode_text(text);
    EXPECT_EQ(encoded, expected);
}

TEST(Http2HpackHuffmanTest, DecodesRfcExample) {
    const std::vector<std::uint8_t> encoded = {
        0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xff,
    };

    const std::vector<std::uint8_t> decoded = decode_bytes(encoded);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(decoded.data()), decoded.size()), "www.example.com");
}

TEST(Http2HpackHuffmanTest, LowercaseModeMatchesLowercaseInput) {
    constexpr std::string_view mixed = "Content-Type";
    constexpr std::string_view lower = "content-type";

    const std::vector<std::uint8_t> encoded_mixed =
        encode_text(mixed, fiber::http::Http2HuffmanLowerMode::Ascii);
    const std::vector<std::uint8_t> encoded_lower = encode_text(lower);

    EXPECT_EQ(encoded_mixed, encoded_lower);
}

TEST(Http2HpackHuffmanTest, RoundTripsAllByteValues) {
    std::vector<std::uint8_t> input(256);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<std::uint8_t>(i);
    }

    const std::size_t encoded_len = fiber::http::http2_huffman_encoded_length(input.data(), input.size());
    std::vector<std::uint8_t> encoded(encoded_len);
    auto encode_result =
        fiber::http::http2_huffman_encode(input.data(), input.size(), encoded.data(), encoded.size());
    ASSERT_EQ(encode_result.code, fiber::http::Http2HuffmanCode::Ok);

    std::vector<std::uint8_t> decoded(input.size());
    fiber::http::Http2HuffmanDecodeState state;
    auto decode_result =
        fiber::http::http2_huffman_decode(state, encoded.data(), encoded.size(), decoded.data(), decoded.size(), true);
    ASSERT_EQ(decode_result.code, fiber::http::Http2HuffmanCode::Ok);
    decoded.resize(decode_result.written);

    EXPECT_EQ(decoded, input);
}

TEST(Http2HpackHuffmanTest, DecodeSupportsStreaming) {
    const std::vector<std::uint8_t> encoded = encode_text("cache-control");
    std::vector<std::uint8_t> decoded(64);
    fiber::http::Http2HuffmanDecodeState state;

    std::size_t total_written = 0;
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        const bool last = i + 1 == encoded.size();
        auto result = fiber::http::http2_huffman_decode(state, &encoded[i], 1, decoded.data() + total_written,
                                                        decoded.size() - total_written, last);
        if (!last) {
            EXPECT_TRUE(result.code == fiber::http::Http2HuffmanCode::Ok ||
                        result.code == fiber::http::Http2HuffmanCode::NeedMore);
        } else {
            EXPECT_EQ(result.code, fiber::http::Http2HuffmanCode::Ok);
        }
        total_written += result.written;
    }

    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(decoded.data()), total_written), "cache-control");
}

TEST(Http2HpackHuffmanTest, EncodeFailsWhenOutputBufferIsTooSmall) {
    constexpr std::string_view text = "www.example.com";
    std::vector<std::uint8_t> input = as_bytes(text);
    const std::size_t encoded_len = fiber::http::http2_huffman_encoded_length(input.data(), input.size());
    std::vector<std::uint8_t> encoded(encoded_len - 1);

    const auto result =
        fiber::http::http2_huffman_encode(input.data(), input.size(), encoded.data(), encoded.size());
    EXPECT_EQ(result.code, fiber::http::Http2HuffmanCode::OutputFull);
    EXPECT_EQ(result.written, encoded.size());
}

TEST(Http2HpackHuffmanTest, DecodeFailsWhenOutputBufferIsTooSmall) {
    const std::vector<std::uint8_t> encoded = encode_text("www.example.com");
    std::array<std::uint8_t, 4> decoded{};
    fiber::http::Http2HuffmanDecodeState state;

    const auto result =
        fiber::http::http2_huffman_decode(state, encoded.data(), encoded.size(), decoded.data(), decoded.size(), true);
    EXPECT_EQ(result.code, fiber::http::Http2HuffmanCode::OutputFull);
    EXPECT_EQ(result.consumed, 3u);
    EXPECT_EQ(result.written, 3u);
}

TEST(Http2HpackHuffmanTest, DecodeRejectsInvalidPadding) {
    const std::array<std::uint8_t, 1> encoded{0x18};
    std::array<std::uint8_t, 8> decoded{};
    fiber::http::Http2HuffmanDecodeState state;

    const auto result =
        fiber::http::http2_huffman_decode(state, encoded.data(), encoded.size(), decoded.data(), decoded.size(), true);
    EXPECT_EQ(result.code, fiber::http::Http2HuffmanCode::InvalidEncoding);
}

TEST(Http2HpackHuffmanTest, DecodeRejectsEosSymbol) {
    const std::array<std::uint8_t, 4> encoded{0xff, 0xff, 0xff, 0xfc};
    std::array<std::uint8_t, 8> decoded{};
    fiber::http::Http2HuffmanDecodeState state;

    const auto result =
        fiber::http::http2_huffman_decode(state, encoded.data(), encoded.size(), decoded.data(), decoded.size(), true);
    EXPECT_EQ(result.code, fiber::http::Http2HuffmanCode::InvalidEncoding);
}

TEST(Http2HpackHuffmanTest, DecodeReportsNeedMoreForIncompleteCode) {
    const std::array<std::uint8_t, 1> encoded{0xfe};
    std::array<std::uint8_t, 8> decoded{};
    fiber::http::Http2HuffmanDecodeState state;

    const auto result = fiber::http::http2_huffman_decode(state, encoded.data(), encoded.size(), decoded.data(),
                                                          decoded.size(), false);
    EXPECT_EQ(result.code, fiber::http::Http2HuffmanCode::NeedMore);
    EXPECT_EQ(result.consumed, 1u);
    EXPECT_EQ(result.written, 0u);
}
