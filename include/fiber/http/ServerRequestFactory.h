#ifndef FIBER_HTTP_SERVER_REQUEST_FACTORY_H
#define FIBER_HTTP_SERVER_REQUEST_FACTORY_H

#include <cstdint>
#include <memory>

#include "Http2StreamFactory.h"
#include "HttpExchange.h"

namespace fiber::http {

class Http2Connection;

class ServerRequestFactory {
public:
    // The factory owns the request configuration so protocol connections may
    // safely outlive the HttpServer facade during asynchronous shutdown.
    ServerRequestFactory(const HttpServerOptions &http_options, const HttpHandler &handler) :
        http_options_(http_options), handler_(std::make_shared<HttpHandler>(handler)) {}

    [[nodiscard]] static const Http2StreamFactoryOps &ops() noexcept;
    [[nodiscard]] Http2Stream::Lease create_peer_stream(std::uint32_t stream_id, Http2Connection &conn) noexcept;

private:
    static Http2Stream::Lease create_peer_stream_op(void *ctx, std::uint32_t stream_id, Http2Connection &conn) noexcept;

    HttpServerOptions http_options_{};
    std::shared_ptr<const HttpHandler> handler_{};
};

} // namespace fiber::http

#endif // FIBER_HTTP_SERVER_REQUEST_FACTORY_H
