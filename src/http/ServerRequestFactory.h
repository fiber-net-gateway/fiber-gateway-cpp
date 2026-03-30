#ifndef FIBER_HTTP_SERVER_REQUEST_FACTORY_H
#define FIBER_HTTP_SERVER_REQUEST_FACTORY_H

#include <cstdint>

#include "HttpExchange.h"
#include "Http2StreamFactory.h"

namespace fiber::http {

class Http2Connection;

class ServerRequestFactory {
public:
    ServerRequestFactory(const HttpServerOptions &http_options, const HttpHandler &handler) noexcept :
        http_options_(&http_options), handler_(&handler) {}

    [[nodiscard]] static const Http2StreamFactoryOps &ops() noexcept;
    [[nodiscard]] Http2Stream::Lease create_peer_stream(std::uint32_t stream_id, Http2Connection &conn) noexcept;

private:
    static Http2Stream::Lease create_peer_stream_op(void *ctx, std::uint32_t stream_id,
                                                    Http2Connection &conn) noexcept;

    const HttpServerOptions *http_options_ = nullptr;
    const HttpHandler *handler_ = nullptr;
};

} // namespace fiber::http

#endif // FIBER_HTTP_SERVER_REQUEST_FACTORY_H
