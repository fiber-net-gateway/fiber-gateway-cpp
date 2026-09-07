#ifndef FIBER_HTTP_HTTP2_HPACK_DISCARD_SINK_H
#define FIBER_HTTP_HTTP2_HPACK_DISCARD_SINK_H

#include <string_view>

#include <fiber/common/mem/IoBuf.h>
#include <fiber/http/Http2HpackDecoder.h>

namespace fiber::http {

// Keeps only the current field; the connection decoder owns the dynamic table.
class Http2HpackDiscardSink {
public:
    explicit Http2HpackDiscardSink(std::size_t max_string_size) noexcept : max_string_size_(max_string_size) {}
    static const Http2HpackDecoder::Ops &ops() noexcept;
    void finish_block() noexcept;

private:
    static common::IoErr indexed_field(void *, Http2HpackDecoder::TableEntryView) noexcept;
    static common::IoErr indexed_name(void *, Http2HpackDecoder::TableEntryView) noexcept;
    static common::IoErr name_raw(void *, const std::uint8_t *, std::size_t) noexcept;
    static common::IoErr name_huffman(void *, const std::uint8_t *, std::size_t) noexcept;
    static common::IoErr value_raw(void *, const std::uint8_t *, std::size_t, Http2HpackDecoder::FieldView *) noexcept;
    static common::IoErr value_huffman(void *, const std::uint8_t *, std::size_t,
                                       Http2HpackDecoder::FieldView *) noexcept;
    // Smallest backing block, so an empty name/value still owns storage.
    static constexpr std::size_t kMinBufferSize = 64;

    static std::string_view view_of(const mem::IoBuf &buffer) noexcept;
    common::IoErr materialize(mem::IoBuf &buffer, const std::uint8_t *data, std::size_t len, bool huffman) noexcept;
    common::IoErr value(const std::uint8_t *data, std::size_t len, bool huffman,
                        Http2HpackDecoder::FieldView *out) noexcept;
    mem::IoBuf name_;
    mem::IoBuf value_;
    std::size_t max_string_size_;
};

} // namespace fiber::http
#endif
