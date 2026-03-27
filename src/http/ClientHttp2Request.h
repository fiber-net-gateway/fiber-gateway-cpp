#ifndef FIBER_HTTP_CLIENT_HTTP2_REQUEST_H
#define FIBER_HTTP_CLIENT_HTTP2_REQUEST_H

#include <cstdint>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "Http2StreamFactory.h"
#include "Http2Stream.h"

namespace fiber::http {

class Http2Connection;

class ClientHttp2Request : public common::NonCopyable, public common::NonMovable {
public:
    [[nodiscard]] static const Http2StreamFactoryOps &factory_ops() noexcept;
    [[nodiscard]] static Http2Stream::Lease create(std::uint32_t stream_id, Http2Connection &conn) noexcept;

    [[nodiscard]] Http2Stream &stream() noexcept { return stream_; }
    [[nodiscard]] const Http2Stream &stream() const noexcept { return stream_; }

private:
    static Http2Stream::Lease create_local_stream(std::uint32_t stream_id, Http2Connection &conn) noexcept;
    static Http2Stream::Lease create_peer_stream(std::uint32_t stream_id, Http2Connection &conn) noexcept;
    static Http2Stream::Lease create_local_stream_op(void *ctx, std::uint32_t stream_id,
                                                     Http2Connection &conn) noexcept;
    static Http2Stream::Lease create_peer_stream_op(void *ctx, std::uint32_t stream_id,
                                                    Http2Connection &conn) noexcept;
    explicit ClientHttp2Request(std::uint32_t stream_id, Http2Connection &conn) noexcept;
    static const Http2Stream::Ops &stream_ops() noexcept;
    static void destroy_owner(void *owner) noexcept;

    [[maybe_unused]] Http2Connection *conn_ = nullptr;
    Http2Stream stream_;
};

} // namespace fiber::http

#endif // FIBER_HTTP_CLIENT_HTTP2_REQUEST_H
