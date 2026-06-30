#ifndef FIBER_HTTP_HTTP3_QPACK_ENCODER_H
#define FIBER_HTTP_HTTP3_QPACK_ENCODER_H

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "HttpCommon.h"

namespace fiber::http {

class Http3QpackEncoder : public common::NonCopyable, public common::NonMovable {
public:
    struct OutputOps {
        common::IoErr (*acquire)(void *ctx, std::size_t min_bytes, std::uint8_t *&dst,
                                 std::size_t &len) noexcept = nullptr;
        void (*commit)(void *ctx, std::size_t written) noexcept = nullptr;
    };

    struct Options {
        std::uint32_t max_string_size = 64 * 1024;
        std::size_t huffman_threshold = 16;
    };

    Http3QpackEncoder(void *output_ctx, OutputOps output_ops) noexcept;
    Http3QpackEncoder(void *output_ctx, OutputOps output_ops, Options options) noexcept;

    [[nodiscard]] common::IoErr encode_status(int status_code) noexcept;
    [[nodiscard]] common::IoErr encode_method(HttpMethod method) noexcept;
    [[nodiscard]] common::IoErr encode_scheme(std::string_view scheme) noexcept;
    [[nodiscard]] common::IoErr encode_authority(std::string_view authority) noexcept;
    [[nodiscard]] common::IoErr encode_path(std::string_view path) noexcept;
    [[nodiscard]] common::IoErr encode_field(std::string_view name, std::uint64_t name_hash,
                                             std::string_view value) noexcept;
    [[nodiscard]] common::IoErr finish() noexcept;

private:
    [[nodiscard]] common::IoErr ensure_prefix() noexcept;
    [[nodiscard]] common::IoErr append_indexed(std::uint32_t index) noexcept;
    [[nodiscard]] common::IoErr append_literal_static_name(std::uint32_t name_index, std::string_view value) noexcept;
    [[nodiscard]] common::IoErr append_literal_name(std::string_view name, std::string_view value) noexcept;
    [[nodiscard]] common::IoErr append_string(std::string_view value) noexcept;
    [[nodiscard]] common::IoErr append_literal_name_string(std::string_view name) noexcept;
    [[nodiscard]] common::IoErr append_prefixed_string(std::string_view value, std::uint8_t first_byte_mask,
                                                       std::uint8_t huffman_mask, std::uint8_t prefix_bits) noexcept;
    [[nodiscard]] common::IoErr append_integer(std::uint8_t first_byte_mask, std::uint8_t prefix_bits,
                                               std::uint64_t value) noexcept;
    [[nodiscard]] common::IoErr append_bytes(const std::uint8_t *data, std::size_t len) noexcept;
    [[nodiscard]] common::IoErr append_byte(std::uint8_t byte) noexcept;
    [[nodiscard]] common::IoErr ensure_output(std::size_t min_bytes) noexcept;
    [[nodiscard]] bool should_huffman_encode(std::string_view value) const noexcept;

    const Options options_;
    void *const output_ctx_ = nullptr;
    const OutputOps output_ops_{};
    std::uint8_t *output_dst_ = nullptr;
    std::size_t output_len_ = 0;
    bool prefix_written_ = false;
    bool finished_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP3_QPACK_ENCODER_H
