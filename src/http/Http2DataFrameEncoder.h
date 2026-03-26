#ifndef FIBER_HTTP_HTTP2_DATA_FRAME_ENCODER_H
#define FIBER_HTTP_HTTP2_DATA_FRAME_ENCODER_H

#include <cstddef>
#include <cstdint>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/IoBuf.h"

namespace fiber::http {

class Http2OutboundEncodeTarget;

class Http2DataFrameEncoder : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        std::uint32_t stream_id = 0;
        std::uint32_t max_frame_size = 16384;
        bool end_stream = false;
    };

    explicit Http2DataFrameEncoder(Options options) noexcept : options_(options) {}

    [[nodiscard]] common::IoErr encode(Http2OutboundEncodeTarget &target, mem::IoBufChain &payload,
                                       std::size_t payload_bytes) noexcept;

private:
    [[nodiscard]] common::IoErr validate_options() const noexcept;
    [[nodiscard]] common::IoErr append_frame(Http2OutboundEncodeTarget &target, mem::IoBufChain &payload,
                                             std::uint32_t payload_bytes, bool end_stream) noexcept;

    Options options_{};
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_DATA_FRAME_ENCODER_H
