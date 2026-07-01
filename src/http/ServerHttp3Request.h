#ifndef FIBER_HTTP_SERVER_HTTP3_REQUEST_H
#define FIBER_HTTP_SERVER_HTTP3_REQUEST_H

#include <cstddef>
#include <cstdint>

#include "../async/Spawn.h"
#include "../async/Task.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../quic/QuicConnection.h"
#include "Http3Protocol.h"
#include "HttpExchange.h"
#include "HttpExchangeIo.h"

namespace fiber::event {
class EventLoop;
}

namespace fiber::http {

class Http3Connection;

class ServerHttp3Request final : public HttpExchangeIo, public common::NonCopyable, public common::NonMovable {
public:
    [[nodiscard]] static quic::QuicStream::Lease create(std::uint64_t stream_id, Http3Connection &conn,
                                                        const HttpServerOptions &http_options,
                                                        const HttpHandler &handler) noexcept;

    [[nodiscard]] static ServerHttp3Request *from_stream(quic::QuicStream &stream) noexcept;
    [[nodiscard]] static const ServerHttp3Request *from_stream(const quic::QuicStream &stream) noexcept;

    [[nodiscard]] quic::QuicStream &stream() noexcept { return stream_; }
    [[nodiscard]] const quic::QuicStream &stream() const noexcept { return stream_; }
    [[nodiscard]] HttpExchange &exchange() noexcept { return exchange_; }
    [[nodiscard]] const HttpExchange &exchange() const noexcept { return exchange_; }

    void start_read_loop(event::EventLoop &loop) noexcept;

    fiber::async::Task<common::IoResult<mem::IoBufChain>> read_body(HttpExchange &exchange,
                                                                    std::size_t max_bytes) noexcept override;
    fiber::async::Task<common::IoResult<void>> send_header(HttpExchange &exchange,
                                                           const OutgoingHeaderBlockView &header) override;
    fiber::async::Task<common::IoResult<std::size_t>> write_body(HttpExchange &exchange,
                                                                 mem::IoBufChain chunk) noexcept override;
    fiber::async::Task<common::IoResult<std::size_t>> write_body(HttpExchange &exchange, const std::uint8_t *buf,
                                                                 std::size_t len, bool end) noexcept override;

private:
    class RequestHeaderParser;

    ServerHttp3Request(Http3Connection &conn, const HttpServerOptions &http_options,
                       const HttpHandler &handler) noexcept;

    static void destroy_owner(void *owner, quic::QuicStream &stream) noexcept;

    async::DetachedTask run_read_loop(quic::QuicStream::Lease lease) noexcept;
    async::Task<common::IoResult<void>> parse_request_header() noexcept;

    quic::QuicConnection::Lease quic_lease_{};
    quic::QuicStream stream_;
    mem::IoBufChain inbound_buf_;
    HttpExchange exchange_;
    const HttpHandler *handler_ = nullptr;
    std::uint32_t max_qpack_string_size_ = 0;
    Http3ErrorCode header_parse_error_ = Http3ErrorCode::GeneralProtocolError;
    bool read_loop_started_ = false;
    bool request_head_received_ = false;
    bool handler_started_ = false;
    bool handler_done_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_SERVER_HTTP3_REQUEST_H
