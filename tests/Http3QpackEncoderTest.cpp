#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <fiber/common/IoError.h>
#include <fiber/common/mem/IoBuf.h>
#include <fiber/http/Http3Protocol.h>
#include <fiber/http/Http3QpackDecoder.h>
#include <fiber/http/Http3QpackEncoder.h>
#include <fiber/http/Http3QpackEncoderIoBufWriter.h>
#include <fiber/http/HttpHeaderHash.h>
#include <fiber/http/Huffman.h>
#include <fiber/quic/QuicCursor.h>
#include <fiber/quic/QuicTransportCodec.h>

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
        self->raw_value_count++;
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
        self->huff_value_count++;
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
    std::size_t raw_value_count = 0;
    std::size_t huff_value_count = 0;
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

TEST(Http3QpackEncoderTest, DoesNotHuffmanEncodeValueThatWouldExpand) {
    // RFC 9204 §4.5: Huffman must only be used when it shortens the string.
    // "!!!" expands under Huffman (3 -> 4 bytes), so it must be sent raw even
    // though the threshold gate (here 1) would otherwise permit Huffman.
    IoBufNodePool pool;
    Http3QpackEncoderIoBufWriter writer(pool, Http3QpackEncoder::Options{.huffman_threshold = 1});
    ASSERT_EQ(writer.encode_field("content-type", fiber::http::http_header_name_hash("content-type"), "!!!"),
              IoErr::None);
    IoBufChain block(pool);
    ASSERT_EQ(writer.finish(block), IoErr::None);
    const std::vector<std::uint8_t> bytes = chain_to_bytes(std::move(block));

    DecodeRecorder recorder;
    decode_all(bytes, recorder);
    ASSERT_EQ(recorder.fields.size(), 1U);
    EXPECT_EQ(recorder.fields[0], "content-type=!!!");
    EXPECT_EQ(recorder.huff_value_count, 0U);
    EXPECT_EQ(recorder.raw_value_count, 1U);
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

TEST(Http3QpackEncoderTest, UsesNewTailWhenContiguousHuffmanOutputDoesNotFit) {
    IoBufNodePool pool;
    Http3QpackEncoderIoBufWriter writer(pool, Http3QpackEncoder::Options{.huffman_threshold = 1}, 4);
    ASSERT_EQ(writer.encode_field("x-test", fiber::http::http_header_name_hash("x-test"), "abc"), IoErr::None);

    IoBufChain block(pool);
    ASSERT_EQ(writer.finish(block), IoErr::None);
    ASSERT_GT(block.size(), 1U);
    ASSERT_NE(block.front(), nullptr);
    EXPECT_GT(block.front()->writable(), 0U);

    const std::vector<std::uint8_t> bytes = chain_to_bytes(std::move(block));
    DecodeRecorder recorder;
    decode_all(bytes, recorder);
    ASSERT_EQ(recorder.fields.size(), 1U);
    EXPECT_EQ(recorder.fields[0], "x-test=abc");
}

TEST(Http3QpackEncoderTest, SupportsReservedPrefixForHttp3HeadersFrame) {
    constexpr std::size_t kReserve = 16;
    IoBufNodePool pool;
    Http3QpackEncoderIoBufWriter writer(pool, Http3QpackEncoder::Options{.huffman_threshold = 1024}, 4, kReserve);
    ASSERT_EQ(writer.encode_status(204), IoErr::None);
    ASSERT_EQ(writer.encode_field("server", fiber::http::http_header_name_hash("server"), "fiber"), IoErr::None);

    IoBufChain frame(pool);
    ASSERT_EQ(writer.finish(frame), IoErr::None);
    ASSERT_NE(writer.prefix_reserved_data(), nullptr);
    ASSERT_EQ(writer.prefix_reserved_size(), kReserve);
    ASSERT_GE(frame.readable_bytes(), kReserve);

    const std::size_t payload_len = frame.readable_bytes() - kReserve;
    const std::size_t header_len =
            fiber::quic::quic_varint_len(static_cast<std::uint64_t>(fiber::http::Http3FrameType::Headers)) +
            fiber::quic::quic_varint_len(payload_len);
    ASSERT_LE(header_len, kReserve);

    const std::size_t gap = kReserve - header_len;
    fiber::quic::QuicWriteCursor cursor(writer.prefix_reserved_data() + gap, header_len);
    ASSERT_TRUE(fiber::quic::quic_write_varint(cursor, static_cast<std::uint64_t>(fiber::http::Http3FrameType::Headers))
                        .has_value());
    ASSERT_TRUE(fiber::quic::quic_write_varint(cursor, payload_len).has_value());
    ASSERT_EQ(cursor.offset(), header_len);
    frame.consume_and_compact(gap);

    const std::vector<std::uint8_t> bytes = chain_to_bytes(std::move(frame));
    ASSERT_EQ(bytes.size(), header_len + payload_len);
    ASSERT_EQ(bytes[0], static_cast<std::uint8_t>(fiber::http::Http3FrameType::Headers));
    ASSERT_EQ(bytes[1], payload_len);
    ASSERT_GE(bytes.size(), header_len + 2U);
    EXPECT_EQ(bytes[header_len], 0x00);
    EXPECT_EQ(bytes[header_len + 1], 0x00);

    std::vector<std::uint8_t> payload(bytes.begin() + static_cast<std::ptrdiff_t>(header_len), bytes.end());
    DecodeRecorder recorder;
    decode_all(payload, recorder);
    ASSERT_EQ(recorder.fields.size(), 2U);
    EXPECT_EQ(recorder.fields[0], ":status=204");
    EXPECT_EQ(recorder.fields[1], "server=fiber");
}

TEST(Http3QpackEncoderTest, RejectsStringsAboveLimit) {
    IoBufNodePool pool;
    Http3QpackEncoderIoBufWriter writer(pool,
                                        Http3QpackEncoder::Options{.max_string_size = 2, .huffman_threshold = 1024});
    EXPECT_EQ(writer.encode_field("x", fiber::http::http_header_name_hash("x"), "abc"), IoErr::Invalid);
    writer.abort();
}

// The following cases exercise the O(1) pseudo-header fast path (scheme a): an
// in-table value must emit a single indexed byte for its exact static index, and a
// non-in-table value must fall back to a static name reference at the first
// same-name index. Both paths must stay byte-identical to the general find() route.

TEST(Http3QpackEncoderTest, EncodesStatus500AsIndexedField) {
    IoBufNodePool pool;
    Http3QpackEncoderIoBufWriter writer(pool, Http3QpackEncoder::Options{.huffman_threshold = 1024});
    ASSERT_EQ(writer.encode_status(500), IoErr::None);

    IoBufChain block(pool);
    ASSERT_EQ(writer.finish(block), IoErr::None);
    // 500 -> QPACK static index 71; 71 >= 6-bit prefix max (63) -> 0xff, 71-63=8 -> 0x08.
    EXPECT_EQ(chain_to_bytes(std::move(block)), (std::vector<std::uint8_t>{0x00, 0x00, 0xff, 0x08}));
}

TEST(Http3QpackEncoderTest, EncodesStatus502UsingStaticNameReference) {
    IoBufNodePool pool;
    Http3QpackEncoderIoBufWriter writer(pool, Http3QpackEncoder::Options{.huffman_threshold = 1024});
    ASSERT_EQ(writer.encode_status(502), IoErr::None);

    IoBufChain block(pool);
    ASSERT_EQ(writer.finish(block), IoErr::None);
    // 502 not in static table -> name ref @24 (first :status), literal "502".
    const std::vector<std::uint8_t> bytes = chain_to_bytes(std::move(block));
    EXPECT_EQ(bytes, (std::vector<std::uint8_t>{0x00, 0x00, 0x5f, 0x09, 0x03, '5', '0', '2'}));

    DecodeRecorder recorder;
    decode_all(bytes, recorder);
    ASSERT_EQ(recorder.fields.size(), 1U);
    EXPECT_EQ(recorder.fields[0], ":status=502");
}

TEST(Http3QpackEncoderTest, EncodesPostAsIndexedField) {
    IoBufNodePool pool;
    Http3QpackEncoderIoBufWriter writer(pool, Http3QpackEncoder::Options{.huffman_threshold = 1024});
    ASSERT_EQ(writer.encode_method(HttpMethod::Post), IoErr::None);

    IoBufChain block(pool);
    ASSERT_EQ(writer.finish(block), IoErr::None);
    // POST -> QPACK static index 20; 0xc0 | 20 = 0xd4.
    EXPECT_EQ(chain_to_bytes(std::move(block)), (std::vector<std::uint8_t>{0x00, 0x00, 0xd4}));
}

TEST(Http3QpackEncoderTest, EncodesNonStaticMethodUsingStaticNameReference) {
    IoBufNodePool pool;
    Http3QpackEncoderIoBufWriter writer(pool, Http3QpackEncoder::Options{.huffman_threshold = 1024});
    ASSERT_EQ(writer.encode_method(HttpMethod::MKCOL), IoErr::None);

    IoBufChain block(pool);
    ASSERT_EQ(writer.finish(block), IoErr::None);
    // MKCOL not in static table -> name ref @15 (first :method); 15 == 4-bit prefix
    // max -> 0x5f, 15-15=0 -> 0x00, then literal "MKCOL".
    const std::vector<std::uint8_t> bytes = chain_to_bytes(std::move(block));
    EXPECT_EQ(bytes, (std::vector<std::uint8_t>{0x00, 0x00, 0x5f, 0x00, 0x05, 'M', 'K', 'C', 'O', 'L'}));

    DecodeRecorder recorder;
    decode_all(bytes, recorder);
    ASSERT_EQ(recorder.fields.size(), 1U);
    EXPECT_EQ(recorder.fields[0], ":method=MKCOL");
}

TEST(Http3QpackEncoderTest, EncodesSchemeHttpsAndHttpAsIndexedField) {
    IoBufNodePool pool;
    Http3QpackEncoderIoBufWriter w_https(pool, Http3QpackEncoder::Options{.huffman_threshold = 1024});
    ASSERT_EQ(w_https.encode_scheme("https"), IoErr::None);
    IoBufChain b_https(pool);
    ASSERT_EQ(w_https.finish(b_https), IoErr::None);
    // https -> index 23; 0xc0 | 23 = 0xd7.
    EXPECT_EQ(chain_to_bytes(std::move(b_https)), (std::vector<std::uint8_t>{0x00, 0x00, 0xd7}));

    Http3QpackEncoderIoBufWriter w_http(pool, Http3QpackEncoder::Options{.huffman_threshold = 1024});
    ASSERT_EQ(w_http.encode_scheme("http"), IoErr::None);
    IoBufChain b_http(pool);
    ASSERT_EQ(w_http.finish(b_http), IoErr::None);
    // http -> index 22; 0xc0 | 22 = 0xd6.
    EXPECT_EQ(chain_to_bytes(std::move(b_http)), (std::vector<std::uint8_t>{0x00, 0x00, 0xd6}));
}

TEST(Http3QpackEncoderTest, EncodesNonStaticSchemeUsingStaticNameReference) {
    IoBufNodePool pool;
    Http3QpackEncoderIoBufWriter writer(pool, Http3QpackEncoder::Options{.huffman_threshold = 1024});
    ASSERT_EQ(writer.encode_scheme("ftp"), IoErr::None);

    IoBufChain block(pool);
    ASSERT_EQ(writer.finish(block), IoErr::None);
    // ftp not in static table -> name ref @22; 22 >= 15 -> 0x5f, 22-15=7 -> 0x07, "ftp".
    const std::vector<std::uint8_t> bytes = chain_to_bytes(std::move(block));
    EXPECT_EQ(bytes, (std::vector<std::uint8_t>{0x00, 0x00, 0x5f, 0x07, 0x03, 'f', 't', 'p'}));

    DecodeRecorder recorder;
    decode_all(bytes, recorder);
    ASSERT_EQ(recorder.fields.size(), 1U);
    EXPECT_EQ(recorder.fields[0], ":scheme=ftp");
}

TEST(Http3QpackEncoderTest, EncodesRootPathAsIndexedField) {
    IoBufNodePool pool;
    Http3QpackEncoderIoBufWriter writer(pool, Http3QpackEncoder::Options{.huffman_threshold = 1024});
    ASSERT_EQ(writer.encode_path("/"), IoErr::None);

    IoBufChain block(pool);
    ASSERT_EQ(writer.finish(block), IoErr::None);
    // "/" -> QPACK static index 1; 0xc0 | 1 = 0xc1.
    EXPECT_EQ(chain_to_bytes(std::move(block)), (std::vector<std::uint8_t>{0x00, 0x00, 0xc1}));
}
