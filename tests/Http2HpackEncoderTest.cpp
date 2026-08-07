#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <fiber/common/mem/IoBuf.h>
#include <fiber/http/Http2HpackDecoder.h>
#include <fiber/http/Http2HpackEncoder.h>
#include <fiber/http/Http2HpackEncoderIoBufWriter.h>
#include <fiber/http/HttpHeaderHash.h>
#include <fiber/http/Huffman.h>

namespace {

using fiber::common::IoErr;
using fiber::http::Http2HpackDecoder;
using fiber::http::Http2HpackEncoder;
using fiber::http::Http2HpackEncoderIoBufWriter;
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

struct FixedEncoderOutput {
    static IoErr acquire(void *ctx, std::size_t min_bytes, std::uint8_t *&dst, std::size_t &len) noexcept {
        auto *self = static_cast<FixedEncoderOutput *>(ctx);
        len = self->bytes.size() - self->used;
        if (len < min_bytes) {
            dst = nullptr;
            len = 0;
            return IoErr::NoMem;
        }
        dst = self->bytes.data() + self->used;
        return IoErr::None;
    }

    static void commit(void *ctx, std::size_t written) noexcept {
        auto *self = static_cast<FixedEncoderOutput *>(ctx);
        self->used += written;
    }

    static const Http2HpackEncoder::OutputOps &ops() noexcept {
        static const Http2HpackEncoder::OutputOps kOps{&FixedEncoderOutput::acquire, &FixedEncoderOutput::commit};
        return kOps;
    }

    std::array<std::uint8_t, 64> bytes{};
    std::size_t used = 0;
};

struct DecodeRecorder {
    static IoErr on_indexed_field(void *ctx, Http2HpackDecoder::TableEntryView entry) noexcept {
        auto *self = static_cast<DecodeRecorder *>(ctx);
        self->fields.emplace_back(std::string(entry.name) + "=" + std::string(entry.value));
        return IoErr::None;
    }

    static IoErr on_indexed_name(void *ctx, Http2HpackDecoder::TableEntryView entry) noexcept {
        auto *self = static_cast<DecodeRecorder *>(ctx);
        self->pending_name = std::string(entry.name);
        self->pending_name_hash = entry.name_hash;
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
        auto result = fiber::http::hpack_huffman_decode_exact(
                state, data, len, reinterpret_cast<std::uint8_t *>(self->pending_name.data()), true);
        if (result.code != fiber::http::HpackHuffmanCode::Ok || result.written != decoded_len) {
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
        self->raw_value_count++;
        return IoErr::None;
    }

    static IoErr on_value_huffman(void *ctx, const std::uint8_t *data, std::size_t len,
                                  Http2HpackDecoder::FieldView *) noexcept {
        auto *self = static_cast<DecodeRecorder *>(ctx);
        bool ok = false;
        const std::size_t decoded_len = fiber::http::hpack_huffman_decoded_length(data, len, &ok);
        if (!ok) {
            return IoErr::Invalid;
        }
        std::string value(decoded_len, '\0');
        fiber::http::HpackHuffmanDecodeState state;
        auto result = fiber::http::hpack_huffman_decode_exact(state, data, len,
                                                              reinterpret_cast<std::uint8_t *>(value.data()), true);
        if (result.code != fiber::http::HpackHuffmanCode::Ok || result.written != decoded_len) {
            return IoErr::Invalid;
        }
        self->fields.emplace_back(self->pending_name + "=" + value);
        self->pending_name.clear();
        self->pending_name_hash = 0;
        self->huff_value_count++;
        return IoErr::None;
    }

    static const Http2HpackDecoder::Ops &ops() noexcept {
        static const Http2HpackDecoder::Ops kOps{
                &DecodeRecorder::on_indexed_field, &DecodeRecorder::on_indexed_name, &DecodeRecorder::on_name_raw,
                &DecodeRecorder::on_name_huffman,  &DecodeRecorder::on_value_raw,    &DecodeRecorder::on_value_huffman,
        };
        return kOps;
    }

    std::vector<std::string> fields;
    std::string pending_name;
    std::uint64_t pending_name_hash = 0;
    std::size_t raw_value_count = 0;
    std::size_t huff_value_count = 0;
};

TEST(Http2HpackEncoderTest, EncodesStaticExactAsIndexedField) {
    IoBufNodePool pool;
    Http2HpackEncoder encoder({});
    Http2HpackEncoderIoBufWriter writer(encoder, pool);
    ASSERT_EQ(writer.begin(), IoErr::None);
    ASSERT_EQ(writer.encode_status(200), IoErr::None);

    IoBufChain block(pool);
    ASSERT_EQ(writer.finish(block), IoErr::None);
    EXPECT_EQ(chain_to_bytes(std::move(block)), (std::vector<std::uint8_t>{0x20, 0x88}));
}

TEST(Http2HpackEncoderTest, EncodesCommonPseudoHeadersAsFixedStaticIndexes) {
    Http2HpackEncoder encoder({});
    FixedEncoderOutput output;
    ASSERT_EQ(encoder.begin_block(&output, &FixedEncoderOutput::ops()), IoErr::None);

    ASSERT_EQ(encoder.encode_method(fiber::http::HttpMethod::Get), IoErr::None);
    ASSERT_EQ(encoder.encode_method(fiber::http::HttpMethod::Post), IoErr::None);
    ASSERT_EQ(encoder.encode_scheme("http"), IoErr::None);
    ASSERT_EQ(encoder.encode_scheme("https"), IoErr::None);
    ASSERT_EQ(encoder.encode_authority({}), IoErr::None);
    ASSERT_EQ(encoder.encode_path("/"), IoErr::None);
    ASSERT_EQ(encoder.encode_path("/index.html"), IoErr::None);
    ASSERT_EQ(encoder.encode_status(200), IoErr::None);
    ASSERT_EQ(encoder.encode_status(204), IoErr::None);
    ASSERT_EQ(encoder.encode_status(206), IoErr::None);
    ASSERT_EQ(encoder.encode_status(304), IoErr::None);
    ASSERT_EQ(encoder.encode_status(400), IoErr::None);
    ASSERT_EQ(encoder.encode_status(404), IoErr::None);
    ASSERT_EQ(encoder.encode_status(500), IoErr::None);
    ASSERT_EQ(encoder.finish_block(), IoErr::None);

    const std::vector<std::uint8_t> bytes(output.bytes.begin(), output.bytes.begin() + output.used);
    EXPECT_EQ(bytes, (std::vector<std::uint8_t>{
                             0x20,
                             0x82,
                             0x83,
                             0x86,
                             0x87,
                             0x81,
                             0x84,
                             0x85,
                             0x88,
                             0x89,
                             0x8a,
                             0x8b,
                             0x8c,
                             0x8d,
                             0x8e,
                     }));
}

TEST(Http2HpackEncoderTest, EncodesNonStaticStatusUsingIndexedName) {
    IoBufNodePool pool;
    Http2HpackEncoder encoder({.huffman_threshold = 1024});
    Http2HpackEncoderIoBufWriter writer(encoder, pool);
    ASSERT_EQ(writer.begin(), IoErr::None);
    ASSERT_EQ(writer.encode_status(418), IoErr::None);

    IoBufChain block(pool);
    ASSERT_EQ(writer.finish(block), IoErr::None);
    const std::vector<std::uint8_t> bytes = chain_to_bytes(std::move(block));
    EXPECT_EQ(bytes, (std::vector<std::uint8_t>{0x20, 0x08, 0x03, '4', '1', '8'}));

    Http2HpackDecoder decoder;
    ASSERT_TRUE(decoder.init());
    DecodeRecorder recorder;
    decoder.begin_block(&recorder, &DecodeRecorder::ops());
    ASSERT_EQ(decoder.decode(bytes.data(), bytes.size(), true), IoErr::None);
    ASSERT_EQ(recorder.fields.size(), 1U);
    EXPECT_EQ(recorder.fields[0], ":status=418");
}

TEST(Http2HpackEncoderTest, EncodesStaticNameMatchWithoutIndexingAndDecodesBack) {
    IoBufNodePool pool;
    Http2HpackEncoder encoder({.huffman_threshold = 1024});
    Http2HpackEncoderIoBufWriter writer(encoder, pool);
    ASSERT_EQ(writer.begin(), IoErr::None);
    ASSERT_EQ(writer.encode_field("content-type", fiber::http::http_header_name_hash("content-type"), "text/plain"),
              IoErr::None);

    IoBufChain block(pool);
    ASSERT_EQ(writer.finish(block), IoErr::None);
    const std::vector<std::uint8_t> bytes = chain_to_bytes(std::move(block));
    ASSERT_FALSE(bytes.empty());
    ASSERT_GE(bytes.size(), 2U);
    EXPECT_EQ(bytes[0], 0x20);
    EXPECT_EQ(bytes[1], 0x0f);

    Http2HpackDecoder decoder;
    ASSERT_TRUE(decoder.init());
    DecodeRecorder recorder;
    decoder.begin_block(&recorder, &DecodeRecorder::ops());
    ASSERT_EQ(decoder.decode(bytes.data(), bytes.size(), true), IoErr::None);
    ASSERT_EQ(recorder.fields.size(), 1U);
    EXPECT_EQ(recorder.fields[0], "content-type=text/plain");
}

TEST(Http2HpackEncoderTest, RepeatedFieldsNeverUseDynamicIndexes) {
    IoBufNodePool pool;
    Http2HpackEncoder encoder({.huffman_threshold = 1024});

    Http2HpackEncoderIoBufWriter first_writer(encoder, pool);
    ASSERT_EQ(first_writer.begin(), IoErr::None);
    ASSERT_EQ(first_writer.encode_field("server", fiber::http::http_header_name_hash("server"), "nginx-1.25.1"),
              IoErr::None);
    IoBufChain first_block(pool);
    ASSERT_EQ(first_writer.finish(first_block), IoErr::None);
    EXPECT_EQ(chain_to_bytes(std::move(first_block)),
              (std::vector<std::uint8_t>{0x20, 0x0f, 0x27, 0x0c, 'n', 'g', 'i', 'n', 'x', '-', '1', '.', '2', '5', '.',
                                         '1'}));

    Http2HpackEncoderIoBufWriter second_writer(encoder, pool);
    ASSERT_EQ(second_writer.begin(), IoErr::None);
    ASSERT_EQ(second_writer.encode_field("server", fiber::http::http_header_name_hash("server"), "nginx-1.25.1"),
              IoErr::None);
    IoBufChain second_block(pool);
    ASSERT_EQ(second_writer.finish(second_block), IoErr::None);
    EXPECT_EQ(chain_to_bytes(std::move(second_block)),
              (std::vector<std::uint8_t>{0x20, 0x0f, 0x27, 0x0c, 'n', 'g', 'i', 'n', 'x', '-', '1', '.', '2', '5', '.',
                                         '1'}));
}

TEST(Http2HpackEncoderTest, UsesRawOrHuffmanStringEncodingBasedOnThreshold) {
    IoBufNodePool pool;
    Http2HpackEncoder raw_encoder({.huffman_threshold = 1024});
    Http2HpackEncoderIoBufWriter raw_writer(raw_encoder, pool);
    ASSERT_EQ(raw_writer.begin(), IoErr::None);
    ASSERT_EQ(raw_writer.encode_field("x-test", fiber::http::http_header_name_hash("x-test"), "abc"), IoErr::None);
    IoBufChain raw_block(pool);
    ASSERT_EQ(raw_writer.finish(raw_block), IoErr::None);
    const std::vector<std::uint8_t> raw_bytes = chain_to_bytes(std::move(raw_block));
    ASSERT_GE(raw_bytes.size(), 3U);
    EXPECT_EQ(raw_bytes[0], 0x20);
    EXPECT_EQ(raw_bytes[1], 0x00);
    EXPECT_EQ(raw_bytes[2] & 0x80U, 0x00U);

    Http2HpackEncoder huffman_encoder({.huffman_threshold = 1});
    Http2HpackEncoderIoBufWriter huffman_writer(huffman_encoder, pool);
    ASSERT_EQ(huffman_writer.begin(), IoErr::None);
    ASSERT_EQ(huffman_writer.encode_field("x-test", fiber::http::http_header_name_hash("x-test"), "abc"), IoErr::None);
    IoBufChain huffman_block(pool);
    ASSERT_EQ(huffman_writer.finish(huffman_block), IoErr::None);
    const std::vector<std::uint8_t> huffman_bytes = chain_to_bytes(std::move(huffman_block));
    ASSERT_GE(huffman_bytes.size(), 3U);
    EXPECT_EQ(huffman_bytes[0], 0x20);
    EXPECT_EQ(huffman_bytes[1], 0x00);
    EXPECT_EQ(huffman_bytes[2] & 0x80U, 0x80U);
}

TEST(Http2HpackEncoderTest, DoesNotHuffmanEncodeValueThatWouldExpand) {
    // RFC 7541 §6.2: Huffman must only be used when it shortens the string.
    // "!!!" expands under Huffman (3 -> 4 bytes), so it must be sent raw even
    // though the threshold gate (here 1) would otherwise permit Huffman.
    IoBufNodePool pool;
    Http2HpackEncoder encoder({.huffman_threshold = 1});
    Http2HpackEncoderIoBufWriter writer(encoder, pool);
    ASSERT_EQ(writer.begin(), IoErr::None);
    ASSERT_EQ(writer.encode_field("content-type", fiber::http::http_header_name_hash("content-type"), "!!!"),
              IoErr::None);
    IoBufChain block(pool);
    ASSERT_EQ(writer.finish(block), IoErr::None);
    const std::vector<std::uint8_t> bytes = chain_to_bytes(std::move(block));

    Http2HpackDecoder decoder;
    ASSERT_TRUE(decoder.init());
    DecodeRecorder recorder;
    decoder.begin_block(&recorder, &DecodeRecorder::ops());
    ASSERT_EQ(decoder.decode(bytes.data(), bytes.size(), true), IoErr::None);
    ASSERT_EQ(recorder.fields.size(), 1U);
    EXPECT_EQ(recorder.fields[0], "content-type=!!!");
    EXPECT_EQ(recorder.huff_value_count, 0U);
    EXPECT_EQ(recorder.raw_value_count, 1U);
}

TEST(Http2HpackEncoderTest, UsesNewTailWhenContiguousHuffmanOutputDoesNotFit) {
    IoBufNodePool pool;
    Http2HpackEncoder encoder({.huffman_threshold = 1});
    Http2HpackEncoderIoBufWriter writer(encoder, pool, 4);
    ASSERT_EQ(writer.begin(), IoErr::None);
    ASSERT_EQ(writer.encode_field("x-test", fiber::http::http_header_name_hash("x-test"), "abc"), IoErr::None);

    IoBufChain block(pool);
    ASSERT_EQ(writer.finish(block), IoErr::None);
    ASSERT_GT(block.size(), 1U);
    ASSERT_NE(block.front(), nullptr);
    EXPECT_GT(block.front()->writable(), 0U);

    const std::vector<std::uint8_t> bytes = chain_to_bytes(std::move(block));
    Http2HpackDecoder decoder;
    ASSERT_TRUE(decoder.init());
    DecodeRecorder recorder;
    decoder.begin_block(&recorder, &DecodeRecorder::ops());
    ASSERT_EQ(decoder.decode(bytes.data(), bytes.size(), true), IoErr::None);
    ASSERT_EQ(recorder.fields.size(), 1U);
    EXPECT_EQ(recorder.fields[0], "x-test=abc");
}

TEST(Http2HpackEncoderTest, EmitsTableSizeZeroOnEveryBlock) {
    IoBufNodePool pool;
    Http2HpackEncoder encoder({});

    Http2HpackEncoderIoBufWriter first_writer(encoder, pool);
    ASSERT_EQ(first_writer.begin(), IoErr::None);
    IoBufChain first_block(pool);
    ASSERT_EQ(first_writer.finish(first_block), IoErr::None);
    const std::vector<std::uint8_t> first_bytes = chain_to_bytes(std::move(first_block));
    EXPECT_EQ(first_bytes, (std::vector<std::uint8_t>{0x20}));

    Http2HpackEncoderIoBufWriter second_writer(encoder, pool);
    ASSERT_EQ(second_writer.begin(), IoErr::None);
    IoBufChain second_block(pool);
    ASSERT_EQ(second_writer.finish(second_block), IoErr::None);
    const std::vector<std::uint8_t> second_bytes = chain_to_bytes(std::move(second_block));
    EXPECT_EQ(second_bytes, (std::vector<std::uint8_t>{0x20}));

    Http2HpackDecoder decoder;
    ASSERT_TRUE(decoder.init());
    DecodeRecorder recorder;
    decoder.begin_block(&recorder, &DecodeRecorder::ops());
    ASSERT_EQ(decoder.decode(first_bytes.data(), first_bytes.size(), true), IoErr::None);
    decoder.begin_block(&recorder, &DecodeRecorder::ops());
    ASSERT_EQ(decoder.decode(second_bytes.data(), second_bytes.size(), true), IoErr::None);
}

TEST(Http2HpackEncoderTest, CanceledBlockDoesNotAffectNextBlock) {
    IoBufNodePool pool;
    Http2HpackEncoder encoder({});

    Http2HpackEncoderIoBufWriter canceled_writer(encoder, pool);
    ASSERT_EQ(canceled_writer.begin(), IoErr::None);
    canceled_writer.abort();

    Http2HpackEncoderIoBufWriter writer(encoder, pool);
    ASSERT_EQ(writer.begin(), IoErr::None);
    IoBufChain block(pool);
    ASSERT_EQ(writer.finish(block), IoErr::None);
    EXPECT_EQ(chain_to_bytes(std::move(block)), (std::vector<std::uint8_t>{0x20}));
}

} // namespace
