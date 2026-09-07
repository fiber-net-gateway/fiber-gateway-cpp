#include "http/Http2HpackDiscardSink.h"

#include <algorithm>
#include <cstring>

#include <fiber/http/HttpHeaderHash.h>
#include "http/Huffman.h"

namespace fiber::http {

const Http2HpackDecoder::Ops &Http2HpackDiscardSink::ops() noexcept {
    static const Http2HpackDecoder::Ops ops{&indexed_field, &indexed_name, &name_raw,
                                            &name_huffman,  &value_raw,    &value_huffman};
    return ops;
}

common::IoErr Http2HpackDiscardSink::materialize(mem::IoBuf &buffer, const std::uint8_t *data, std::size_t len,
                                                 bool huffman) noexcept {
    bool valid = true;
    const std::size_t size = huffman ? hpack_huffman_decoded_length(data, len, &valid) : len;
    if (!valid || size > max_string_size_) {
        return common::IoErr::Invalid;
    }
    // An empty name or value is legal HPACK, so the buffer must be backed even
    // when nothing is written into it: FieldView needs a real pointer, and
    // clear() requires storage. Always keep at least the minimum block.
    if (!buffer || buffer.capacity() < size) {
        auto replacement = mem::IoBuf::allocate(std::max(size, kMinBufferSize));
        if (!replacement) {
            return common::IoErr::NoMem;
        }
        buffer = std::move(replacement);
    }
    buffer.clear();
    if (size) {
        if (huffman) {
            HpackHuffmanDecodeState state;
            auto result = hpack_huffman_decode_exact(state, data, len, buffer.writable_data(), true);
            if (result.code != HpackHuffmanCode::Ok || result.written != size) {
                return common::IoErr::Invalid;
            }
        } else {
            std::memcpy(buffer.writable_data(), data, size);
        }
        buffer.commit(size);
    }
    return common::IoErr::None;
}

common::IoErr Http2HpackDiscardSink::indexed_field(void *, Http2HpackDecoder::TableEntryView) noexcept {
    return common::IoErr::None;
}
common::IoErr Http2HpackDiscardSink::indexed_name(void *ctx, Http2HpackDecoder::TableEntryView entry) noexcept {
    return name_raw(ctx, reinterpret_cast<const std::uint8_t *>(entry.name.data()), entry.name.size());
}
common::IoErr Http2HpackDiscardSink::name_raw(void *ctx, const std::uint8_t *data, std::size_t len) noexcept {
    auto &self = *static_cast<Http2HpackDiscardSink *>(ctx);
    return self.materialize(self.name_, data, len, false);
}
common::IoErr Http2HpackDiscardSink::name_huffman(void *ctx, const std::uint8_t *data, std::size_t len) noexcept {
    auto &self = *static_cast<Http2HpackDiscardSink *>(ctx);
    return self.materialize(self.name_, data, len, true);
}
common::IoErr Http2HpackDiscardSink::value(const std::uint8_t *data, std::size_t len, bool huffman,
                                           Http2HpackDecoder::FieldView *out) noexcept {
    const auto err = materialize(value_, data, len, huffman);
    if (err == common::IoErr::None && out) {
        out->name = view_of(name_);
        out->name_hash = http_header_name_hash(out->name);
        out->value = view_of(value_);
    }
    return err;
}
std::string_view Http2HpackDiscardSink::view_of(const mem::IoBuf &buffer) noexcept {
    // materialize() always backs the buffer, so readable_data() stays non-null
    // for the empty string: the decoder rejects a FieldView that pairs a null
    // pointer with a non-zero size, and an empty view is accepted either way.
    if (!buffer) {
        return {};
    }
    return std::string_view(reinterpret_cast<const char *>(buffer.readable_data()), buffer.readable());
}
common::IoErr Http2HpackDiscardSink::value_raw(void *ctx, const std::uint8_t *data, std::size_t len,
                                               Http2HpackDecoder::FieldView *out) noexcept {
    return static_cast<Http2HpackDiscardSink *>(ctx)->value(data, len, false, out);
}
common::IoErr Http2HpackDiscardSink::value_huffman(void *ctx, const std::uint8_t *data, std::size_t len,
                                                   Http2HpackDecoder::FieldView *out) noexcept {
    return static_cast<Http2HpackDiscardSink *>(ctx)->value(data, len, true, out);
}
void Http2HpackDiscardSink::finish_block() noexcept {
    if (name_.capacity() > 4096)
        name_.reset();
    if (value_.capacity() > 4096)
        value_.reset();
}

} // namespace fiber::http
