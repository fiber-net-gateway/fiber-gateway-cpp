#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <fiber/common/IoError.h>
#include <fiber/http/Http3QpackDecoder.h>
#include <fiber/http/HttpHeaderHash.h>
#include "http/Huffman.h"

namespace {

using fiber::common::IoErr;
using fiber::http::Http3QpackDecoder;

struct DecodeRecorder {
    static IoErr on_indexed_field(void *ctx, Http3QpackDecoder::TableEntryView entry) noexcept {
        auto *self = static_cast<DecodeRecorder *>(ctx);
        self->fields.emplace_back(std::string(entry.name) + "=" + std::string(entry.value));
        return IoErr::None;
    }

    static IoErr on_indexed_name(void *ctx, std::string_view name, std::uint64_t name_hash) noexcept {
        auto *self = static_cast<DecodeRecorder *>(ctx);
        self->pending_name = std::string(name);
        self->pending_name_hash = name_hash;
        return IoErr::None;
    }

    static IoErr on_name_raw(void *ctx, const std::uint8_t *data, std::size_t len) noexcept {
        auto *self = static_cast<DecodeRecorder *>(ctx);
        self->pending_name.assign(reinterpret_cast<const char *>(data), len);
        self->pending_name_hash = fiber::http::http_header_name_hash(self->pending_name);
        return IoErr::None;
    }

    static IoErr on_name_huffman(void *ctx, const std::uint8_t *data, std::size_t len) noexcept {
        auto *self = static_cast<DecodeRecorder *>(ctx);
        bool ok = false;
        const std::size_t decoded_len = fiber::http::hpack_huffman_decoded_length(data, len, &ok);
        if (!ok) {
            return IoErr::Invalid;
        }
        self->pending_name.assign(decoded_len, '\0');
        fiber::http::HpackHuffmanDecodeState state;
        const fiber::http::HpackHuffmanDecodeResult result = fiber::http::hpack_huffman_decode_exact(
                state, data, len, reinterpret_cast<std::uint8_t *>(self->pending_name.data()), true);
        if (result.code != fiber::http::HpackHuffmanCode::Ok || result.written != decoded_len) {
            return IoErr::Invalid;
        }
        self->pending_name_hash = fiber::http::http_header_name_hash(self->pending_name);
        return IoErr::None;
    }

    static IoErr on_value_raw(void *ctx, const std::uint8_t *data, std::size_t len) noexcept {
        auto *self = static_cast<DecodeRecorder *>(ctx);
        self->fields.emplace_back(self->pending_name + "=" + std::string(reinterpret_cast<const char *>(data), len));
        self->pending_name.clear();
        self->pending_name_hash = 0;
        return IoErr::None;
    }

    static IoErr on_value_huffman(void *ctx, const std::uint8_t *data, std::size_t len) noexcept {
        auto *self = static_cast<DecodeRecorder *>(ctx);
        bool ok = false;
        const std::size_t decoded_len = fiber::http::hpack_huffman_decoded_length(data, len, &ok);
        if (!ok) {
            return IoErr::Invalid;
        }
        std::string value(decoded_len, '\0');
        fiber::http::HpackHuffmanDecodeState state;
        const fiber::http::HpackHuffmanDecodeResult result = fiber::http::hpack_huffman_decode_exact(
                state, data, len, reinterpret_cast<std::uint8_t *>(value.data()), true);
        if (result.code != fiber::http::HpackHuffmanCode::Ok || result.written != decoded_len) {
            return IoErr::Invalid;
        }
        self->fields.emplace_back(self->pending_name + "=" + value);
        self->pending_name.clear();
        self->pending_name_hash = 0;
        return IoErr::None;
    }

    static const Http3QpackDecoder::Ops &ops() noexcept {
        static const Http3QpackDecoder::Ops kOps{
                &DecodeRecorder::on_indexed_field, &DecodeRecorder::on_indexed_name, &DecodeRecorder::on_name_raw,
                &DecodeRecorder::on_name_huffman,  &DecodeRecorder::on_value_raw,    &DecodeRecorder::on_value_huffman,
        };
        return kOps;
    }

    std::vector<std::string> fields;
    std::string pending_name;
    std::uint64_t pending_name_hash = 0;
};

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

void append_raw_string(std::vector<std::uint8_t> &out, std::string_view value) {
    append_prefixed_integer(out, 0, 7, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

std::vector<std::uint8_t> huffman_encode(std::string_view value) {
    const auto *data = reinterpret_cast<const std::uint8_t *>(value.data());
    const std::size_t encoded_len = fiber::http::hpack_huffman_encoded_length(data, value.size());
    std::vector<std::uint8_t> out(encoded_len);
    const std::size_t written = fiber::http::hpack_huffman_encode_exact(data, value.size(), out.data());
    out.resize(written);
    return out;
}

void append_huffman_string(std::vector<std::uint8_t> &out, std::string_view value) {
    const std::vector<std::uint8_t> encoded = huffman_encode(value);
    append_prefixed_integer(out, 0x80, 7, encoded.size());
    out.insert(out.end(), encoded.begin(), encoded.end());
}

void append_literal_name_header(std::vector<std::uint8_t> &out, std::string_view name, bool huffman) {
    if (!huffman) {
        append_prefixed_integer(out, 0x20, 3, name.size());
        out.insert(out.end(), name.begin(), name.end());
        return;
    }

    const std::vector<std::uint8_t> encoded = huffman_encode(name);
    append_prefixed_integer(out, 0x28, 3, encoded.size());
    out.insert(out.end(), encoded.begin(), encoded.end());
}

void decode_all(const std::vector<std::uint8_t> &bytes, DecodeRecorder &recorder, IoErr expected = IoErr::None) {
    Http3QpackDecoder decoder;
    ASSERT_TRUE(decoder.init());
    decoder.begin_block(&recorder, &DecodeRecorder::ops());
    ASSERT_EQ(decoder.decode(bytes.data(), bytes.size(), true), expected);
}

} // namespace

TEST(Http3QpackDecoderTest, DecodesStaticIndexedField) {
    DecodeRecorder recorder;
    decode_all({0x00, 0x00, 0xd1}, recorder);

    ASSERT_EQ(recorder.fields.size(), 1U);
    EXPECT_EQ(recorder.fields[0], ":method=GET");
}

TEST(Http3QpackDecoderTest, DecodesStaticIndexedFieldWithContinuationIndex) {
    DecodeRecorder recorder;
    decode_all({0x00, 0x00, 0xff, 0x23}, recorder);

    ASSERT_EQ(recorder.fields.size(), 1U);
    EXPECT_EQ(recorder.fields[0], "x-frame-options=sameorigin");
}

TEST(Http3QpackDecoderTest, DecodesLiteralWithStaticNameReference) {
    std::vector<std::uint8_t> bytes{0x00, 0x00, 0x51};
    append_raw_string(bytes, "/index.html");

    DecodeRecorder recorder;
    decode_all(bytes, recorder);

    ASSERT_EQ(recorder.fields.size(), 1U);
    EXPECT_EQ(recorder.fields[0], ":path=/index.html");
}

TEST(Http3QpackDecoderTest, DecodesLiteralWithLiteralName) {
    std::vector<std::uint8_t> bytes{0x00, 0x00};
    append_literal_name_header(bytes, "x-test", false);
    append_raw_string(bytes, "ok");

    DecodeRecorder recorder;
    decode_all(bytes, recorder);

    ASSERT_EQ(recorder.fields.size(), 1U);
    EXPECT_EQ(recorder.fields[0], "x-test=ok");
}

TEST(Http3QpackDecoderTest, DecodesHuffmanNameAndValue) {
    std::vector<std::uint8_t> bytes{0x00, 0x00};
    append_literal_name_header(bytes, "x-test", true);
    append_huffman_string(bytes, "ok");

    DecodeRecorder recorder;
    decode_all(bytes, recorder);

    ASSERT_EQ(recorder.fields.size(), 1U);
    EXPECT_EQ(recorder.fields[0], "x-test=ok");
}

TEST(Http3QpackDecoderTest, SupportsSplitInput) {
    const std::vector<std::uint8_t> bytes{0x00, 0x00, 0xff, 0x23};
    DecodeRecorder recorder;
    Http3QpackDecoder decoder;
    ASSERT_TRUE(decoder.init());
    decoder.begin_block(&recorder, &DecodeRecorder::ops());

    ASSERT_EQ(decoder.decode(bytes.data(), 1, false), IoErr::None);
    ASSERT_EQ(decoder.decode(bytes.data() + 1, 1, false), IoErr::None);
    ASSERT_EQ(decoder.decode(bytes.data() + 2, 1, false), IoErr::None);
    ASSERT_EQ(decoder.decode(bytes.data() + 3, 1, true), IoErr::None);

    ASSERT_EQ(recorder.fields.size(), 1U);
    EXPECT_EQ(recorder.fields[0], "x-frame-options=sameorigin");
}

TEST(Http3QpackDecoderTest, RejectsNonzeroRequiredInsertCount) {
    DecodeRecorder recorder;
    decode_all({0x01, 0x00}, recorder, IoErr::Invalid);
}

TEST(Http3QpackDecoderTest, RejectsIntegerAboveQpackLimit) {
    DecodeRecorder recorder;
    decode_all({0x00, 0x7f, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x7f}, recorder, IoErr::Invalid);
}

TEST(Http3QpackDecoderTest, RejectsDynamicIndexedField) {
    DecodeRecorder recorder;
    decode_all({0x00, 0x00, 0x80}, recorder, IoErr::Invalid);
}

TEST(Http3QpackDecoderTest, RejectsDynamicNameReference) {
    std::vector<std::uint8_t> bytes{0x00, 0x00, 0x40};
    append_raw_string(bytes, "value");

    DecodeRecorder recorder;
    decode_all(bytes, recorder, IoErr::Invalid);
}

TEST(Http3QpackDecoderTest, RejectsPostBaseRepresentations) {
    DecodeRecorder indexed_recorder;
    decode_all({0x00, 0x00, 0x10}, indexed_recorder, IoErr::Invalid);

    DecodeRecorder literal_recorder;
    decode_all({0x00, 0x00, 0x00}, literal_recorder, IoErr::Invalid);
}

TEST(Http3QpackDecoderTest, RejectsIncompleteBlockAtEnd) {
    DecodeRecorder recorder;
    decode_all({0x00}, recorder, IoErr::Invalid);
}

TEST(Http3QpackDecoderTest, RejectsStaticIndexOutOfRange) {
    DecodeRecorder recorder;
    decode_all({0x00, 0x00, 0xff, 0x24}, recorder, IoErr::Invalid);
}
