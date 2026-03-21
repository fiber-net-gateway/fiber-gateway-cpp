#include "ServerHttp2Push.h"

#include <new>

#include "Http2Connection.h"

namespace fiber::http {

namespace {

common::IoErr noop_header_block_start(void *, Http2HpackDecoder::Sink &sink) noexcept {
    static const Http2HpackDecoder::Ops kDecoderOps{};
    sink.ctx = nullptr;
    sink.ops = &kDecoderOps;
    return common::IoErr::None;
}

common::IoErr noop_header_block_complete(void *, bool) noexcept { return common::IoErr::None; }

common::IoErr noop_body(void *, mem::IoBuf &&, bool) noexcept { return common::IoErr::None; }

} // namespace

const Http2Stream::Ops &ServerHttp2Push::stream_ops() noexcept {
    static const Http2Stream::Ops kOps{
        &ServerHttp2Push::destroy_owner,
        &noop_header_block_start,
        &noop_header_block_complete,
        &noop_body,
    };
    return kOps;
}

ServerHttp2Push::ServerHttp2Push(std::uint32_t stream_id, Http2Connection &conn) noexcept :
    conn_(&conn), stream_(stream_id, this, stream_ops()) {}

Http2Stream::Lease ServerHttp2Push::create(std::uint32_t stream_id, Http2Connection &conn) noexcept {
    auto *owner = new (std::nothrow) ServerHttp2Push(stream_id, conn);
    if (!owner) {
        return {};
    }
    return Http2Stream::Lease::adopt(&owner->stream_);
}

void ServerHttp2Push::destroy_owner(void *owner) noexcept { delete static_cast<ServerHttp2Push *>(owner); }

} // namespace fiber::http
