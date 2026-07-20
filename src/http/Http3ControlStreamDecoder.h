#ifndef FIBER_HTTP_HTTP3_CONTROL_STREAM_DECODER_H
#define FIBER_HTTP_HTTP3_CONTROL_STREAM_DECODER_H

#include <cstdint>

#include "Http3Codec.h"

namespace fiber::http {

enum class Http3ControlStreamEventType : std::uint8_t {
    None,
    Settings,
    Goaway,
    MaxPushId,
};

struct Http3ControlStreamEvent {
    Http3ControlStreamEventType type = Http3ControlStreamEventType::None;
    Http3Settings settings{};
    std::uint64_t id = 0;
};

class Http3ControlStreamDecoder {
public:
    [[nodiscard]] Http3ParseStatus parse(mem::IoBufChain &in, Http3ControlStreamEvent &event) noexcept;
    [[nodiscard]] Http3ParseError error() const noexcept { return error_; }
    void reset() noexcept;

private:
    enum class State : std::uint8_t {
        FrameHeader,
        Settings,
        Varint,
        Skip,
        Error,
    };

    [[nodiscard]] Http3ParseStatus fail(Http3ErrorCode h3_error, common::IoErr io_error = common::IoErr::None) noexcept;

    Http3FrameHeaderParser frame_header_parser_{};
    Http3SettingsParser settings_parser_{};
    Http3FrameVarintParser varint_parser_{};
    Http3PayloadSkipParser skip_parser_{};
    Http3FrameHeader header_{};
    Http3ParseError error_{};
    State state_ = State::FrameHeader;
    Http3ControlStreamEventType varint_event_type_ = Http3ControlStreamEventType::None;
    bool first_frame_ = true;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP3_CONTROL_STREAM_DECODER_H
