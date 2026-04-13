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

class ResponseBodySpec {
public:
    enum class Kind : std::uint8_t {
        Auto,
        None,
        ContentLength,
        Chunked,
    };

    constexpr ResponseBodySpec() noexcept = default;

    [[nodiscard]] static constexpr ResponseBodySpec Auto() noexcept { return ResponseBodySpec(Kind::Auto, 0); }
    [[nodiscard]] static constexpr ResponseBodySpec None() noexcept { return ResponseBodySpec(Kind::None, 0); }
    [[nodiscard]] static constexpr ResponseBodySpec ContentLength(std::size_t length) noexcept {
        return ResponseBodySpec(Kind::ContentLength, length);
    }
    [[nodiscard]] static constexpr ResponseBodySpec Chunked() noexcept { return ResponseBodySpec(Kind::Chunked, 0); }

    [[nodiscard]] constexpr Kind kind() const noexcept { return kind_; }
    [[nodiscard]] constexpr bool is_auto() const noexcept { return kind_ == Kind::Auto; }
    [[nodiscard]] constexpr bool is_none() const noexcept { return kind_ == Kind::None; }
    [[nodiscard]] constexpr bool is_content_length() const noexcept { return kind_ == Kind::ContentLength; }
    [[nodiscard]] constexpr bool is_chunked() const noexcept { return kind_ == Kind::Chunked; }
    [[nodiscard]] constexpr std::size_t content_length() const noexcept { return content_length_; }

private:
    constexpr ResponseBodySpec(Kind kind, std::size_t content_length) noexcept :
        kind_(kind),
        content_length_(content_length) {}

    Kind kind_ = Kind::Auto;
    std::size_t content_length_ = 0;
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
    ResponseBodySpec body{};
    ResponseConnectionMode connection_mode = ResponseConnectionMode::Auto;
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
