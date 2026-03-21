#include "ServerRequestFactory.h"

#include "../common/Assert.h"
#include "Http2Connection.h"
#include "ServerHttp2Push.h"
#include "ServerHttp2Request.h"

namespace fiber::http {

const Http2StreamFactoryOps &ServerRequestFactory::ops() noexcept {
    static const Http2StreamFactoryOps kOps{
            &ServerRequestFactory::create_local_stream_op,
            &ServerRequestFactory::create_peer_stream_op,
    };
    return kOps;
}

Http2Stream::Lease ServerRequestFactory::create_local_stream(std::uint32_t stream_id, Http2Connection &conn) noexcept {
    return ServerHttp2Push::create(stream_id, conn);
}

Http2Stream::Lease ServerRequestFactory::create_peer_stream(std::uint32_t stream_id, Http2Connection &conn) noexcept {
    FIBER_ASSERT(http_options_ != nullptr);
    FIBER_ASSERT(handler_ != nullptr);
    return ServerHttp2Request::create(stream_id, conn, *http_options_, *handler_);
}

Http2Stream::Lease ServerRequestFactory::create_local_stream_op(void *ctx, std::uint32_t stream_id,
                                                                Http2Connection &conn) noexcept {
    return static_cast<ServerRequestFactory *>(ctx)->create_local_stream(stream_id, conn);
}

Http2Stream::Lease ServerRequestFactory::create_peer_stream_op(void *ctx, std::uint32_t stream_id,
                                                               Http2Connection &conn) noexcept {
    return static_cast<ServerRequestFactory *>(ctx)->create_peer_stream(stream_id, conn);
}

} // namespace fiber::http
