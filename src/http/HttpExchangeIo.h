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
    virtual ~HttpExchangeIo() = default;

    virtual fiber::async::Task<common::IoResult<mem::IoBufChain>>
    read_body(HttpExchange &exchange, size_t max_bytes, std::chrono::milliseconds timeout) noexcept = 0;
    virtual fiber::async::Task<common::IoResult<void>>
    send_header(HttpExchange &exchange, const OutgoingHeaderBlockView &header, std::chrono::milliseconds timeout) = 0;
    virtual fiber::async::Task<common::IoResult<size_t>> write_body(HttpExchange &exchange, mem::IoBufChain chunk,
                                                                    std::chrono::milliseconds timeout) noexcept = 0;
    virtual fiber::async::Task<common::IoResult<size_t>> write_body(HttpExchange &exchange, const uint8_t *buf,
                                                                    size_t len, bool end,
                                                                    std::chrono::milliseconds timeout) noexcept = 0;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP_EXCHANGE_IO_H
