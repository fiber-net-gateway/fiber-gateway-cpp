#include "ServerHttp2Push.h"

#include <new>

#include "Http2Connection.h"

namespace fiber::http {

ServerHttp2Push::ServerHttp2Push(std::uint32_t stream_id, Http2Connection &conn) noexcept :
    conn_(&conn), stream_(stream_id, this, &ServerHttp2Push::destroy_owner) {}

Http2Stream::Lease ServerHttp2Push::create(std::uint32_t stream_id, Http2Connection &conn) noexcept {
    auto *owner = new (std::nothrow) ServerHttp2Push(stream_id, conn);
    if (!owner) {
        return {};
    }
    return Http2Stream::Lease::adopt(&owner->stream_);
}

void ServerHttp2Push::destroy_owner(void *owner) noexcept { delete static_cast<ServerHttp2Push *>(owner); }

} // namespace fiber::http
