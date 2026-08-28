#include "http/ClientHttp2Push.h"

#include <new>

#include <fiber/http/Http2Connection.h>

namespace fiber::http {

namespace {

common::IoErr noop_indexed_field(void *, Http2HpackDecoder::TableEntryView) noexcept { return common::IoErr::None; }

common::IoErr noop_indexed_name(void *, Http2HpackDecoder::TableEntryView) noexcept { return common::IoErr::None; }

common::IoErr noop_name_raw(void *, const std::uint8_t *, std::size_t) noexcept { return common::IoErr::None; }

common::IoErr noop_name_huffman(void *, const std::uint8_t *, std::size_t) noexcept { return common::IoErr::None; }

common::IoErr noop_value_raw(void *, const std::uint8_t *, std::size_t, Http2HpackDecoder::FieldView *) noexcept {
    return common::IoErr::None;
}

common::IoErr noop_value_huffman(void *, const std::uint8_t *, std::size_t, Http2HpackDecoder::FieldView *) noexcept {
    return common::IoErr::None;
}

common::IoErr noop_header_block_start(void *, Http2HpackDecoder::Sink &sink) noexcept {
    static const Http2HpackDecoder::Ops kDecoderOps{
            &noop_indexed_field, &noop_indexed_name, &noop_name_raw,
            &noop_name_huffman,  &noop_value_raw,    &noop_value_huffman,
    };
    sink.ctx = nullptr;
    sink.ops = &kDecoderOps;
    return common::IoErr::None;
}

common::IoErr noop_header_block_complete(void *, bool) noexcept { return common::IoErr::None; }

common::IoErr noop_body(void *, mem::IoBuf &&, bool) noexcept { return common::IoErr::None; }

void noop_abort(void *, common::IoErr) noexcept {}

} // namespace

const Http2Stream::Ops &ClientHttp2Push::stream_ops() noexcept {
    static const Http2Stream::Ops kOps{
            &ClientHttp2Push::destroy_owner,
            &noop_header_block_start,
            &noop_header_block_complete,
            &noop_body,
            &noop_abort,
    };
    return kOps;
}

ClientHttp2Push::ClientHttp2Push(Http2Connection &conn) noexcept : conn_(&conn), stream_(this, stream_ops()) {}

Http2Stream::Lease ClientHttp2Push::create(std::uint32_t stream_id, Http2Connection &conn) noexcept {
    (void) stream_id;
    auto *owner = new (std::nothrow) ClientHttp2Push(conn);
    if (!owner) {
        return {};
    }
    return Http2Stream::Lease::adopt(&owner->stream_);
}

void ClientHttp2Push::destroy_owner(void *owner) noexcept { delete static_cast<ClientHttp2Push *>(owner); }

} // namespace fiber::http
