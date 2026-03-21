#include "ClientHttp2Request.h"

#include <new>

#include "Http2Connection.h"

namespace fiber::http {

namespace {

common::IoErr noop_header_block_start(void *, Http2HpackDecoder::Sink &sink) noexcept {
    sink.ctx = nullptr;
    static const Http2HpackDecoder::Ops kDecoderOps{};
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
