#include <gtest/gtest.h>

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "common/IoError.h"
#include "http/Http2HpackDecoder.h"
#include "http/Http2HpackHuffman.h"
#include "http/HttpHeaderHash.h"

namespace {

struct DecodedEvent {
    enum class Kind : std::uint8_t {
        IndexedField,
        IndexedName,
        NameRaw,
        NameHuffman,
        ValueRaw,
        ValueHuffman,
    };

    Kind kind = Kind::IndexedField;
    std::string name;
    std::string value;
    std::uint64_t name_hash = 0;
};

struct DecoderRecorder {
    static fiber::common::IoErr on_indexed_field(void *ctx, fiber::http::Http2HpackDecoder::TableEntryView entry) noexcept {
        auto *self = static_cast<DecoderRecorder *>(ctx);
        self->events.push_back({DecodedEvent::Kind::IndexedField, std::string(entry.name), std::string(entry.value),
                                entry.name_hash});
        return fiber::common::IoErr::None;
    }

    static fiber::common::IoErr on_indexed_name(void *ctx, std::string_view name, std::uint64_t name_hash) noexcept {
        auto *self = static_cast<DecoderRecorder *>(ctx);
        self->names.emplace_back(name);
        self->pending_name = self->names.back();
        self->pending_name_hash = name_hash;
        self->events.push_back({DecodedEvent::Kind::IndexedName, std::string(name), {}, name_hash});
        return fiber::common::IoErr::None;
    }

    static fiber::common::IoErr on_name_raw(void *ctx, const std::uint8_t *data, std::size_t len) noexcept {
        auto *self = static_cast<DecoderRecorder *>(ctx);
        self->names.emplace_back(reinterpret_cast<const char *>(data), len);
        self->pending_name = self->names.back();
        self->pending_name_hash = fiber::http::http_header_name_hash(self->pending_name);
        self->events.push_back(
            {DecodedEvent::Kind::NameRaw, std::string(self->pending_name), {}, self->pending_name_hash});
        return fiber::common::IoErr::None;
    }

    static fiber::common::IoErr on_name_huffman(void *ctx, const std::uint8_t *data, std::size_t len) noexcept {
        auto *self = static_cast<DecoderRecorder *>(ctx);
        bool ok = false;
        const std::size_t decoded_len = fiber::http::http2_huffman_decoded_length(data, len, &ok);
        if (!ok) {
            return fiber::common::IoErr::Invalid;
        }

        self->names.emplace_back(decoded_len, '\0');
        fiber::http::Http2HuffmanDecodeState state;
        fiber::http::Http2HuffmanDecodeResult result = fiber::http::http2_huffman_decode(
            state, data, len, reinterpret_cast<std::uint8_t *>(self->names.back().data()), decoded_len, true);
        if (result.code != fiber::http::Http2HuffmanCode::Ok || result.written != decoded_len) {
            return fiber::common::IoErr::Invalid;
        }

        self->pending_name = self->names.back();
        self->pending_name_hash = fiber::http::http_header_name_hash(self->pending_name);
        self->events.push_back(
            {DecodedEvent::Kind::NameHuffman, std::string(self->pending_name), {}, self->pending_name_hash});
        return fiber::common::IoErr::None;
    }

    static fiber::common::IoErr on_value_raw(void *ctx, const std::uint8_t *data, std::size_t len,
                                             fiber::http::Http2HpackDecoder::FieldView *out) noexcept {
        auto *self = static_cast<DecoderRecorder *>(ctx);
        self->values.emplace_back(reinterpret_cast<const char *>(data), len);
        std::string_view value = self->values.back();
        if (out != nullptr) {
            out->name = self->pending_name;
            out->name_hash = self->pending_name_hash;
            out->value = value;
        }
        self->events.push_back({DecodedEvent::Kind::ValueRaw, {}, std::string(value), 0});
        return fiber::common::IoErr::None;
    }

    static fiber::common::IoErr on_value_huffman(void *ctx, const std::uint8_t *data, std::size_t len,
                                                 fiber::http::Http2HpackDecoder::FieldView *out) noexcept {
        auto *self = static_cast<DecoderRecorder *>(ctx);
        bool ok = false;
        const std::size_t decoded_len = fiber::http::http2_huffman_decoded_length(data, len, &ok);
        if (!ok) {
            return fiber::common::IoErr::Invalid;
        }

        self->values.emplace_back(decoded_len, '\0');
        fiber::http::Http2HuffmanDecodeState state;
        fiber::http::Http2HuffmanDecodeResult result = fiber::http::http2_huffman_decode(
            state, data, len, reinterpret_cast<std::uint8_t *>(self->values.back().data()), decoded_len, true);
        if (result.code != fiber::http::Http2HuffmanCode::Ok || result.written != decoded_len) {
            return fiber::common::IoErr::Invalid;
        }

        std::string_view value = self->values.back();
        if (out != nullptr) {
            out->name = self->pending_name;
            out->name_hash = self->pending_name_hash;
            out->value = value;
        }
        self->events.push_back({DecodedEvent::Kind::ValueHuffman, {}, std::string(value), 0});
        return fiber::common::IoErr::None;
    }

    static const fiber::http::Http2HpackDecoder::Ops &ops() noexcept {
        static const fiber::http::Http2HpackDecoder::Ops kOps{
            &DecoderRecorder::on_indexed_field,
            &DecoderRecorder::on_indexed_name,
            &DecoderRecorder::on_name_raw,
            &DecoderRecorder::on_name_huffman,
            &DecoderRecorder::on_value_raw,
            &DecoderRecorder::on_value_huffman,
        };
        return kOps;
    }

    std::vector<DecodedEvent> events;
    std::deque<std::string> names;
    std::deque<std::string> values;
    std::string_view pending_name;
    std::uint64_t pending_name_hash = 0;
};

} // namespace

TEST(Http2HpackDecoderTest, DecodesIndexedFieldFromStaticTable) {
    fiber::http::Http2HpackDecoder decoder;
    ASSERT_TRUE(decoder.init());

    DecoderRecorder recorder;
    decoder.begin_block(&recorder, &DecoderRecorder::ops());

    const std::uint8_t block[] = {0x82};
    EXPECT_EQ(decoder.decode(block, sizeof(block), true), fiber::common::IoErr::None);
    ASSERT_EQ(recorder.events.size(), 1U);
    EXPECT_EQ(recorder.events[0].kind, DecodedEvent::Kind::IndexedField);
    EXPECT_EQ(recorder.events[0].name, ":method");
    EXPECT_EQ(recorder.events[0].value, "GET");
}

TEST(Http2HpackDecoderTest, DecodesLiteralFieldAcrossFragmentsAndReusesDynamicTableEntry) {
    fiber::http::Http2HpackDecoder decoder;
    ASSERT_TRUE(decoder.init());

    DecoderRecorder recorder;
    decoder.begin_block(&recorder, &DecoderRecorder::ops());

    const std::uint8_t literal_block[] = {0x40, 0x03, 'f', 'o', 'o', 0x03, 'b', 'a', 'r'};
    EXPECT_EQ(decoder.decode(literal_block, 2, false), fiber::common::IoErr::None);
    EXPECT_TRUE(recorder.events.empty());
    EXPECT_EQ(decoder.decode(literal_block + 2, 3, false), fiber::common::IoErr::None);
    ASSERT_EQ(recorder.events.size(), 1U);
    EXPECT_EQ(recorder.events[0].kind, DecodedEvent::Kind::NameRaw);
    EXPECT_EQ(recorder.events[0].name, "foo");
    EXPECT_EQ(decoder.decode(literal_block + 5, sizeof(literal_block) - 5, true), fiber::common::IoErr::None);

    ASSERT_EQ(recorder.events.size(), 2U);
    EXPECT_EQ(recorder.events[1].kind, DecodedEvent::Kind::ValueRaw);
    EXPECT_EQ(recorder.events[1].value, "bar");

    recorder.events.clear();
    decoder.begin_block(&recorder, &DecoderRecorder::ops());
    const std::uint8_t indexed_block[] = {0xbe};
    EXPECT_EQ(decoder.decode(indexed_block, sizeof(indexed_block), true), fiber::common::IoErr::None);
    ASSERT_EQ(recorder.events.size(), 1U);
    EXPECT_EQ(recorder.events[0].kind, DecodedEvent::Kind::IndexedField);
    EXPECT_EQ(recorder.events[0].name, "foo");
    EXPECT_EQ(recorder.events[0].value, "bar");
}

TEST(Http2HpackDecoderTest, DecodesHuffmanLiteralFieldViaOwnerCallbacks) {
    fiber::http::Http2HpackDecoder decoder;
    ASSERT_TRUE(decoder.init());

    const std::string name = "foo";
    const std::string value = "bar";
    const std::size_t encoded_name_len = fiber::http::http2_huffman_encoded_length(
        reinterpret_cast<const std::uint8_t *>(name.data()), name.size());
    const std::size_t encoded_value_len = fiber::http::http2_huffman_encoded_length(
        reinterpret_cast<const std::uint8_t *>(value.data()), value.size());
    std::vector<std::uint8_t> encoded_name(encoded_name_len);
    std::vector<std::uint8_t> encoded_value(encoded_value_len);
    fiber::http::Http2HuffmanEncodeResult name_result = fiber::http::http2_huffman_encode(
        reinterpret_cast<const std::uint8_t *>(name.data()), name.size(), encoded_name.data(), encoded_name.size());
    fiber::http::Http2HuffmanEncodeResult value_result = fiber::http::http2_huffman_encode(
        reinterpret_cast<const std::uint8_t *>(value.data()), value.size(), encoded_value.data(), encoded_value.size());
    ASSERT_EQ(name_result.code, fiber::http::Http2HuffmanCode::Ok);
    ASSERT_EQ(value_result.code, fiber::http::Http2HuffmanCode::Ok);

    std::vector<std::uint8_t> block;
    block.push_back(0x40);
    block.push_back(static_cast<std::uint8_t>(0x80U | encoded_name.size()));
    block.insert(block.end(), encoded_name.begin(), encoded_name.end());
    block.push_back(static_cast<std::uint8_t>(0x80U | encoded_value.size()));
    block.insert(block.end(), encoded_value.begin(), encoded_value.end());

    DecoderRecorder recorder;
    decoder.begin_block(&recorder, &DecoderRecorder::ops());
    EXPECT_EQ(decoder.decode(block.data(), block.size(), true), fiber::common::IoErr::None);

    ASSERT_EQ(recorder.events.size(), 2U);
    EXPECT_EQ(recorder.events[0].kind, DecodedEvent::Kind::NameHuffman);
    EXPECT_EQ(recorder.events[0].name, "foo");
    EXPECT_EQ(recorder.events[1].kind, DecodedEvent::Kind::ValueHuffman);
    EXPECT_EQ(recorder.events[1].value, "bar");
}

TEST(Http2HpackDecoderTest, RejectsLiteralStringLongerThanConfiguredLimit) {
    fiber::http::Http2HpackDecoder decoder;
    ASSERT_TRUE(decoder.init(4096U, 2U));

    DecoderRecorder recorder;
    decoder.begin_block(&recorder, &DecoderRecorder::ops());

    const std::uint8_t block[] = {0x40, 0x03, 'f', 'o', 'o', 0x01, 'x'};
    EXPECT_EQ(decoder.decode(block, sizeof(block), true), fiber::common::IoErr::Invalid);
}

TEST(Http2HpackDecoderTest, RejectsIndexedNameLongerThanConfiguredLimit) {
    fiber::http::Http2HpackDecoder decoder;
    ASSERT_TRUE(decoder.init(4096U, 2U));

    DecoderRecorder recorder;
    decoder.begin_block(&recorder, &DecoderRecorder::ops());

    const std::uint8_t block[] = {0x01, 0x01, 'x'};
    EXPECT_EQ(decoder.decode(block, sizeof(block), true), fiber::common::IoErr::Invalid);
}
