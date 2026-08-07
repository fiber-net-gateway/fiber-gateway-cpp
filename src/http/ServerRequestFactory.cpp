#include <fiber/http/ServerRequestFactory.h>

#include <fiber/common/Assert.h>
#include <fiber/http/Http2Connection.h>
#include <fiber/http/ServerHttp2Push.h>
#include <fiber/http/ServerHttp2Request.h>

namespace fiber::http {

const Http2StreamFactoryOps &ServerRequestFactory::ops() noexcept {
    static const Http2StreamFactoryOps kOps{
            &ServerRequestFactory::create_peer_stream_op,
    };
    return kOps;
}

Http2Stream::Lease ServerRequestFactory::create_peer_stream(std::uint32_t stream_id, Http2Connection &conn) noexcept {
    FIBER_ASSERT(http_options_ != nullptr);
    FIBER_ASSERT(handler_ != nullptr);
    return ServerHttp2Request::create(stream_id, conn, *http_options_, *handler_);
}

Http2Stream::Lease ServerRequestFactory::create_peer_stream_op(void *ctx, std::uint32_t stream_id,
                                                               Http2Connection &conn) noexcept {
    return static_cast<ServerRequestFactory *>(ctx)->create_peer_stream(stream_id, conn);
}

} // namespace fiber::http
