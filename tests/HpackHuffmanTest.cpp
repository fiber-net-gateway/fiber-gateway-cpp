#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <fiber/http/Huffman.h>

namespace {

std::vector<std::uint8_t> as_bytes(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t *>(text.data()),
            reinterpret_cast<const std::uint8_t *>(text.data()) + text.size()};
}

std::vector<std::uint8_t>
encode_text(std::string_view text,
            fiber::http::HpackHuffmanLowerMode lower_mode = fiber::http::HpackHuffmanLowerMode::None) {
    std::vector<std::uint8_t> input = as_bytes(text);
    const std::size_t encoded_len = fiber::http::hpack_huffman_encoded_length(input.data(), input.size(), lower_mode);
    std::vector<std::uint8_t> encoded(encoded_len);

    const auto result =
            fiber::http::hpack_huffman_encode(input.data(), input.size(), encoded.data(), encoded.size(), lower_mode);
    EXPECT_EQ(result.code, fiber::http::HpackHuffmanCode::Ok);
    EXPECT_EQ(result.consumed, input.size());
    EXPECT_EQ(result.written, encoded.size());

    return encoded;
}

std::vector<std::uint8_t>
encode_text_exact(std::string_view text,
                  fiber::http::HpackHuffmanLowerMode lower_mode = fiber::http::HpackHuffmanLowerMode::None) {
    std::vector<std::uint8_t> input = as_bytes(text);
    const std::size_t encoded_len = fiber::http::hpack_huffman_encoded_length(input.data(), input.size(), lower_mode);
    std::vector<std::uint8_t> encoded(encoded_len);

    const std::size_t written =
            fiber::http::hpack_huffman_encode_exact(input.data(), input.size(), encoded.data(), lower_mode);
    EXPECT_EQ(written, encoded.size());

    return encoded;
}

std::vector<std::uint8_t> decode_bytes(const std::vector<std::uint8_t> &encoded) {
    fiber::http::HpackHuffmanDecodeState state;
    std::vector<std::uint8_t> decoded(encoded.size() * 8U);

    const auto result = fiber::http::hpack_huffman_decode(state, encoded.data(), encoded.size(), decoded.data(),
                                                          decoded.size(), true);
    EXPECT_EQ(result.code, fiber::http::HpackHuffmanCode::Ok);
    decoded.resize(result.written);
    return decoded;
}

std::vector<std::uint8_t> decode_bytes_exact(const std::vector<std::uint8_t> &encoded) {
    bool ok = false;
    const std::size_t decoded_len = fiber::http::hpack_huffman_decoded_length(encoded.data(), encoded.size(), &ok);
    EXPECT_TRUE(ok);
    std::vector<std::uint8_t> decoded(decoded_len);
    fiber::http::HpackHuffmanDecodeState state;

    const auto result =
            fiber::http::hpack_huffman_decode_exact(state, encoded.data(), encoded.size(), decoded.data(), true);
    EXPECT_EQ(result.code, fiber::http::HpackHuffmanCode::Ok);
    EXPECT_EQ(result.written, decoded.size());
    return decoded;
}

} // namespace

TEST(HpackHuffmanTest, EncodesRfcExample) {
    constexpr std::string_view text = "www.example.com";
    const std::vector<std::uint8_t> expected = {
            0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xff,
    };

    const std::vector<std::uint8_t> encoded = encode_text(text);
    EXPECT_EQ(encoded, expected);
}

TEST(HpackHuffmanTest, DecodesRfcExample) {
    const std::vector<std::uint8_t> encoded = {
            0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xff,
    };

    const std::vector<std::uint8_t> decoded = decode_bytes(encoded);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(decoded.data()), decoded.size()), "www.example.com");
}

TEST(HpackHuffmanTest, LowercaseModeMatchesLowercaseInput) {
    constexpr std::string_view mixed = "Content-Type";
    constexpr std::string_view lower = "content-type";

    const std::vector<std::uint8_t> encoded_mixed = encode_text(mixed, fiber::http::HpackHuffmanLowerMode::Ascii);
    const std::vector<std::uint8_t> encoded_lower = encode_text(lower);

    EXPECT_EQ(encoded_mixed, encoded_lower);
}

TEST(HpackHuffmanTest, ExactEncodeMatchesCheckedEncode) {
    constexpr std::string_view text = "Content-Type";

    const std::vector<std::uint8_t> encoded_checked = encode_text(text, fiber::http::HpackHuffmanLowerMode::Ascii);
    const std::vector<std::uint8_t> encoded_exact = encode_text_exact(text, fiber::http::HpackHuffmanLowerMode::Ascii);

    EXPECT_EQ(encoded_exact, encoded_checked);
}

TEST(HpackHuffmanTest, IncrementalEncodeMatchesExactEncode) {
    constexpr std::string_view text = "server: nginx-1.25.1";
    std::vector<std::uint8_t> input = as_bytes(text);
    const std::vector<std::uint8_t> encoded_exact = encode_text_exact(text);
    std::vector<std::uint8_t> encoded(encoded_exact.size());
    fiber::http::HpackHuffmanEncodeState state;

    std::size_t src_offset = 0;
    std::size_t dst_offset = 0;
    while (true) {
        const std::size_t dst_cap = std::min<std::size_t>(5, encoded.size() - dst_offset);
        const auto result = fiber::http::hpack_huffman_encode_incremental(state, input.data() + src_offset,
                                                                          input.size() - src_offset,
                                                                          encoded.data() + dst_offset, dst_cap, true);

        src_offset += result.consumed;
        dst_offset += result.written;

        if (result.code == fiber::http::HpackHuffmanCode::OutputFull) {
            continue;
        }

        EXPECT_EQ(result.code, fiber::http::HpackHuffmanCode::Ok);
        break;
    }

    EXPECT_EQ(src_offset, input.size());
    EXPECT_EQ(dst_offset, encoded.size());
    EXPECT_EQ(encoded, encoded_exact);
}

TEST(HpackHuffmanTest, IncrementalEncodeCanPauseBeforeFinalFlush) {
    constexpr std::string_view text = "www.example.com";
    std::vector<std::uint8_t> input = as_bytes(text);
    const std::size_t encoded_len = fiber::http::hpack_huffman_encoded_length(input.data(), input.size());
    std::vector<std::uint8_t> encoded(encoded_len);
    fiber::http::HpackHuffmanEncodeState state;

    const auto first = fiber::http::hpack_huffman_encode_incremental(state, input.data(), input.size(), encoded.data(),
                                                                     encoded_len - 1, true);
    EXPECT_EQ(first.code, fiber::http::HpackHuffmanCode::OutputFull);
    EXPECT_EQ(first.consumed, input.size());
    EXPECT_EQ(first.written, encoded_len - 1);

    const auto second = fiber::http::hpack_huffman_encode_incremental(
            state, input.data() + first.consumed, input.size() - first.consumed, encoded.data() + first.written,
            encoded.size() - first.written, true);
    EXPECT_EQ(second.code, fiber::http::HpackHuffmanCode::Ok);
    EXPECT_EQ(second.consumed, 0u);
    EXPECT_EQ(second.written, 1u);

    const std::vector<std::uint8_t> encoded_exact = encode_text_exact(text);
    EXPECT_EQ(encoded, encoded_exact);
}

TEST(HpackHuffmanTest, ExactDecodeMatchesCheckedDecode) {
    const std::vector<std::uint8_t> encoded = encode_text("www.example.com");

    const std::vector<std::uint8_t> decoded_checked = decode_bytes(encoded);
    const std::vector<std::uint8_t> decoded_exact = decode_bytes_exact(encoded);

    EXPECT_EQ(decoded_exact, decoded_checked);
}

TEST(HpackHuffmanTest, RoundTripsAllByteValues) {
    std::vector<std::uint8_t> input(256);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<std::uint8_t>(i);
    }

    const std::size_t encoded_len = fiber::http::hpack_huffman_encoded_length(input.data(), input.size());
    std::vector<std::uint8_t> encoded(encoded_len);
    auto encode_result = fiber::http::hpack_huffman_encode(input.data(), input.size(), encoded.data(), encoded.size());
    ASSERT_EQ(encode_result.code, fiber::http::HpackHuffmanCode::Ok);

    std::vector<std::uint8_t> decoded(input.size());
    fiber::http::HpackHuffmanDecodeState state;
    auto decode_result = fiber::http::hpack_huffman_decode(state, encoded.data(), encoded.size(), decoded.data(),
                                                           decoded.size(), true);
    ASSERT_EQ(decode_result.code, fiber::http::HpackHuffmanCode::Ok);
    decoded.resize(decode_result.written);

    EXPECT_EQ(decoded, input);
}

TEST(HpackHuffmanTest, DecodeSupportsStreaming) {
    const std::vector<std::uint8_t> encoded = encode_text("cache-control");
    std::vector<std::uint8_t> decoded(64);
    fiber::http::HpackHuffmanDecodeState state;

    std::size_t total_written = 0;
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        const bool last = i + 1 == encoded.size();
        auto result = fiber::http::hpack_huffman_decode(state, &encoded[i], 1, decoded.data() + total_written,
                                                        decoded.size() - total_written, last);
        if (!last) {
            EXPECT_TRUE(result.code == fiber::http::HpackHuffmanCode::Ok ||
                        result.code == fiber::http::HpackHuffmanCode::NeedMore);
        } else {
            EXPECT_EQ(result.code, fiber::http::HpackHuffmanCode::Ok);
        }
        total_written += result.written;
    }

    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(decoded.data()), total_written), "cache-control");
}

TEST(HpackHuffmanTest, EncodeFailsWhenOutputBufferIsTooSmall) {
    constexpr std::string_view text = "www.example.com";
    std::vector<std::uint8_t> input = as_bytes(text);
    const std::size_t encoded_len = fiber::http::hpack_huffman_encoded_length(input.data(), input.size());
    std::vector<std::uint8_t> encoded(encoded_len - 1);

    const auto result = fiber::http::hpack_huffman_encode(input.data(), input.size(), encoded.data(), encoded.size());
    EXPECT_EQ(result.code, fiber::http::HpackHuffmanCode::OutputFull);
    EXPECT_EQ(result.consumed, input.size());
    EXPECT_EQ(result.written, encoded.size());
}

TEST(HpackHuffmanTest, DecodeFailsWhenOutputBufferIsTooSmall) {
    const std::vector<std::uint8_t> encoded = encode_text("www.example.com");
    std::array<std::uint8_t, 4> decoded{};
    fiber::http::HpackHuffmanDecodeState state;

    const auto result = fiber::http::hpack_huffman_decode(state, encoded.data(), encoded.size(), decoded.data(),
                                                          decoded.size(), true);
    EXPECT_EQ(result.code, fiber::http::HpackHuffmanCode::OutputFull);
    EXPECT_EQ(result.consumed, 3u);
    EXPECT_EQ(result.written, 3u);
}

TEST(HpackHuffmanTest, DecodeRejectsInvalidPadding) {
    const std::array<std::uint8_t, 1> encoded{0x18};
    std::array<std::uint8_t, 8> decoded{};
    fiber::http::HpackHuffmanDecodeState state;

    const auto result = fiber::http::hpack_huffman_decode(state, encoded.data(), encoded.size(), decoded.data(),
                                                          decoded.size(), true);
    EXPECT_EQ(result.code, fiber::http::HpackHuffmanCode::InvalidEncoding);
}

TEST(HpackHuffmanTest, DecodeRejectsEosSymbol) {
    const std::array<std::uint8_t, 4> encoded{0xff, 0xff, 0xff, 0xfc};
    std::array<std::uint8_t, 8> decoded{};
    fiber::http::HpackHuffmanDecodeState state;

    const auto result = fiber::http::hpack_huffman_decode(state, encoded.data(), encoded.size(), decoded.data(),
                                                          decoded.size(), true);
    EXPECT_EQ(result.code, fiber::http::HpackHuffmanCode::InvalidEncoding);
}

TEST(HpackHuffmanTest, DecodeReportsNeedMoreForIncompleteCode) {
    const std::array<std::uint8_t, 1> encoded{0xfe};
    std::array<std::uint8_t, 8> decoded{};
    fiber::http::HpackHuffmanDecodeState state;

    const auto result = fiber::http::hpack_huffman_decode(state, encoded.data(), encoded.size(), decoded.data(),
                                                          decoded.size(), false);
    EXPECT_EQ(result.code, fiber::http::HpackHuffmanCode::NeedMore);
    EXPECT_EQ(result.consumed, 1u);
    EXPECT_EQ(result.written, 0u);
}
