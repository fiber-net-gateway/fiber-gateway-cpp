#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/IoError.h"
#include "common/mem/IoBuf.h"
#include "http/Http3QpackDecoder.h"
#include "http/Http3QpackEncoder.h"
#include "http/Http3QpackEncoderIoBufWriter.h"
#include "http/HttpHeaderHash.h"
#include "http/Huffman.h"

namespace {

using fiber::common::IoErr;
using fiber::http::Http3QpackDecoder;
using fiber::http::Http3QpackEncoder;
using fiber::http::Http3QpackEncoderIoBufWriter;
using fiber::http::HttpMethod;
using fiber::mem::IoBufChain;
using fiber::mem::IoBufNodePool;

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

void decode_all(const std::vector<std::uint8_t> &bytes, DecodeRecorder &recorder) {
    Http3QpackDecoder decoder;
    ASSERT_TRUE(decoder.init());
    decoder.begin_block(&recorder, &DecodeRecorder::ops());
    ASSERT_EQ(decoder.decode(bytes.data(), bytes.size(), true), IoErr::None);
}

} // namespace

TEST(Http3QpackEncoderTest, EmitsEmptyBlockPrefix) {
    IoBufNodePool pool;
    Http3QpackEncoderIoBufWriter writer(pool);

    IoBufChain block(pool);
    ASSERT_EQ(writer.finish(block), IoErr::None);
    EXPECT_EQ(chain_to_bytes(std::move(block)), (std::vector<std::uint8_t>{0x00, 0x00}));
}

TEST(Http3QpackEncoderTest, EncodesStaticExactMethodAsIndexedField) {
    IoBufNodePool pool;
    Http3QpackEncoderIoBufWriter writer(pool, Http3QpackEncoder::Options{.huffman_threshold = 1024});
    ASSERT_EQ(writer.encode_method(HttpMethod::Get), IoErr::None);

    IoBufChain block(pool);
    ASSERT_EQ(writer.finish(block), IoErr::None);
    const std::vector<std::uint8_t> bytes = chain_to_bytes(std::move(block));
    EXPECT_EQ(bytes, (std::vector<std::uint8_t>{0x00, 0x00, 0xd1}));

    DecodeRecorder recorder;
    decode_all(bytes, recorder);
    ASSERT_EQ(recorder.fields.size(), 1U);
    EXPECT_EQ(recorder.fields[0], ":method=GET");
}

TEST(Http3QpackEncoderTest, EncodesStaticExactStatusAsIndexedField) {
    IoBufNodePool pool;
    Http3QpackEncoderIoBufWriter writer(pool, Http3QpackEncoder::Options{.huffman_threshold = 1024});
    ASSERT_EQ(writer.encode_status(200), IoErr::None);

    IoBufChain block(pool);
    ASSERT_EQ(writer.finish(block), IoErr::None);
    EXPECT_EQ(chain_to_bytes(std::move(block)), (std::vector<std::uint8_t>{0x00, 0x00, 0xd9}));
}

TEST(Http3QpackEncoderTest, EncodesNonStaticStatusUsingStaticNameReference) {
    IoBufNodePool pool;
    Http3QpackEncoderIoBufWriter writer(pool, Http3QpackEncoder::Options{.huffman_threshold = 1024});
    ASSERT_EQ(writer.encode_status(418), IoErr::None);

    IoBufChain block(pool);
    ASSERT_EQ(writer.finish(block), IoErr::None);
    const std::vector<std::uint8_t> bytes = chain_to_bytes(std::move(block));
    EXPECT_EQ(bytes, (std::vector<std::uint8_t>{0x00, 0x00, 0x5f, 0x09, 0x03, '4', '1', '8'}));

    DecodeRecorder recorder;
    decode_all(bytes, recorder);
    ASSERT_EQ(recorder.fields.size(), 1U);
    EXPECT_EQ(recorder.fields[0], ":status=418");
}

TEST(Http3QpackEncoderTest, EncodesPathUsingStaticNameReference) {
    IoBufNodePool pool;
    Http3QpackEncoderIoBufWriter writer(pool, Http3QpackEncoder::Options{.huffman_threshold = 1024});
    ASSERT_EQ(writer.encode_path("/index.html"), IoErr::None);

    IoBufChain block(pool);
    ASSERT_EQ(writer.finish(block), IoErr::None);
    const std::vector<std::uint8_t> bytes = chain_to_bytes(std::move(block));
    EXPECT_EQ(bytes, (std::vector<std::uint8_t>{0x00, 0x00, 0x51, 0x0b, '/', 'i', 'n', 'd', 'e', 'x', '.', 'h', 't',
                                                'm', 'l'}));
}

TEST(Http3QpackEncoderTest, EncodesAuthorityUsingStaticNameIndexZero) {
    IoBufNodePool pool;
    Http3QpackEncoderIoBufWriter writer(pool, Http3QpackEncoder::Options{.huffman_threshold = 1024});
    ASSERT_EQ(writer.encode_authority("example.com"), IoErr::None);

    IoBufChain block(pool);
    ASSERT_EQ(writer.finish(block), IoErr::None);
    const std::vector<std::uint8_t> bytes = chain_to_bytes(std::move(block));
    ASSERT_GE(bytes.size(), 4U);
    EXPECT_EQ(bytes[0], 0x00);
    EXPECT_EQ(bytes[1], 0x00);
    EXPECT_EQ(bytes[2], 0x50);

    DecodeRecorder recorder;
    decode_all(bytes, recorder);
    ASSERT_EQ(recorder.fields.size(), 1U);
    EXPECT_EQ(recorder.fields[0], ":authority=example.com");
}

TEST(Http3QpackEncoderTest, EncodesLiteralNameAndDecodesBack) {
    IoBufNodePool pool;
    Http3QpackEncoderIoBufWriter writer(pool, Http3QpackEncoder::Options{.huffman_threshold = 1024});
    ASSERT_EQ(writer.encode_field("x-test", fiber::http::http_header_name_hash("x-test"), "ok"), IoErr::None);

    IoBufChain block(pool);
    ASSERT_EQ(writer.finish(block), IoErr::None);
    const std::vector<std::uint8_t> bytes = chain_to_bytes(std::move(block));
    EXPECT_EQ(bytes, (std::vector<std::uint8_t>{0x00, 0x00, 0x26, 'x', '-', 't', 'e', 's', 't', 0x02, 'o', 'k'}));

    DecodeRecorder recorder;
    decode_all(bytes, recorder);
    ASSERT_EQ(recorder.fields.size(), 1U);
    EXPECT_EQ(recorder.fields[0], "x-test=ok");
}

TEST(Http3QpackEncoderTest, EncodesHuffmanNameAndValue) {
    IoBufNodePool pool;
    Http3QpackEncoderIoBufWriter writer(pool, Http3QpackEncoder::Options{.huffman_threshold = 1});
    ASSERT_EQ(writer.encode_field("x-test", fiber::http::http_header_name_hash("x-test"), "ok"), IoErr::None);

    IoBufChain block(pool);
    ASSERT_EQ(writer.finish(block), IoErr::None);
    const std::vector<std::uint8_t> bytes = chain_to_bytes(std::move(block));
    ASSERT_GE(bytes.size(), 3U);
    EXPECT_EQ(bytes[0], 0x00);
    EXPECT_EQ(bytes[1], 0x00);
    EXPECT_EQ(bytes[2] & 0xf8U, 0x28U);

    DecodeRecorder recorder;
    decode_all(bytes, recorder);
    ASSERT_EQ(recorder.fields.size(), 1U);
    EXPECT_EQ(recorder.fields[0], "x-test=ok");
}

TEST(Http3QpackEncoderTest, SupportsSmallWriterChunks) {
    IoBufNodePool pool;
    Http3QpackEncoderIoBufWriter writer(pool, Http3QpackEncoder::Options{.huffman_threshold = 1024}, 1);
    ASSERT_EQ(writer.encode_field("x-test", fiber::http::http_header_name_hash("x-test"), "chunked"), IoErr::None);

    IoBufChain block(pool);
    ASSERT_EQ(writer.finish(block), IoErr::None);
    const std::vector<std::uint8_t> bytes = chain_to_bytes(std::move(block));

    DecodeRecorder recorder;
    decode_all(bytes, recorder);
    ASSERT_EQ(recorder.fields.size(), 1U);
    EXPECT_EQ(recorder.fields[0], "x-test=chunked");
}

TEST(Http3QpackEncoderTest, RejectsStringsAboveLimit) {
    IoBufNodePool pool;
    Http3QpackEncoderIoBufWriter writer(pool,
                                        Http3QpackEncoder::Options{.max_string_size = 2, .huffman_threshold = 1024});
    EXPECT_EQ(writer.encode_field("x", fiber::http::http_header_name_hash("x"), "abc"), IoErr::Invalid);
    writer.abort();
}
