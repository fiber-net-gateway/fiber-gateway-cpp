#ifndef FIBER_HTTP_CLIENT_HTTP3_EXCHANGE_H
#define FIBER_HTTP_CLIENT_HTTP3_EXCHANGE_H

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/BufPool.h"
#include "../quic/QuicStream.h"
#include "ClientHttp3Types.h"

namespace fiber::http {

class ClientHttp3Request;
class Http3ClientConnection;
class Http3Connection;

class ClientHttp3Exchange : public common::NonCopyable, public common::NonMovable {
public:
    ClientHttp3Exchange() noexcept = default;
    ClientHttp3Exchange(Http3Connection &conn, mem::BufPool &pool) noexcept;
    ClientHttp3Exchange(Http3ClientConnection &conn, mem::BufPool &pool) noexcept;
    ClientHttp3Exchange(ClientHttp3Exchange &&other) noexcept;
    ClientHttp3Exchange &operator=(ClientHttp3Exchange &&other) noexcept;
    ~ClientHttp3Exchange() = default;

    // The buffer pool and parent connection must outlive the exchange. Dropping an
    // unfinished exchange does not cancel it; call abort() when abandoning a request.

    async::Task<common::IoResult<void>>
    send_request_header(const Http3RequestHead &head, bool end_stream,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    async::Task<common::IoResult<std::size_t>>
    write_body(mem::IoBufChain chunk, std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    async::Task<common::IoResult<std::size_t>>
    write_body(const std::uint8_t *buf, std::size_t len, bool end_stream,
               std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    async::Task<common::IoResult<void>>
    write_trailer(const HttpHeaders &headers,
                  std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    async::Task<common::IoResult<const Http3ResponseHead *>>
    read_header(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    async::Task<common::IoResult<mem::IoBufChain>>
    read_body(std::size_t max_bytes = 64 * 1024,
              std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;

    common::IoResult<void> abort(common::IoErr reason = common::IoErr::Canceled) noexcept;
    void cancel(common::IoErr reason = common::IoErr::Canceled) noexcept;

    [[nodiscard]] bool valid() const noexcept { return conn_ != nullptr || static_cast<bool>(stream_); }
    [[nodiscard]] Http3ExtendedConnectSupport extended_connect_support() const noexcept;
    [[nodiscard]] Http3RequestOutcome outcome() const noexcept;
    [[nodiscard]] std::uint64_t stream_id() const noexcept;
    [[nodiscard]] quic::QuicStream *stream() noexcept { return stream_.get(); }
    [[nodiscard]] const quic::QuicStream *stream() const noexcept { return stream_.get(); }

private:
    async::Task<common::IoResult<ClientHttp3Request *>>
    ensure_request_opened(std::chrono::milliseconds timeout) noexcept;
    [[nodiscard]] ClientHttp3Request *request() noexcept;
    [[nodiscard]] const ClientHttp3Request *request() const noexcept;

    Http3Connection *conn_ = nullptr;
    mem::BufPool *pool_ = nullptr;
    quic::QuicStream::Lease stream_{};
};

} // namespace fiber::http

#endif // FIBER_HTTP_CLIENT_HTTP3_EXCHANGE_H
