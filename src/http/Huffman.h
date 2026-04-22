#ifndef FIBER_HTTP_HUFFMAN_H
#define FIBER_HTTP_HUFFMAN_H

#include <cstddef>
#include <cstdint>

namespace fiber::http {

enum class HpackHuffmanLowerMode : std::uint8_t {
    None = 0,
    Ascii = 1,
};

enum class HpackHuffmanCode : std::uint8_t {
    Ok = 0,
    OutputFull,
    NeedMore,
    InvalidEncoding,
};

struct HpackHuffmanEncodeResult {
    HpackHuffmanCode code = HpackHuffmanCode::Ok;
    std::size_t consumed = 0;
    std::size_t written = 0;
};

struct HpackHuffmanDecodeResult {
    HpackHuffmanCode code = HpackHuffmanCode::Ok;
    std::size_t consumed = 0;
    std::size_t written = 0;
};

struct HpackHuffmanDecodeState {
    std::uint16_t state = 0;
    bool ending = true;

    void reset() noexcept {
        state = 0;
        ending = true;
    }
};

struct HpackHuffmanEncodeState {
    std::uint64_t bit_buffer = 0;
    std::uint8_t pending_bits = 0;
    bool finalizing = false;

    void reset() noexcept {
        bit_buffer = 0;
        pending_bits = 0;
        finalizing = false;
    }
};

[[nodiscard]] std::size_t
hpack_huffman_encoded_length(const std::uint8_t *src, std::size_t len,
                             HpackHuffmanLowerMode lower_mode = HpackHuffmanLowerMode::None) noexcept;

[[nodiscard]] std::size_t hpack_huffman_decoded_length(const std::uint8_t *src, std::size_t len,
                                                       bool *ok = nullptr) noexcept;

[[nodiscard]] HpackHuffmanEncodeResult
hpack_huffman_encode(const std::uint8_t *src, std::size_t len, std::uint8_t *dst, std::size_t dst_cap,
                     HpackHuffmanLowerMode lower_mode = HpackHuffmanLowerMode::None) noexcept;

[[nodiscard]] HpackHuffmanEncodeResult
hpack_huffman_encode_incremental(HpackHuffmanEncodeState &state, const std::uint8_t *src, std::size_t len,
                                 std::uint8_t *dst, std::size_t dst_cap, bool last,
                                 HpackHuffmanLowerMode lower_mode = HpackHuffmanLowerMode::None) noexcept;

[[nodiscard]] std::size_t
hpack_huffman_encode_exact(const std::uint8_t *src, std::size_t len, std::uint8_t *dst,
                           HpackHuffmanLowerMode lower_mode = HpackHuffmanLowerMode::None) noexcept;

[[nodiscard]] HpackHuffmanDecodeResult hpack_huffman_decode(HpackHuffmanDecodeState &state, const std::uint8_t *src,
                                                            std::size_t len, std::uint8_t *dst, std::size_t dst_cap,
                                                            bool last) noexcept;

[[nodiscard]] HpackHuffmanDecodeResult hpack_huffman_decode_exact(HpackHuffmanDecodeState &state,
                                                                  const std::uint8_t *src, std::size_t len,
                                                                  std::uint8_t *dst, bool last) noexcept;

} // namespace fiber::http

#endif // FIBER_HTTP_HUFFMAN_H
