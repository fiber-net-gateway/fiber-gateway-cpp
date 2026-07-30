#ifndef FIBER_HTTP_HTTP_EXCHANGE_IO_H
#define FIBER_HTTP_HTTP_EXCHANGE_IO_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/mem/IoBufChain.h"
#include "HttpBodySpec.h"

namespace fiber::http {

class HttpExchange;
class HttpHeaders;

enum class OutgoingHeaderKind : std::uint8_t {
    Informational,
    Final,
    Trailer,
};

enum class ResponseConnectionMode : std::uint8_t {
    Auto,
    Close,
};

struct OutgoingHeaderBlockView {
    OutgoingHeaderKind kind = OutgoingHeaderKind::Final;
    int status_code = 0;
    std::string_view reason;
    const HttpHeaders *headers = nullptr;
    HttpBodySpec body{};
    ResponseConnectionMode connection_mode = ResponseConnectionMode::Auto;
    bool end_stream = false;
};

class HttpExchangeIo {
public:
    using ResponseChannelClosedCallback = void (*)(void *ctx) noexcept;

    virtual ~HttpExchangeIo() = default;

    // One-shot notification that this exchange can no longer deliver response
    // bytes. Normal completion of the request receive direction is not enough.
    // Registration invokes callback synchronously when already closed. Clear
    // removes only the matching callback/context pair.
    [[nodiscard]] virtual bool response_channel_closed() const noexcept = 0;
    virtual common::IoErr set_response_channel_closed_callback(ResponseChannelClosedCallback callback,
                                                               void *ctx) noexcept = 0;
    virtual common::IoErr clear_response_channel_closed_callback(ResponseChannelClosedCallback callback,
                                                                 void *ctx) noexcept = 0;

    virtual fiber::async::Task<common::IoResult<mem::IoBufChain>>
    read_body(HttpExchange &exchange, size_t max_bytes, std::chrono::milliseconds timeout) noexcept = 0;
    virtual fiber::async::Task<common::IoResult<void>>
    send_header(HttpExchange &exchange, const OutgoingHeaderBlockView &header, std::chrono::milliseconds timeout) = 0;
    virtual fiber::async::Task<common::IoResult<size_t>> write_all(HttpExchange &exchange, mem::IoBufChain chunk,
                                                                   std::chrono::milliseconds timeout) noexcept = 0;
    virtual fiber::async::Task<common::IoResult<size_t>> write_all(HttpExchange &exchange, const uint8_t *buf,
                                                                   size_t len, bool end,
                                                                   std::chrono::milliseconds timeout) noexcept = 0;
    virtual fiber::async::Task<common::IoResult<size_t>> write(HttpExchange &exchange, mem::IoBufChain &chunk,
                                                               std::chrono::milliseconds timeout) noexcept = 0;
    virtual fiber::async::Task<common::IoResult<size_t>> write(HttpExchange &exchange, const uint8_t *buf, size_t len,
                                                               bool end,
                                                               std::chrono::milliseconds timeout) noexcept = 0;
    virtual common::IoResult<void> abort(HttpExchange &exchange,
                                         common::IoErr reason = common::IoErr::Canceled) noexcept = 0;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP_EXCHANGE_IO_H
