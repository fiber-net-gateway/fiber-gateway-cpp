#ifndef FIBER_HTTP_CLIENT_HTTP2_EXCHANGE_H
#define FIBER_HTTP_CLIENT_HTTP2_EXCHANGE_H

#include <cstddef>
#include <cstdint>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "HttpExchange.h"
#include "HttpExchangeIo.h"
#include "HttpHeaders.h"
#include "Http2Stream.h"

namespace fiber::http {

class ClientHttp2Request;

struct Http2ResponseHead {
    OutgoingHeaderKind kind = OutgoingHeaderKind::Final;
    int status_code = 0;
    bool end_stream = false;
    HttpHeaders headers;

    explicit Http2ResponseHead(mem::BufPool &pool) : headers(pool) {}
};

class ClientHttp2Exchange : public common::NonCopyable, public common::NonMovable {
public:
    ClientHttp2Exchange() noexcept = default;
    explicit ClientHttp2Exchange(Http2Stream::Lease stream) noexcept;
    ClientHttp2Exchange(ClientHttp2Exchange &&other) noexcept = default;
    ClientHttp2Exchange &operator=(ClientHttp2Exchange &&other) noexcept = default;
    ~ClientHttp2Exchange() = default;

    fiber::async::Task<common::IoResult<size_t>> write_body(BodyChunk chunk) noexcept;
    fiber::async::Task<common::IoResult<size_t>> write_body(const std::uint8_t *buf, std::size_t len,
                                                            bool end_stream) noexcept;
    fiber::async::Task<common::IoResult<void>> write_trailer(const HttpHeaders &headers) noexcept;

    fiber::async::Task<common::IoResult<Http2ResponseHead>> read_header(mem::BufPool &pool) noexcept;
    fiber::async::Task<common::IoResult<BodyChunk>> read_body(std::size_t max_bytes = 64 * 1024) noexcept;

    void cancel(common::IoErr reason = common::IoErr::Canceled) noexcept;

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(stream_); }
    [[nodiscard]] std::uint32_t stream_id() const noexcept { return stream_ ? stream_->stream_id() : 0; }
    [[nodiscard]] Http2Stream *stream() noexcept { return stream_.get(); }
    [[nodiscard]] const Http2Stream *stream() const noexcept { return stream_.get(); }

private:
    [[nodiscard]] ClientHttp2Request *request() noexcept;
    [[nodiscard]] const ClientHttp2Request *request() const noexcept;

    Http2Stream::Lease stream_{};
};

} // namespace fiber::http

#endif // FIBER_HTTP_CLIENT_HTTP2_EXCHANGE_H
