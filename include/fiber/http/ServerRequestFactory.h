#ifndef FIBER_HTTP_SERVER_REQUEST_FACTORY_H
#define FIBER_HTTP_SERVER_REQUEST_FACTORY_H

#include <cstdint>
#include <memory>

#include "Http2Connection.h"
#include "HttpExchange.h"

namespace fiber::http {

class Http2Connection;

class ServerRequestFactory {
public:
    // The factory owns the handler so protocol connections may safely outlive
    // the endpoint facade during asynchronous shutdown. ServerHttp2Request
    // needs nothing else from the server configuration.
    explicit ServerRequestFactory(const HttpHandler &handler) : handler_(std::make_shared<HttpHandler>(handler)) {}
    explicit ServerRequestFactory(std::shared_ptr<const HttpHandler> handler) : handler_(std::move(handler)) {}

    [[nodiscard]] static const Http2Connection::Ops &ops() noexcept;
    [[nodiscard]] Http2Stream::Lease create_peer_stream(std::uint32_t stream_id, Http2Connection &conn) noexcept;

private:
    static Http2Stream::Lease create_peer_stream_op(void *ctx, std::uint32_t stream_id, Http2Connection &conn) noexcept;

    std::shared_ptr<const HttpHandler> handler_{};
};

} // namespace fiber::http

#endif // FIBER_HTTP_SERVER_REQUEST_FACTORY_H
