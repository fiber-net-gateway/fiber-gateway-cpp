#ifndef FIBER_HTTP_CLIENT_HTTP1_EXCHANGE_H
#define FIBER_HTTP_CLIENT_HTTP1_EXCHANGE_H

#include <cstddef>
#include <cstdint>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/BufPool.h"
#include "ClientHttp1Types.h"
#include "HttpExchange.h"

namespace fiber::http {

class Http1ClientConnection;

class ClientHttp1Exchange : public common::NonCopyable, public common::NonMovable {
public:
    explicit ClientHttp1Exchange(Http1ClientConnection &conn,
                                 Http1ClientExchangeOptions options = {}) noexcept;
    ~ClientHttp1Exchange();

    fiber::async::Task<common::IoResult<void>> send_header(const Http1RequestHead &head, bool end_stream) noexcept;
    fiber::async::Task<common::IoResult<std::size_t>> write_body(BodyChunk chunk) noexcept;
    fiber::async::Task<common::IoResult<std::size_t>> write_body(const std::uint8_t *buf, std::size_t len,
                                                                 bool end_stream) noexcept;
    fiber::async::Task<common::IoResult<void>> send_trailer(const HttpHeaders &trailers) noexcept;

    fiber::async::Task<common::IoResult<const Http1ResponseHead *>> read_header() noexcept;
    fiber::async::Task<common::IoResult<BodyChunk>> read_body(std::size_t max_bytes = 64 * 1024) noexcept;
    fiber::async::Task<common::IoResult<void>> discard_response_body() noexcept;

    [[nodiscard]] const HttpHeaders &response_trailers() const noexcept { return response_trailers_; }
    [[nodiscard]] const Http1ClientExchangeOptions &options() const noexcept { return options_; }
    [[nodiscard]] bool valid() const noexcept { return active_; }
    [[nodiscard]] bool request_complete() const noexcept { return request_state_ == RequestState::RequestDone; }
    [[nodiscard]] bool response_complete() const noexcept { return response_complete_; }
    [[nodiscard]] bool done() const noexcept { return request_complete() && response_complete_; }

private:
    enum class RequestState : std::uint8_t {
        Init,
        SendingBody,
        RequestDone,
        Failed,
    };

    Http1ClientConnection *conn_ = nullptr;
    Http1ClientExchangeOptions options_{};
    mem::BufPool pool_{};
    Http1ResponseHead response_head_;
    HttpHeaders response_trailers_;
    bool active_ = false;
    bool response_complete_ = false;
    RequestState request_state_ = RequestState::Init;
    Http1RequestBodyMode body_mode_ = Http1RequestBodyMode::None;
    std::size_t content_length_ = 0;
    std::size_t body_sent_ = 0;
};

} // namespace fiber::http

#endif // FIBER_HTTP_CLIENT_HTTP1_EXCHANGE_H
