#ifndef FIBER_HTTP_CLIENT_HTTP2_EXCHANGE_H
#define FIBER_HTTP_CLIENT_HTTP2_EXCHANGE_H

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/BufPool.h"
#include "ClientHttp2Types.h"
#include "Http2Stream.h"
#include "HttpCommon.h"
#include "HttpExchange.h"
#include "HttpExchangeIo.h"
#include "HttpHeaders.h"

namespace fiber::http {

class Http2ClientConnection;
class Http2LocalStreamGate;
class ClientHttp2Request;

class ClientHttp2Exchange : public common::NonCopyable, public common::NonMovable {
public:
    ClientHttp2Exchange() noexcept = default;
    // Opening a stream goes through the connection's admission gate, so an
    // exchange is bound to that gate rather than to the connection itself.
    ClientHttp2Exchange(Http2LocalStreamGate &gate, mem::BufPool &pool) noexcept;
    ClientHttp2Exchange(Http2ClientConnection &conn, mem::BufPool &pool) noexcept;
    ClientHttp2Exchange(Http2Stream::Lease stream, mem::BufPool &pool) noexcept;
    ClientHttp2Exchange(ClientHttp2Exchange &&other) noexcept;
    ClientHttp2Exchange &operator=(ClientHttp2Exchange &&other) noexcept;
    ~ClientHttp2Exchange() = default;

    fiber::async::Task<common::IoResult<void>>
    send_request_header(const Http2RequestHead &head, bool end_stream,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    // write_all accepts the complete payload before returning. write returns
    // after the first flow-controlled DATA batch and consumes an IoBufChain in
    // place; retry the remaining suffix with the same end_stream value. A
    // timeout or terminal send error resets the stream and rejects later writes.
    fiber::async::Task<common::IoResult<size_t>>
    write_all(mem::IoBufChain chunk, std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    fiber::async::Task<common::IoResult<size_t>>
    write_all(const std::uint8_t *buf, std::size_t len, bool end_stream,
              std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    fiber::async::Task<common::IoResult<size_t>>
    write(mem::IoBufChain &chunk, std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    fiber::async::Task<common::IoResult<size_t>>
    write(const std::uint8_t *buf, std::size_t len, bool end_stream,
          std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    fiber::async::Task<common::IoResult<void>>
    write_trailer(const HttpHeaders &headers,
                  std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;

    fiber::async::Task<common::IoResult<const Http2ResponseHead *>>
    read_header(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    fiber::async::Task<common::IoResult<mem::IoBufChain>>
    read_body(std::size_t max_bytes = 64 * 1024,
              std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;

    common::IoResult<void> abort(common::IoErr reason = common::IoErr::Canceled) noexcept;
    void cancel(common::IoErr reason = common::IoErr::Canceled) noexcept;

    [[nodiscard]] bool valid() const noexcept { return gate_ != nullptr || static_cast<bool>(stream_); }
    [[nodiscard]] Http2ExtendedConnectSupport extended_connect_support() const noexcept;
    [[nodiscard]] std::uint32_t stream_id() const noexcept { return stream_ ? stream_->stream_id() : 0; }
    [[nodiscard]] Http2Stream *stream() noexcept { return stream_.get(); }
    [[nodiscard]] const Http2Stream *stream() const noexcept { return stream_.get(); }

private:
    [[nodiscard]] fiber::async::Task<common::IoResult<ClientHttp2Request *>>
    ensure_request_opened(std::chrono::steady_clock::time_point deadline) noexcept;
    [[nodiscard]] ClientHttp2Request *request() noexcept;
    [[nodiscard]] const ClientHttp2Request *request() const noexcept;

    Http2LocalStreamGate *gate_ = nullptr;
    mem::BufPool *pool_ = nullptr;
    Http2Stream::Lease stream_{};
};

} // namespace fiber::http

#endif // FIBER_HTTP_CLIENT_HTTP2_EXCHANGE_H
