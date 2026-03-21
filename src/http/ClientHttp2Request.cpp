#include "ClientHttp2Request.h"

#include <new>

#include "Http2Connection.h"

namespace fiber::http {

namespace {

common::IoErr noop_indexed_field(void *, Http2HpackDecoder::TableEntryView) noexcept { return common::IoErr::None; }

common::IoErr noop_indexed_name(void *, std::string_view, std::uint64_t) noexcept { return common::IoErr::None; }

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
        &noop_indexed_field,
        &noop_indexed_name,
        &noop_name_raw,
        &noop_name_huffman,
        &noop_value_raw,
        &noop_value_huffman,
    };
    sink.ctx = nullptr;
    sink.ops = &kDecoderOps;
    return common::IoErr::None;
}

common::IoErr noop_header_block_complete(void *, bool) noexcept { return common::IoErr::None; }

common::IoErr noop_body(void *, mem::IoBuf &&, bool) noexcept { return common::IoErr::None; }

} // namespace

const Http2Stream::Ops &ClientHttp2Request::stream_ops() noexcept {
    static const Http2Stream::Ops kOps{
        &ClientHttp2Request::destroy_owner,
        &noop_header_block_start,
        &noop_header_block_complete,
        &noop_body,
    };
    return kOps;
}

ClientHttp2Request::ClientHttp2Request(std::uint32_t stream_id, Http2Connection &conn) noexcept :
    conn_(&conn), stream_(stream_id, this, stream_ops()) {}

Http2Stream::Lease ClientHttp2Request::create(std::uint32_t stream_id, Http2Connection &conn) noexcept {
    auto *owner = new (std::nothrow) ClientHttp2Request(stream_id, conn);
    if (!owner) {
        return {};
    }
    return Http2Stream::Lease::adopt(&owner->stream_);
}

void ClientHttp2Request::destroy_owner(void *owner) noexcept { delete static_cast<ClientHttp2Request *>(owner); }

} // namespace fiber::http
