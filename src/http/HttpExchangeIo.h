#ifndef FIBER_HTTP_HTTP_EXCHANGE_IO_H
#define FIBER_HTTP_HTTP_EXCHANGE_IO_H

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../async/Task.h"
#include "../common/IoError.h"

namespace fiber::http {

class HttpExchange;
class HttpHeaders;
struct BodyChunk;

enum class OutgoingHeaderKind : std::uint8_t {
    Informational,
    Final,
    Trailer,
};

enum class ResponseBodyMode : std::uint8_t {
    Auto,
    ContentLength,
    Chunked,
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
    ResponseBodyMode body_mode = ResponseBodyMode::Auto;
    ResponseConnectionMode connection_mode = ResponseConnectionMode::Auto;
    std::size_t content_length = 0;
    bool end_stream = false;
};

class HttpExchangeIo {
public:
    virtual ~HttpExchangeIo() = default;

    virtual fiber::async::Task<common::IoResult<BodyChunk>> read_body(HttpExchange &exchange,
                                                                      size_t max_bytes) noexcept = 0;
    virtual fiber::async::Task<common::IoResult<void>> send_header(HttpExchange &exchange,
                                                                   const OutgoingHeaderBlockView &header) = 0;
    virtual fiber::async::Task<common::IoResult<size_t>> write_body(HttpExchange &exchange,
                                                                    BodyChunk chunk) noexcept = 0;
    virtual fiber::async::Task<common::IoResult<size_t>> write_body(HttpExchange &exchange, const uint8_t *buf,
                                                                    size_t len, bool end) noexcept = 0;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP_EXCHANGE_IO_H
