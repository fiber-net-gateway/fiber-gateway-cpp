#include "ServerHttp2Request.h"

#include <new>

#include "Http2Connection.h"

namespace fiber::http {

ServerHttp2Request::ServerHttp2Request(std::uint32_t stream_id, Http2Connection &conn,
                                       const HttpServerOptions &http_options) noexcept :
    conn_(&conn), stream_(stream_id, this, &ServerHttp2Request::destroy_owner), exchange_(http_options) {}

Http2Stream::Lease ServerHttp2Request::create(std::uint32_t stream_id, Http2Connection &conn,
                                              const HttpServerOptions &http_options) noexcept {
    auto *owner = new (std::nothrow) ServerHttp2Request(stream_id, conn, http_options);
    if (!owner) {
        return {};
    }
    return Http2Stream::Lease::adopt(&owner->stream_);
}

void ServerHttp2Request::destroy_owner(void *owner) noexcept { delete static_cast<ServerHttp2Request *>(owner); }

} // namespace fiber::http
