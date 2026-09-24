#ifndef FIBER_HTTP_HTTP3_CONTROL_STREAM_ENCODER_H
#define FIBER_HTTP_HTTP3_CONTROL_STREAM_ENCODER_H

#include <fiber/common/IoError.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/http/Http3Protocol.h>

namespace fiber::http {

[[nodiscard]] common::IoResult<mem::IoBufChain>
encode_http3_control_stream_preface(const Http3Settings &settings) noexcept;

[[nodiscard]] common::IoResult<mem::IoBufChain> encode_http3_goaway_frame(std::uint64_t id) noexcept;

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP3_CONTROL_STREAM_ENCODER_H
