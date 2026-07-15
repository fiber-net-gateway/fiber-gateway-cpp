#ifndef FIBER_HTTP_HTTP1_EXCHANGE_IO_H
#define FIBER_HTTP_HTTP1_EXCHANGE_IO_H

#include <chrono>
#include <string>

#include "../common/mem/IoBufChain.h"
#include "Http1Parser.h"
#include "HttpExchangeIo.h"

namespace fiber::http {

class HttpExchange;
class Http1Connection;

enum class ResponsePhase : std::uint8_t {
    Init,
    HeaderSent,
    BodyStreaming,
    Finished,
};

class Http1ExchangeIo final : public HttpExchangeIo {
public:
    Http1ExchangeIo(Http1Connection &connection, const HttpExchange &exchange);

    fiber::async::Task<common::IoResult<mem::IoBufChain>>
    read_body(HttpExchange &exchange, size_t max_bytes, std::chrono::milliseconds timeout) noexcept override;
    fiber::async::Task<common::IoResult<void>> send_header(HttpExchange &exchange,
                                                           const OutgoingHeaderBlockView &header,
                                                           std::chrono::milliseconds timeout) override;
    fiber::async::Task<common::IoResult<size_t>> write_body(HttpExchange &exchange, mem::IoBufChain chunk,
                                                            std::chrono::milliseconds timeout) noexcept override;
    fiber::async::Task<common::IoResult<size_t>> write_body(HttpExchange &exchange, const uint8_t *buf, size_t len,
                                                            bool end,
                                                            std::chrono::milliseconds timeout) noexcept override;
    common::IoResult<void> abort(HttpExchange &exchange, common::IoErr reason) noexcept override;

    [[nodiscard]] bool request_body_complete() const noexcept { return body_parser_.done(); }
    [[nodiscard]] bool response_complete() const noexcept { return response_phase_ == ResponsePhase::Finished; }
    [[nodiscard]] bool raw_stream_active() const noexcept {
        return response_phase_ != ResponsePhase::Init && response_body_spec_.is_stream();
    }
    [[nodiscard]] bool should_keep_alive(const HttpExchange &exchange) const noexcept;

private:
    fiber::async::Task<common::IoResult<size_t>> read_more(std::size_t max_bytes,
                                                           std::chrono::milliseconds timeout) noexcept;
    fiber::async::Task<common::IoResult<ParseCode>> advance_chunked_body(std::size_t max_bytes, bool allow_read,
                                                                         std::chrono::milliseconds timeout) noexcept;
    fiber::async::Task<common::IoResult<void>> read_request_trailers(HttpExchange &exchange,
                                                                     std::chrono::milliseconds timeout) noexcept;
    fiber::async::Task<common::IoResult<void>> write_chunked_trailer_block(const HttpHeaders *headers,
                                                                           std::chrono::milliseconds timeout) noexcept;
    fiber::async::Task<common::IoResult<void>> write_informational_header(HttpExchange &exchange, int status_code,
                                                                          const HttpHeaders *headers,
                                                                          std::chrono::milliseconds timeout) noexcept;
    common::IoErr prepare_final_header(const HttpExchange &exchange, const OutgoingHeaderBlockView &header) noexcept;
    common::IoResult<void> normalize_response_plan(bool body_end, std::size_t first_body_len, bool infer_body_mode,
                                                   HttpBodySpec &body_spec) const noexcept;
    [[nodiscard]] bool compute_close_conn(const HttpExchange &exchange) const noexcept;
    common::IoResult<mem::IoBuf> build_response_header(HttpExchange &exchange, bool body_end,
                                                       std::size_t first_body_len, bool infer_body_mode,
                                                       HttpBodySpec &body_spec, bool &close_conn) noexcept;
    common::IoResult<mem::IoBuf> build_informational_header(const HttpExchange &exchange, int status_code,
                                                            const HttpHeaders *headers) const noexcept;
    common::IoResult<mem::IoBuf> build_chunked_trailer_block(const HttpHeaders *headers,
                                                             bool include_final_chunk) const noexcept;
    fiber::async::Task<common::IoResult<void>> write_response_header(HttpExchange &exchange, bool body_end,
                                                                     std::size_t first_body_len, bool infer_body_mode,
                                                                     std::chrono::milliseconds timeout) noexcept;
    common::IoResult<void> ensure_read_buf_writable(std::size_t min_writable) noexcept;
    std::size_t drain_body_input(mem::IoBuf &buffer) noexcept;
    common::IoResult<void> take_prefix(mem::IoBufChain &out, std::size_t len) noexcept;
    common::IoResult<void> spill_read_buf_to_inbound() noexcept;
    [[nodiscard]] mem::IoBuf *front_body_input() noexcept;
    [[nodiscard]] std::size_t body_input_readable() const noexcept;

    Http1Connection *connection_ = nullptr;
    BodyParser body_parser_;
    mem::IoBuf read_buf_;
    bool read_call_used_io_ = false;

    ResponsePhase response_phase_ = ResponsePhase::Init;
    size_t response_body_sent_ = 0;
    bool close_after_response_ = false;
    int response_status_code_ = 0;
    std::string_view response_reason_;
    const HttpHeaders *response_headers_ = nullptr;
    HttpBodySpec response_body_spec_{};
    ResponseConnectionMode response_connection_mode_ = ResponseConnectionMode::Auto;
    size_t response_content_length_ = 0;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_EXCHANGE_IO_H
