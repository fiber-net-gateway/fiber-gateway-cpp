#ifndef FIBER_HTTP_HTTP2_STREAM_FACTORY_H
#define FIBER_HTTP_HTTP2_STREAM_FACTORY_H

#include <cstdint>

#include "Http2Stream.h"

namespace fiber::http {

class Http2Connection;

struct Http2StreamFactoryOps {
    Http2Stream::Lease (*create_local_stream)(void *ctx, std::uint32_t stream_id, Http2Connection &conn) noexcept;
    Http2Stream::Lease (*create_peer_stream)(void *ctx, std::uint32_t stream_id, Http2Connection &conn) noexcept;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_STREAM_FACTORY_H
