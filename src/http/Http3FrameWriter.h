#ifndef FIBER_HTTP_HTTP3_FRAME_WRITER_H
#define FIBER_HTTP_HTTP3_FRAME_WRITER_H

#include <cstddef>
#include <cstdint>

#include "../common/IoError.h"
#include "../common/mem/IoBufChain.h"

namespace fiber::http {

class Http3QpackEncoderIoBufWriter;

inline constexpr std::size_t kHttp3FrameHeaderReserve = 16;
inline constexpr std::uint64_t kMaxHttp3FramePayloadLength = (1ULL << 62U) - 1U;

[[nodiscard]] common::IoResult<void> http3_finish_headers_frame(Http3QpackEncoderIoBufWriter &writer,
                                                                mem::IoBufChain &frame, bool end_stream) noexcept;

[[nodiscard]] common::IoResult<void> http3_prepare_data_frame(mem::IoBufChain &chunk,
                                                              mem::IoBufNodePool &target_pool) noexcept;

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP3_FRAME_WRITER_H
