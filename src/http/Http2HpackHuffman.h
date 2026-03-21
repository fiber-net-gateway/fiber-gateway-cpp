#ifndef FIBER_HTTP_HTTP2_HPACK_HUFFMAN_H
#define FIBER_HTTP_HTTP2_HPACK_HUFFMAN_H

#include <cstddef>
#include <cstdint>

namespace fiber::http {

enum class Http2HuffmanLowerMode : std::uint8_t {
    None = 0,
    Ascii = 1,
};

enum class Http2HuffmanCode : std::uint8_t {
    Ok = 0,
    OutputFull,
    NeedMore,
    InvalidEncoding,
};

struct Http2HuffmanEncodeResult {
    Http2HuffmanCode code = Http2HuffmanCode::Ok;
    std::size_t written = 0;
};

struct Http2HuffmanDecodeResult {
    Http2HuffmanCode code = Http2HuffmanCode::Ok;
    std::size_t consumed = 0;
    std::size_t written = 0;
};

struct Http2HuffmanDecodeState {
    std::uint8_t state = 0;
    bool ending = true;

    void reset() noexcept {
        state = 0;
        ending = true;
    }
};

[[nodiscard]] std::size_t http2_huffman_encoded_length(
    const std::uint8_t *src,
    std::size_t len,
    Http2HuffmanLowerMode lower_mode = Http2HuffmanLowerMode::None) noexcept;

[[nodiscard]] std::size_t http2_huffman_decoded_length(
    const std::uint8_t *src,
    std::size_t len,
    bool *ok = nullptr) noexcept;

[[nodiscard]] Http2HuffmanEncodeResult http2_huffman_encode(
    const std::uint8_t *src,
    std::size_t len,
    std::uint8_t *dst,
    std::size_t dst_cap,
    Http2HuffmanLowerMode lower_mode = Http2HuffmanLowerMode::None) noexcept;

[[nodiscard]] std::size_t http2_huffman_encode_exact(
    const std::uint8_t *src,
    std::size_t len,
    std::uint8_t *dst,
    Http2HuffmanLowerMode lower_mode = Http2HuffmanLowerMode::None) noexcept;

[[nodiscard]] Http2HuffmanDecodeResult http2_huffman_decode(
    Http2HuffmanDecodeState &state,
    const std::uint8_t *src,
    std::size_t len,
    std::uint8_t *dst,
    std::size_t dst_cap,
    bool last) noexcept;

[[nodiscard]] Http2HuffmanDecodeResult http2_huffman_decode_exact(
    Http2HuffmanDecodeState &state,
    const std::uint8_t *src,
    std::size_t len,
    std::uint8_t *dst,
    bool last) noexcept;

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_HPACK_HUFFMAN_H
