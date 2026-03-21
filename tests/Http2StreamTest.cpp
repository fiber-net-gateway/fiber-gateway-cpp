#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "http/Http2HpackHuffman.h"
#include "http/HttpHeaderHash.h"
#include "http/Http2Stream.h"
#include "Http2TestSupport.h"

namespace {

struct OwnedStreamHolder {
    explicit OwnedStreamHolder(std::uint32_t stream_id, bool *destroyed) :
        destroyed_flag(destroyed), stream(stream_id, this, ops()) {}

    static const fiber::http::Http2Stream::Ops &ops() noexcept {
        static const fiber::http::Http2Stream::Ops kOps{
            &OwnedStreamHolder::destroy_owner,
            &OwnedStreamHolder::on_header_block_start,
            &OwnedStreamHolder::on_header_block_complete,
            &OwnedStreamHolder::on_body,
        };
        return kOps;
    }
    static void destroy_owner(void *owner) noexcept { delete static_cast<OwnedStreamHolder *>(owner); }
    static fiber::common::IoErr on_header_block_start(void *owner, fiber::http::Http2HpackDecoder::Sink &sink) noexcept {
        sink.ctx = owner;
        sink.ops = &decoder_ops();
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_header_block_complete(void *, bool) noexcept {
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_body(void *, fiber::mem::IoBuf &&, bool) noexcept {
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_indexed_field(void *, fiber::http::Http2HpackDecoder::TableEntryView) noexcept {
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_indexed_name(void *owner, std::string_view name, std::uint64_t name_hash) noexcept {
        auto *self = static_cast<OwnedStreamHolder *>(owner);
        self->pending_name_storage.assign(name.data(), name.size());
        self->pending_name_hash = name_hash;
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_name_raw(void *owner, const std::uint8_t *data, std::size_t len) noexcept {
        auto *self = static_cast<OwnedStreamHolder *>(owner);
        self->pending_name_storage.assign(reinterpret_cast<const char *>(data), len);
        self->pending_name_hash = fiber::http::http_header_name_hash(self->pending_name_storage);
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_name_huffman(void *owner, const std::uint8_t *data, std::size_t len) noexcept {
        auto *self = static_cast<OwnedStreamHolder *>(owner);
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
        auto *self = static_cast<OwnedStreamHolder *>(owner);
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
        auto *self = static_cast<OwnedStreamHolder *>(owner);
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
            &OwnedStreamHolder::on_indexed_field,
            &OwnedStreamHolder::on_indexed_name,
            &OwnedStreamHolder::on_name_raw,
            &OwnedStreamHolder::on_name_huffman,
            &OwnedStreamHolder::on_value_raw,
            &OwnedStreamHolder::on_value_huffman,
        };
        return kOps;
    }

    ~OwnedStreamHolder() {
        if (destroyed_flag) {
            *destroyed_flag = true;
        }
    }

    bool *destroyed_flag = nullptr;
    fiber::http::Http2Stream stream;
    std::string pending_name_storage;
    std::string pending_value_storage;
    std::uint64_t pending_name_hash = 0;
};

} // namespace

TEST(Http2StreamTest, OwnerBackedCreateReturnsUsableLease) {
    fiber::http::Http2Stream::Lease stream = TestHttp2StreamOwner::create(1);
    ASSERT_TRUE(stream);

    stream->close(fiber::common::IoErr::Canceled);
    stream.reset();
}

TEST(Http2StreamTest, EmbeddedOwnerIsDestroyedWhenAdoptedLeaseReleasesClosedStream) {
    bool destroyed = false;
    auto *owner = new OwnedStreamHolder(3, &destroyed);
    fiber::http::Http2Stream::Lease stream = fiber::http::Http2Stream::Lease::adopt(&owner->stream);
    ASSERT_TRUE(stream);

    stream->close(fiber::common::IoErr::Canceled);
    EXPECT_FALSE(destroyed);

    stream.reset();
    EXPECT_TRUE(destroyed);
}

TEST(Http2StreamTest, AdditionalLeaseRetainsEmbeddedOwnerUntilLastReferenceDrops) {
    bool destroyed = false;
    auto *owner = new OwnedStreamHolder(5, &destroyed);
    fiber::http::Http2Stream::Lease initial = fiber::http::Http2Stream::Lease::adopt(&owner->stream);
    fiber::http::Http2Stream::Lease extra = owner->stream.lease();
    ASSERT_TRUE(initial);
    ASSERT_TRUE(extra);

    owner->stream.close(fiber::common::IoErr::Canceled);

    initial.reset();
    EXPECT_FALSE(destroyed);

    extra.reset();
    EXPECT_TRUE(destroyed);
}
