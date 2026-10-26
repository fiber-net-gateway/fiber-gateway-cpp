#ifndef FIBER_HTTP_HTTP_BODY_SPEC_H
#define FIBER_HTTP_HTTP_BODY_SPEC_H

#include <cstddef>
#include <cstdint>

namespace fiber::http {

class HttpBodySpec {
public:
    enum class Kind : std::uint8_t {
        Auto, // Outgoing framing is inferred by the active HTTP protocol.
        None,
        ContentLength,
        Chunked, // HTTP/1 chunked transfer coding.
        Stream, // HTTP/2 / HTTP/3 content delimited by the end of the stream; HTTP/1 does not support it yet.
        Raw, // Tunnel bytes, not HTTP content: after an HTTP/1 101, or after a 2xx response to CONNECT.
    };

    constexpr HttpBodySpec() noexcept = default;

    [[nodiscard]] static constexpr HttpBodySpec Auto() noexcept { return {Kind::Auto, 0}; }
    [[nodiscard]] static constexpr HttpBodySpec None() noexcept { return {Kind::None, 0}; }
    [[nodiscard]] static constexpr HttpBodySpec ContentLength(std::size_t length) noexcept {
        return {Kind::ContentLength, length};
    }
    [[nodiscard]] static constexpr HttpBodySpec Chunked() noexcept { return {Kind::Chunked, 0}; }
    [[nodiscard]] static constexpr HttpBodySpec Stream() noexcept { return {Kind::Stream, 0}; }
    [[nodiscard]] static constexpr HttpBodySpec Raw() noexcept { return {Kind::Raw, 0}; }

    [[nodiscard]] constexpr Kind kind() const noexcept { return kind_; }
    [[nodiscard]] constexpr bool is_auto() const noexcept { return kind_ == Kind::Auto; }
    [[nodiscard]] constexpr bool is_none() const noexcept { return kind_ == Kind::None; }
    [[nodiscard]] constexpr bool is_content_length() const noexcept { return kind_ == Kind::ContentLength; }
    [[nodiscard]] constexpr bool is_chunked() const noexcept { return kind_ == Kind::Chunked; }
    [[nodiscard]] constexpr bool is_stream() const noexcept { return kind_ == Kind::Stream; }
    [[nodiscard]] constexpr bool is_raw() const noexcept { return kind_ == Kind::Raw; }
    [[nodiscard]] constexpr std::size_t content_length() const noexcept { return content_length_; }

private:
    constexpr HttpBodySpec(Kind kind, std::size_t content_length) noexcept :
        kind_(kind), content_length_(content_length) {}

    Kind kind_ = Kind::Auto;
    std::size_t content_length_ = 0;
};

using ResponseBodySpec = HttpBodySpec;

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP_BODY_SPEC_H
