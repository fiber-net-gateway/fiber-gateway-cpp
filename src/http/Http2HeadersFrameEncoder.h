#ifndef FIBER_HTTP_HTTP2_HEADERS_FRAME_ENCODER_H
#define FIBER_HTTP_HTTP2_HEADERS_FRAME_ENCODER_H

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/IoBuf.h"
#include "Http2HpackEncoder.h"

namespace fiber::http {

class Http2HeadersFrameEncoder : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        std::uint32_t stream_id = 0;
        std::uint32_t max_frame_size = 16384;
        std::uint16_t first_frame_payload_cap = 1024;
        bool end_stream = false;
        std::uint8_t pad_length = 0;
        bool has_priority = false;
        bool exclusive = false;
        std::uint32_t stream_dependency = 0;
        std::uint8_t weight = 16;
    };

    Http2HeadersFrameEncoder(Http2HpackEncoder &encoder, Options options) noexcept;
    ~Http2HeadersFrameEncoder();

    [[nodiscard]] common::IoErr begin() noexcept;
    [[nodiscard]] common::IoErr encode_status(int status_code) noexcept;
    [[nodiscard]] common::IoErr encode_field(std::string_view name, std::uint64_t name_hash,
                                             std::string_view value) noexcept;
    [[nodiscard]] common::IoErr finish(mem::IoBufChain &out) noexcept;
    void abort() noexcept;

private:
    static const Http2HpackEncoder::OutputOps kOutputOps;

    [[nodiscard]] common::IoErr open_frame(bool first_frame) noexcept;
    [[nodiscard]] common::IoErr append_payload_buf(std::uint32_t payload_cap,
                                                   bool reserve_frame_header) noexcept;
    [[nodiscard]] common::IoErr seal_current_frame(bool end_headers) noexcept;
    [[nodiscard]] common::IoErr validate_options() const noexcept;
    [[nodiscard]] std::size_t current_hpack_writable() const noexcept;
    [[nodiscard]] std::size_t current_frame_hpack_remaining() const noexcept;
    [[nodiscard]] std::uint32_t first_frame_buf_payload_cap() const noexcept;
    [[nodiscard]] std::uint32_t next_buf_payload_cap() const noexcept;
    void reset_state() noexcept;
    void commit_to_frames(std::size_t bytes) noexcept;

    static common::IoErr acquire_output(void *ctx, std::size_t min_bytes,
                                        std::uint8_t *&dst, std::size_t &len) noexcept;
    static void commit_output(void *ctx, std::size_t written) noexcept;

    Http2HpackEncoder &encoder_;
    Options options_{};

    mem::IoBufChain frames_;
    mem::IoBuf *current_buf_ = nullptr;
    std::uint8_t *current_frame_header_ = nullptr;
    std::uint32_t current_frame_payload_limit_ = 0;
    std::uint32_t current_payload_written_ = 0;
    std::uint32_t current_prefix_len_ = 0;
    std::uint32_t current_suffix_len_ = 0;
    bool current_first_frame_ = false;
    bool begun_ = false;
    bool finished_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_HEADERS_FRAME_ENCODER_H
