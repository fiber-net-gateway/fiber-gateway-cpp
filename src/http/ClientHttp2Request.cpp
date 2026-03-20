#include "ClientHttp2Request.h"

#include <new>

#include "Http2Connection.h"

namespace fiber::http {

ClientHttp2Request::ClientHttp2Request(std::uint32_t stream_id, Http2Connection &conn) noexcept :
    conn_(&conn), stream_(stream_id, this, &ClientHttp2Request::destroy_owner) {}

Http2Stream::Lease ClientHttp2Request::create(std::uint32_t stream_id, Http2Connection &conn) noexcept {
    auto *owner = new (std::nothrow) ClientHttp2Request(stream_id, conn);
    if (!owner) {
        return {};
    }
    return Http2Stream::Lease::adopt(&owner->stream_);
}

void ClientHttp2Request::destroy_owner(void *owner) noexcept { delete static_cast<ClientHttp2Request *>(owner); }

} // namespace fiber::http
