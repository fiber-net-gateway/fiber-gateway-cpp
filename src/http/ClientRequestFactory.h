#ifndef FIBER_HTTP_CLIENT_REQUEST_FACTORY_H
#define FIBER_HTTP_CLIENT_REQUEST_FACTORY_H

#include <cstdint>

#include "Http2StreamFactory.h"

namespace fiber::http {

class Http2Connection;

class ClientRequestFactory {
public:
    [[nodiscard]] static const Http2StreamFactoryOps &ops() noexcept;

    [[nodiscard]] Http2Stream::Lease create_local_stream(std::uint32_t stream_id, Http2Connection &conn) noexcept;
    [[nodiscard]] Http2Stream::Lease create_peer_stream(std::uint32_t stream_id, Http2Connection &conn) noexcept;

private:
    static Http2Stream::Lease create_local_stream_op(void *ctx, std::uint32_t stream_id,
                                                     Http2Connection &conn) noexcept;
    static Http2Stream::Lease create_peer_stream_op(void *ctx, std::uint32_t stream_id,
                                                    Http2Connection &conn) noexcept;
};

} // namespace fiber::http

#endif // FIBER_HTTP_CLIENT_REQUEST_FACTORY_H
