#ifndef FIBER_HTTP_SERVER_HTTP3_REQUEST_H
#define FIBER_HTTP_SERVER_HTTP3_REQUEST_H

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "../async/Spawn.h"
#include "../async/Task.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../quic/QuicConnection.h"
#include "Http3Codec.h"
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

    fiber::async::Task<common::IoResult<mem::IoBufChain>>
    read_body(HttpExchange &exchange, std::size_t max_bytes, std::chrono::milliseconds timeout) noexcept override;
    fiber::async::Task<common::IoResult<void>> send_header(HttpExchange &exchange,
                                                           const OutgoingHeaderBlockView &header,
                                                           std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<std::size_t>> write_body(HttpExchange &exchange, mem::IoBufChain chunk,
                                                                 std::chrono::milliseconds timeout) noexcept override;
    fiber::async::Task<common::IoResult<std::size_t>> write_body(HttpExchange &exchange, const std::uint8_t *buf,
                                                                 std::size_t len, bool end,
                                                                 std::chrono::milliseconds timeout) noexcept override;
    common::IoResult<void> abort(HttpExchange &exchange, common::IoErr reason) noexcept override;

private:
    enum class HeaderBlockTarget : std::uint8_t;
    enum class BodyRecvState : std::uint8_t;
    class HeaderBlockParser;

    ServerHttp3Request(Http3Connection &conn, const HttpServerOptions &http_options,
                       const HttpHandler &handler) noexcept;

    static void destroy_owner(void *owner, quic::QuicStream &stream) noexcept;

    async::DetachedTask run_read_loop(quic::QuicStream::Lease lease) noexcept;
    async::Task<common::IoResult<void>> parse_request_header() noexcept;
    async::Task<common::IoResult<void>> parse_header_block(HeaderBlockTarget target,
                                                           std::uint64_t payload_length) noexcept;
    async::Task<common::IoResult<void>> skip_frame_payload(std::uint64_t payload_length,
                                                           std::chrono::milliseconds timeout) noexcept;
    async::Task<common::IoResult<void>> read_more_input(std::chrono::milliseconds timeout) noexcept;
    [[nodiscard]] Http3ParseStatus parse_frame_header_once() noexcept;
    [[nodiscard]] common::IoErr begin_body_frame(const Http3FrameHeader &header) noexcept;
    [[nodiscard]] common::IoErr fail_request(Http3ErrorCode error,
                                             common::IoErr reason = common::IoErr::Invalid) noexcept;
    [[nodiscard]] common::IoResult<mem::IoBufChain>
    fail_read_body(Http3ErrorCode error, common::IoErr reason = common::IoErr::Invalid) noexcept;
    [[nodiscard]] common::IoResult<void> take_body_payload(mem::IoBufChain &out, std::size_t bytes) noexcept;

    quic::QuicConnection::Lease quic_lease_{};
    quic::QuicStream stream_;
    mem::IoBufChain inbound_buf_;
    HttpExchange exchange_;
    const HttpHandler *handler_ = nullptr;
    std::uint32_t max_qpack_string_size_ = 0;
    std::chrono::milliseconds body_timeout_{};
    Http3FrameHeaderParser frame_parser_;
    Http3FrameHeader current_frame_{};
    std::uint64_t frame_payload_remaining_ = 0;
    Http3ErrorCode request_parse_error_ = Http3ErrorCode::GeneralProtocolError;
    BodyRecvState body_recv_state_{};
    HttpBodySpec response_body_spec_{HttpBodySpec::Auto()};
    std::size_t response_content_length_ = 0;
    std::size_t response_body_sent_ = 0;
    bool read_loop_started_ = false;
    bool request_head_received_ = false;
    bool handler_started_ = false;
    bool handler_done_ = false;
    bool frame_header_in_progress_ = false;
    bool response_headers_sent_ = false;
    bool response_finished_ = false;
    bool extended_connect_enabled_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_SERVER_HTTP3_REQUEST_H
