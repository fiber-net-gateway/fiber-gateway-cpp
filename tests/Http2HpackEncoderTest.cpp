#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/mem/IoBuf.h"
#include "http/Http2HpackDecoder.h"
#include "http/Http2HpackEncodeCatalog.h"
#include "http/Http2HpackEncoder.h"
#include "http/Http2HpackEncoderIoBufWriter.h"
#include "http/Http2HpackHuffman.h"
#include "http/HttpHeaderHash.h"

namespace {

using fiber::common::IoErr;
using fiber::http::Http2HpackDecoder;
using fiber::http::Http2HpackEncodeCatalog;
using fiber::http::Http2HpackEncoder;
using fiber::http::Http2HpackEncoderIoBufWriter;
using fiber::mem::IoBufChain;

std::vector<std::uint8_t> chain_to_bytes(IoBufChain chain) {
    std::vector<std::uint8_t> out;
    out.reserve(chain.readable_bytes());
    while (auto *front = chain.front()) {
        if (front->readable() == 0) {
            chain.drop_empty_front();
            continue;
        }
        const std::uint8_t *data = front->readable_data();
        out.insert(out.end(), data, data + front->readable());
        chain.consume_and_compact(front->readable());
    }
    return out;
}

struct DecodeRecorder {
    static IoErr on_indexed_field(void *ctx, Http2HpackDecoder::TableEntryView entry) noexcept {
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
        const std::size_t decoded_len = fiber::http::http2_huffman_decoded_length(data, len, &ok);
        if (!ok) {
            return IoErr::Invalid;
        }
        self->pending_name.assign(decoded_len, '\0');
        fiber::http::Http2HuffmanDecodeState state;
        auto result = fiber::http::http2_huffman_decode_exact(
            state, data, len, reinterpret_cast<std::uint8_t *>(self->pending_name.data()), true);
        if (result.code != fiber::http::Http2HuffmanCode::Ok || result.written != decoded_len) {
            return IoErr::Invalid;
        }
        self->pending_name_hash = fiber::http::http_header_name_hash(self->pending_name);
        return IoErr::None;
    }

    static IoErr on_value_raw(void *ctx, const std::uint8_t *data, std::size_t len,
                              Http2HpackDecoder::FieldView *) noexcept {
        auto *self = static_cast<DecodeRecorder *>(ctx);
        self->fields.emplace_back(self->pending_name + "=" + std::string(reinterpret_cast<const char *>(data), len));
        self->pending_name.clear();
        self->pending_name_hash = 0;
        return IoErr::None;
    }

    static IoErr on_value_huffman(void *ctx, const std::uint8_t *data, std::size_t len,
                                  Http2HpackDecoder::FieldView *) noexcept {
        auto *self = static_cast<DecodeRecorder *>(ctx);
        bool ok = false;
        const std::size_t decoded_len = fiber::http::http2_huffman_decoded_length(data, len, &ok);
        if (!ok) {
            return IoErr::Invalid;
        }
        std::string value(decoded_len, '\0');
        fiber::http::Http2HuffmanDecodeState state;
        auto result = fiber::http::http2_huffman_decode_exact(
            state, data, len, reinterpret_cast<std::uint8_t *>(value.data()), true);
        if (result.code != fiber::http::Http2HuffmanCode::Ok || result.written != decoded_len) {
            return IoErr::Invalid;
        }
        self->fields.emplace_back(self->pending_name + "=" + value);
        self->pending_name.clear();
        self->pending_name_hash = 0;
        return IoErr::None;
    }

    static const Http2HpackDecoder::Ops &ops() noexcept {
        static const Http2HpackDecoder::Ops kOps{
            &DecodeRecorder::on_indexed_field,
            &DecodeRecorder::on_indexed_name,
            &DecodeRecorder::on_name_raw,
            &DecodeRecorder::on_name_huffman,
            &DecodeRecorder::on_value_raw,
            &DecodeRecorder::on_value_huffman,
        };
        return kOps;
    }

    std::vector<std::string> fields;
    std::string pending_name;
    std::uint64_t pending_name_hash = 0;
};

TEST(Http2HpackEncoderTest, EncodesStaticExactAsIndexedField) {
    Http2HpackEncodeCatalog catalog;
    ASSERT_TRUE(catalog.init({}));

    Http2HpackEncoder encoder({.catalog = &catalog});
    ASSERT_TRUE(encoder.init());
    Http2HpackEncoderIoBufWriter writer(encoder);
    ASSERT_EQ(writer.begin(), IoErr::None);
    ASSERT_EQ(writer.encode_status(200), IoErr::None);

    IoBufChain block;
    ASSERT_EQ(writer.finish(block), IoErr::None);
    EXPECT_EQ(chain_to_bytes(std::move(block)), (std::vector<std::uint8_t>{0x88}));
}

TEST(Http2HpackEncoderTest, EncodesNonStaticStatusUsingIndexedName) {
    Http2HpackEncodeCatalog catalog;
    ASSERT_TRUE(catalog.init({}));

    Http2HpackEncoder encoder({.catalog = &catalog, .huffman_threshold = 1024});
    ASSERT_TRUE(encoder.init());
    Http2HpackEncoderIoBufWriter writer(encoder);
    ASSERT_EQ(writer.begin(), IoErr::None);
    ASSERT_EQ(writer.encode_status(418), IoErr::None);

    IoBufChain block;
    ASSERT_EQ(writer.finish(block), IoErr::None);
    const std::vector<std::uint8_t> bytes = chain_to_bytes(std::move(block));
    EXPECT_EQ(bytes, (std::vector<std::uint8_t>{0x08, 0x03, '4', '1', '8'}));

    Http2HpackDecoder decoder;
    ASSERT_TRUE(decoder.init());
    DecodeRecorder recorder;
    decoder.begin_block(&recorder, &DecodeRecorder::ops());
    ASSERT_EQ(decoder.decode(bytes.data(), bytes.size(), true), IoErr::None);
    ASSERT_EQ(recorder.fields.size(), 1U);
    EXPECT_EQ(recorder.fields[0], ":status=418");
}

TEST(Http2HpackEncoderTest, EncodesStaticNameMatchWithoutIndexingAndDecodesBack) {
    Http2HpackEncodeCatalog catalog;
    ASSERT_TRUE(catalog.init({}));

    Http2HpackEncoder encoder({.catalog = &catalog, .huffman_threshold = 1024});
    ASSERT_TRUE(encoder.init());
    Http2HpackEncoderIoBufWriter writer(encoder);
    ASSERT_EQ(writer.begin(), IoErr::None);
    ASSERT_EQ(writer.encode_field("content-type", fiber::http::http_header_name_hash("content-type"), "text/plain"),
              IoErr::None);

    IoBufChain block;
    ASSERT_EQ(writer.finish(block), IoErr::None);
    const std::vector<std::uint8_t> bytes = chain_to_bytes(std::move(block));
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(bytes[0], 0x0f);

    Http2HpackDecoder decoder;
    ASSERT_TRUE(decoder.init());
    DecodeRecorder recorder;
    decoder.begin_block(&recorder, &DecodeRecorder::ops());
    ASSERT_EQ(decoder.decode(bytes.data(), bytes.size(), true), IoErr::None);
    ASSERT_EQ(recorder.fields.size(), 1U);
    EXPECT_EQ(recorder.fields[0], "content-type=text/plain");
}

TEST(Http2HpackEncoderTest, ActivatesPolicyEntryAndReusesDynamicIndexOnSecondBlock) {
    constexpr std::array<Http2HpackEncodeCatalog::PolicyEntry, 1> kPolicies{{
        {"server", fiber::http::http_header_name_hash("server"), "nginx-1.25.1"},
    }};

    Http2HpackEncodeCatalog catalog;
    ASSERT_TRUE(catalog.init(kPolicies));

    Http2HpackEncoder encoder({.catalog = &catalog, .huffman_threshold = 1024});
    ASSERT_TRUE(encoder.init());

    Http2HpackEncoderIoBufWriter first_writer(encoder);
    ASSERT_EQ(first_writer.begin(), IoErr::None);
    ASSERT_EQ(first_writer.encode_field("server", fiber::http::http_header_name_hash("server"), "nginx-1.25.1"),
              IoErr::None);
    IoBufChain first_block;
    ASSERT_EQ(first_writer.finish(first_block), IoErr::None);
    EXPECT_EQ(chain_to_bytes(std::move(first_block)),
              (std::vector<std::uint8_t>{0x76, 0x0c, 'n', 'g', 'i', 'n', 'x', '-', '1', '.', '2', '5', '.', '1'}));

    Http2HpackEncoderIoBufWriter second_writer(encoder);
    ASSERT_EQ(second_writer.begin(), IoErr::None);
    ASSERT_EQ(second_writer.encode_field("server", fiber::http::http_header_name_hash("server"), "nginx-1.25.1"),
              IoErr::None);
    IoBufChain second_block;
    ASSERT_EQ(second_writer.finish(second_block), IoErr::None);
    EXPECT_EQ(chain_to_bytes(std::move(second_block)), (std::vector<std::uint8_t>{0xbe}));
}

TEST(Http2HpackEncoderTest, UsesRawOrHuffmanStringEncodingBasedOnThreshold) {
    Http2HpackEncodeCatalog catalog;
    ASSERT_TRUE(catalog.init({}));

    Http2HpackEncoder raw_encoder({.catalog = &catalog, .huffman_threshold = 1024});
    ASSERT_TRUE(raw_encoder.init());
    Http2HpackEncoderIoBufWriter raw_writer(raw_encoder);
    ASSERT_EQ(raw_writer.begin(), IoErr::None);
    ASSERT_EQ(raw_writer.encode_field("x-test", fiber::http::http_header_name_hash("x-test"), "abc"), IoErr::None);
    IoBufChain raw_block;
    ASSERT_EQ(raw_writer.finish(raw_block), IoErr::None);
    const std::vector<std::uint8_t> raw_bytes = chain_to_bytes(std::move(raw_block));
    ASSERT_GE(raw_bytes.size(), 2U);
    EXPECT_EQ(raw_bytes[0], 0x00);
    EXPECT_EQ(raw_bytes[1] & 0x80U, 0x00U);

    Http2HpackEncoder huffman_encoder({.catalog = &catalog, .huffman_threshold = 1});
    ASSERT_TRUE(huffman_encoder.init());
    Http2HpackEncoderIoBufWriter huffman_writer(huffman_encoder);
    ASSERT_EQ(huffman_writer.begin(), IoErr::None);
    ASSERT_EQ(huffman_writer.encode_field("x-test", fiber::http::http_header_name_hash("x-test"), "abc"),
              IoErr::None);
    IoBufChain huffman_block;
    ASSERT_EQ(huffman_writer.finish(huffman_block), IoErr::None);
    const std::vector<std::uint8_t> huffman_bytes = chain_to_bytes(std::move(huffman_block));
    ASSERT_GE(huffman_bytes.size(), 2U);
    EXPECT_EQ(huffman_bytes[0], 0x00);
    EXPECT_EQ(huffman_bytes[1] & 0x80U, 0x80U);
}

TEST(Http2HpackEncoderTest, EmitsTableSizeUpdateAtStartOfNextBlock) {
    Http2HpackEncodeCatalog catalog;
    ASSERT_TRUE(catalog.init({}));

    Http2HpackEncoder encoder({.catalog = &catalog, .max_dynamic_table_size = 256});
    ASSERT_TRUE(encoder.init());
    encoder.update_max_dynamic_table_size(128);

    Http2HpackEncoderIoBufWriter writer(encoder);
    ASSERT_EQ(writer.begin(), IoErr::None);
    IoBufChain block;
    ASSERT_EQ(writer.finish(block), IoErr::None);
    EXPECT_EQ(chain_to_bytes(std::move(block)), (std::vector<std::uint8_t>{0x3f, 0x61}));
}

} // namespace
