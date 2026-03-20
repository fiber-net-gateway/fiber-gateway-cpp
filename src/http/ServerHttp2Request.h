#ifndef FIBER_HTTP_SERVER_HTTP2_REQUEST_H
#define FIBER_HTTP_SERVER_HTTP2_REQUEST_H

#include <cstdint>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "HttpExchange.h"
#include "Http2Stream.h"

namespace fiber::http {

class Http2Connection;

class ServerHttp2Request : public common::NonCopyable, public common::NonMovable {
public:
    [[nodiscard]] static Http2Stream::Lease create(std::uint32_t stream_id, Http2Connection &conn,
                                                   const HttpServerOptions &http_options) noexcept;

    [[nodiscard]] Http2Stream &stream() noexcept { return stream_; }
    [[nodiscard]] const Http2Stream &stream() const noexcept { return stream_; }
    [[nodiscard]] HttpExchange &exchange() noexcept { return exchange_; }
    [[nodiscard]] const HttpExchange &exchange() const noexcept { return exchange_; }

private:
    ServerHttp2Request(std::uint32_t stream_id, Http2Connection &conn, const HttpServerOptions &http_options) noexcept;
    static void destroy_owner(void *owner) noexcept;

    [[maybe_unused]] Http2Connection *conn_ = nullptr;
    Http2Stream stream_;
    HttpExchange exchange_;
};

} // namespace fiber::http

#endif // FIBER_HTTP_SERVER_HTTP2_REQUEST_H
