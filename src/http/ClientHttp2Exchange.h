#ifndef FIBER_HTTP_CLIENT_HTTP2_EXCHANGE_H
#define FIBER_HTTP_CLIENT_HTTP2_EXCHANGE_H

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

class Http2Connection;
class Http2ClientConnection;
class ClientHttp2Request;

class ClientHttp2Exchange : public common::NonCopyable, public common::NonMovable {
public:
    ClientHttp2Exchange() noexcept = default;
    ClientHttp2Exchange(Http2Connection &conn, mem::BufPool &pool) noexcept;
    ClientHttp2Exchange(Http2ClientConnection &conn, mem::BufPool &pool) noexcept;
    ClientHttp2Exchange(Http2Stream::Lease stream, mem::BufPool &pool) noexcept;
    ClientHttp2Exchange(ClientHttp2Exchange &&other) noexcept;
    ClientHttp2Exchange &operator=(ClientHttp2Exchange &&other) noexcept;
    ~ClientHttp2Exchange() = default;

    fiber::async::Task<common::IoResult<void>> send_request_header(const Http2RequestHead &head,
                                                                   bool end_stream) noexcept;
    fiber::async::Task<common::IoResult<size_t>> write_body(BodyChunk chunk) noexcept;
    fiber::async::Task<common::IoResult<size_t>> write_body(const std::uint8_t *buf, std::size_t len,
                                                            bool end_stream) noexcept;
    fiber::async::Task<common::IoResult<void>> write_trailer(const HttpHeaders &headers) noexcept;

    fiber::async::Task<common::IoResult<const Http2ResponseHead *>> read_header() noexcept;
    fiber::async::Task<common::IoResult<BodyChunk>> read_body(std::size_t max_bytes = 64 * 1024) noexcept;

    void cancel(common::IoErr reason = common::IoErr::Canceled) noexcept;

    [[nodiscard]] bool valid() const noexcept { return conn_ != nullptr || static_cast<bool>(stream_); }
    [[nodiscard]] std::uint32_t stream_id() const noexcept { return stream_ ? stream_->stream_id() : 0; }
    [[nodiscard]] Http2Stream *stream() noexcept { return stream_.get(); }
    [[nodiscard]] const Http2Stream *stream() const noexcept { return stream_.get(); }

private:
    [[nodiscard]] common::IoResult<ClientHttp2Request *> ensure_request_opened() noexcept;
    [[nodiscard]] ClientHttp2Request *request() noexcept;
    [[nodiscard]] const ClientHttp2Request *request() const noexcept;

    Http2Connection *conn_ = nullptr;
    mem::BufPool *pool_ = nullptr;
    Http2Stream::Lease stream_{};
};

} // namespace fiber::http

#endif // FIBER_HTTP_CLIENT_HTTP2_EXCHANGE_H
