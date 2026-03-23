#ifndef FIBER_TESTS_HTTP2_TEST_SUPPORT_H
#define FIBER_TESTS_HTTP2_TEST_SUPPORT_H

#include <cstdint>
#include <new>
#include <string>

#include "http/Http2Connection.h"
#include "http/Http2HpackHuffman.h"
#include "http/Http2StreamFactory.h"
#include "http/HttpHeaderHash.h"

namespace {

struct TestHttp2StreamOwner {
    explicit TestHttp2StreamOwner(std::uint32_t stream_id) : stream(stream_id, this, ops()) {}

    static fiber::http::Http2Stream::Lease create(std::uint32_t stream_id) noexcept {
        auto *owner = new (std::nothrow) TestHttp2StreamOwner(stream_id);
        if (!owner) {
            return {};
        }
        return fiber::http::Http2Stream::Lease::adopt(&owner->stream);
    }

    static const fiber::http::Http2Stream::Ops &ops() noexcept {
        static const fiber::http::Http2HpackDecoder::Ops kDecoderOps{
            &TestHttp2StreamOwner::on_indexed_field,
            &TestHttp2StreamOwner::on_indexed_name,
            &TestHttp2StreamOwner::on_name_raw,
            &TestHttp2StreamOwner::on_name_huffman,
            &TestHttp2StreamOwner::on_value_raw,
            &TestHttp2StreamOwner::on_value_huffman,
        };
        static const fiber::http::Http2Stream::Ops kOps{
            &TestHttp2StreamOwner::destroy_owner,
            &TestHttp2StreamOwner::on_header_block_start,
            &TestHttp2StreamOwner::on_header_block_complete,
            &TestHttp2StreamOwner::on_body,
            &TestHttp2StreamOwner::on_abort,
        };
        (void)kDecoderOps;
        return kOps;
    }

    static void destroy_owner(void *owner) noexcept { delete static_cast<TestHttp2StreamOwner *>(owner); }
    static fiber::common::IoErr on_header_block_start(void *owner, fiber::http::Http2HpackDecoder::Sink &sink) noexcept {
        auto *self = static_cast<TestHttp2StreamOwner *>(owner);
        if (self->reading_trailers || self->trailers_complete) {
            return fiber::common::IoErr::Invalid;
        }
        if (self->headers_received) {
            self->reading_trailers = true;
        }
        sink.ctx = owner;
        sink.ops = &decoder_ops();
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_header_block_complete(void *owner, bool end_stream) noexcept {
        auto *self = static_cast<TestHttp2StreamOwner *>(owner);
        if (!self->headers_received) {
            self->headers_received = true;
            if (end_stream) {
                self->trailers_complete = true;
            }
            return fiber::common::IoErr::None;
        }
        if (!self->reading_trailers || !end_stream) {
            return fiber::common::IoErr::Invalid;
        }
        self->trailers_complete = true;
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_body(void *owner, fiber::mem::IoBuf &&, bool end_stream) noexcept {
        auto *self = static_cast<TestHttp2StreamOwner *>(owner);
        if (!self->headers_received || self->reading_trailers || self->trailers_complete) {
            return fiber::common::IoErr::Invalid;
        }
        if (end_stream) {
            self->trailers_complete = true;
        }
        return fiber::common::IoErr::None;
    }
    static void on_abort(void *, fiber::common::IoErr) noexcept {}
    static fiber::common::IoErr on_indexed_field(void *, fiber::http::Http2HpackDecoder::TableEntryView) noexcept {
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_indexed_name(void *owner, std::string_view name, std::uint64_t name_hash) noexcept {
        auto *self = static_cast<TestHttp2StreamOwner *>(owner);
        self->pending_name_storage.assign(name.data(), name.size());
        self->pending_name_hash = name_hash;
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_name_raw(void *owner, const std::uint8_t *data, std::size_t len) noexcept {
        auto *self = static_cast<TestHttp2StreamOwner *>(owner);
        self->pending_name_storage.assign(reinterpret_cast<const char *>(data), len);
        self->pending_name_hash = fiber::http::http_header_name_hash(self->pending_name_storage);
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_name_huffman(void *owner, const std::uint8_t *data, std::size_t len) noexcept {
        auto *self = static_cast<TestHttp2StreamOwner *>(owner);
        bool ok = false;
        const std::size_t decoded_len = fiber::http::http2_huffman_decoded_length(data, len, &ok);
        if (!ok) {
            return fiber::common::IoErr::Invalid;
        }
        self->pending_name_storage.assign(decoded_len, '\0');
        fiber::http::Http2HuffmanDecodeState state;
        fiber::http::Http2HuffmanDecodeResult result = fiber::http::http2_huffman_decode(
            state, data, len, reinterpret_cast<std::uint8_t *>(self->pending_name_storage.data()), decoded_len, true);
        if (result.code != fiber::http::Http2HuffmanCode::Ok || result.written != decoded_len) {
            return fiber::common::IoErr::Invalid;
        }
        self->pending_name_hash = fiber::http::http_header_name_hash(self->pending_name_storage);
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_value_raw(void *owner, const std::uint8_t *data, std::size_t len,
                                             fiber::http::Http2HpackDecoder::FieldView *out) noexcept {
        auto *self = static_cast<TestHttp2StreamOwner *>(owner);
        self->pending_value_storage.assign(reinterpret_cast<const char *>(data), len);
        if (out != nullptr) {
            out->name = self->pending_name_storage;
            out->name_hash = self->pending_name_hash;
            out->value = self->pending_value_storage;
        }
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_value_huffman(void *owner, const std::uint8_t *data, std::size_t len,
                                                 fiber::http::Http2HpackDecoder::FieldView *out) noexcept {
        auto *self = static_cast<TestHttp2StreamOwner *>(owner);
        bool ok = false;
        const std::size_t decoded_len = fiber::http::http2_huffman_decoded_length(data, len, &ok);
        if (!ok) {
            return fiber::common::IoErr::Invalid;
        }
        self->pending_value_storage.assign(decoded_len, '\0');
        fiber::http::Http2HuffmanDecodeState state;
        fiber::http::Http2HuffmanDecodeResult result = fiber::http::http2_huffman_decode(
            state, data, len, reinterpret_cast<std::uint8_t *>(self->pending_value_storage.data()), decoded_len, true);
        if (result.code != fiber::http::Http2HuffmanCode::Ok || result.written != decoded_len) {
            return fiber::common::IoErr::Invalid;
        }
        if (out != nullptr) {
            out->name = self->pending_name_storage;
            out->name_hash = self->pending_name_hash;
            out->value = self->pending_value_storage;
        }
        return fiber::common::IoErr::None;
    }
    static const fiber::http::Http2HpackDecoder::Ops &decoder_ops() noexcept {
        static const fiber::http::Http2HpackDecoder::Ops kOps{
            &TestHttp2StreamOwner::on_indexed_field,
            &TestHttp2StreamOwner::on_indexed_name,
            &TestHttp2StreamOwner::on_name_raw,
            &TestHttp2StreamOwner::on_name_huffman,
            &TestHttp2StreamOwner::on_value_raw,
            &TestHttp2StreamOwner::on_value_huffman,
        };
        return kOps;
    }

    fiber::http::Http2Stream stream;
    std::string pending_name_storage;
    std::string pending_value_storage;
    std::uint64_t pending_name_hash = 0;
    bool headers_received = false;
    bool reading_trailers = false;
    bool trailers_complete = false;
};

class TestHttp2StreamFactory {
public:
    [[nodiscard]] static const fiber::http::Http2StreamFactoryOps &ops() noexcept {
        static const fiber::http::Http2StreamFactoryOps kOps{
                &TestHttp2StreamFactory::create_local_stream_op,
                &TestHttp2StreamFactory::create_peer_stream_op,
        };
        return kOps;
    }

    [[nodiscard]] fiber::http::Http2Stream::Lease create_local_stream(std::uint32_t stream_id,
                                                                      fiber::http::Http2Connection &) noexcept {
        return TestHttp2StreamOwner::create(stream_id);
    }

    [[nodiscard]] fiber::http::Http2Stream::Lease create_peer_stream(std::uint32_t stream_id,
                                                                     fiber::http::Http2Connection &) noexcept {
        return TestHttp2StreamOwner::create(stream_id);
    }

private:
    static fiber::http::Http2Stream::Lease create_local_stream_op(void *ctx, std::uint32_t stream_id,
                                                                  fiber::http::Http2Connection &conn) noexcept {
        return static_cast<TestHttp2StreamFactory *>(ctx)->create_local_stream(stream_id, conn);
    }

    static fiber::http::Http2Stream::Lease create_peer_stream_op(void *ctx, std::uint32_t stream_id,
                                                                 fiber::http::Http2Connection &conn) noexcept {
        return static_cast<TestHttp2StreamFactory *>(ctx)->create_peer_stream(stream_id, conn);
    }
};

inline TestHttp2StreamFactory &test_http2_stream_factory() noexcept {
    static TestHttp2StreamFactory factory;
    return factory;
}

} // namespace

#endif // FIBER_TESTS_HTTP2_TEST_SUPPORT_H
