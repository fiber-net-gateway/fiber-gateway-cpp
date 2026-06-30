#ifndef FIBER_HTTP_HTTP3_QPACK_CONTROL_STREAM_DECODER_H
#define FIBER_HTTP_HTTP3_QPACK_CONTROL_STREAM_DECODER_H

#include <cstdint>

#include "Http3Codec.h"

namespace fiber::http {

class Http3QpackEncoderStreamDecoder {
public:
    [[nodiscard]] Http3ParseStatus parse(mem::IoBufChain &in) noexcept;
    [[nodiscard]] Http3ParseError error() const noexcept { return error_; }
    void reset() noexcept;

private:
    enum class State : std::uint8_t {
        Instruction,
        Capacity,
        Error,
    };

    [[nodiscard]] Http3ParseStatus fail(common::IoErr io_error = common::IoErr::Invalid) noexcept;

    Http3QpackPrefixIntParser prefix_parser_{};
    Http3ParseError error_{.h3_error = Http3ErrorCode::QpackEncoderStreamError};
    State state_ = State::Instruction;
};

enum class Http3QpackDecoderStreamEventType : std::uint8_t {
    None,
    StreamCancellation,
};

struct Http3QpackDecoderStreamEvent {
    Http3QpackDecoderStreamEventType type = Http3QpackDecoderStreamEventType::None;
    std::uint64_t stream_id = 0;
};

class Http3QpackDecoderStreamDecoder {
public:
    [[nodiscard]] Http3ParseStatus parse(mem::IoBufChain &in, Http3QpackDecoderStreamEvent &event) noexcept;
    [[nodiscard]] Http3ParseError error() const noexcept { return error_; }
    void reset() noexcept;

private:
    enum class State : std::uint8_t {
        Instruction,
        StreamCancellation,
        Error,
    };

    [[nodiscard]] Http3ParseStatus fail(common::IoErr io_error = common::IoErr::Invalid) noexcept;

    Http3QpackPrefixIntParser prefix_parser_{};
    Http3ParseError error_{.h3_error = Http3ErrorCode::QpackDecoderStreamError};
    State state_ = State::Instruction;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP3_QPACK_CONTROL_STREAM_DECODER_H
