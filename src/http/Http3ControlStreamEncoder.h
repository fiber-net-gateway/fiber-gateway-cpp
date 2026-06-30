#ifndef FIBER_HTTP_HTTP3_CONTROL_STREAM_ENCODER_H
#define FIBER_HTTP_HTTP3_CONTROL_STREAM_ENCODER_H

#include "../common/IoError.h"
#include "../common/mem/IoBufChain.h"
#include "Http3Protocol.h"

namespace fiber::http {

[[nodiscard]] common::IoResult<mem::IoBufChain>
encode_http3_control_stream_preface(const Http3Settings &settings, mem::IoBufNodePool &node_pool) noexcept;

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP3_CONTROL_STREAM_ENCODER_H
