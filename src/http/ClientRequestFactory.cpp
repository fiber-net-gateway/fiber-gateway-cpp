#include "ClientRequestFactory.h"

#include "ClientHttp2Push.h"
#include "ClientHttp2Request.h"
#include "Http2Connection.h"

namespace fiber::http {

const Http2StreamFactoryOps &ClientRequestFactory::ops() noexcept {
    static const Http2StreamFactoryOps kOps{
            &ClientRequestFactory::create_local_stream_op,
            &ClientRequestFactory::create_peer_stream_op,
    };
    return kOps;
}

Http2Stream::Lease ClientRequestFactory::create_local_stream(std::uint32_t stream_id, Http2Connection &conn) noexcept {
    return ClientHttp2Request::create(stream_id, conn);
}

Http2Stream::Lease ClientRequestFactory::create_peer_stream(std::uint32_t stream_id, Http2Connection &conn) noexcept {
    return ClientHttp2Push::create(stream_id, conn);
}

Http2Stream::Lease ClientRequestFactory::create_local_stream_op(void *ctx, std::uint32_t stream_id,
                                                                Http2Connection &conn) noexcept {
    return static_cast<ClientRequestFactory *>(ctx)->create_local_stream(stream_id, conn);
}

Http2Stream::Lease ClientRequestFactory::create_peer_stream_op(void *ctx, std::uint32_t stream_id,
                                                               Http2Connection &conn) noexcept {
    return static_cast<ClientRequestFactory *>(ctx)->create_peer_stream(stream_id, conn);
}

} // namespace fiber::http
