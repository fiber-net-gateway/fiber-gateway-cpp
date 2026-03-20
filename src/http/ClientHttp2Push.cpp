#include "ClientHttp2Push.h"

#include <new>

#include "Http2Connection.h"

namespace fiber::http {

ClientHttp2Push::ClientHttp2Push(std::uint32_t stream_id, Http2Connection &conn) noexcept :
    conn_(&conn), stream_(stream_id, this, &ClientHttp2Push::destroy_owner) {}

Http2Stream::Lease ClientHttp2Push::create(std::uint32_t stream_id, Http2Connection &conn) noexcept {
    auto *owner = new (std::nothrow) ClientHttp2Push(stream_id, conn);
    if (!owner) {
        return {};
    }
    return Http2Stream::Lease::adopt(&owner->stream_);
}

void ClientHttp2Push::destroy_owner(void *owner) noexcept { delete static_cast<ClientHttp2Push *>(owner); }

} // namespace fiber::http
