#include "http/Http3ControlStreamDecoder.h"

namespace fiber::http {

namespace {

[[nodiscard]] bool is_http2_only_frame_type(std::uint64_t type) noexcept {
    return type == 0x02U || type == 0x06U || type == 0x08U || type == 0x09U;
}

} // namespace

Http3ParseStatus Http3ControlStreamDecoder::parse(mem::IoBufChain &in, Http3ControlStreamEvent &event) noexcept {
    event = {};

    for (;;) {
        switch (state_) {
            case State::FrameHeader: {
                Http3ParseStatus status = frame_header_parser_.parse(in);
                if (status != Http3ParseStatus::Done) {
                    return status;
                }
                header_ = frame_header_parser_.header();
                frame_header_parser_.reset();

                if (first_frame_ && header_.type != static_cast<std::uint64_t>(Http3FrameType::Settings)) {
                    return fail(Http3ErrorCode::MissingSettings);
                }
                if (!first_frame_ && header_.type == static_cast<std::uint64_t>(Http3FrameType::Settings)) {
                    return fail(Http3ErrorCode::FrameUnexpected);
                }
                first_frame_ = false;

                if (is_http2_only_frame_type(header_.type) ||
                    header_.type == static_cast<std::uint64_t>(Http3FrameType::Data) ||
                    header_.type == static_cast<std::uint64_t>(Http3FrameType::Headers) ||
                    header_.type == static_cast<std::uint64_t>(Http3FrameType::PushPromise)) {
                    return fail(Http3ErrorCode::FrameUnexpected);
                }
                if (header_.type == static_cast<std::uint64_t>(Http3FrameType::CancelPush)) {
                    return fail(Http3ErrorCode::IdError);
                }

                if (header_.type == static_cast<std::uint64_t>(Http3FrameType::Settings)) {
                    settings_parser_.start(header_.length);
                    state_ = State::Settings;
                    break;
                }

                if (header_.type == static_cast<std::uint64_t>(Http3FrameType::Goaway) ||
                    header_.type == static_cast<std::uint64_t>(Http3FrameType::MaxPushId)) {
                    varint_parser_.start(header_.length);
                    varint_event_type_ = header_.type == static_cast<std::uint64_t>(Http3FrameType::Goaway)
                                                 ? Http3ControlStreamEventType::Goaway
                                                 : Http3ControlStreamEventType::MaxPushId;
                    state_ = State::Varint;
                    break;
                }

                skip_parser_.start(header_.length);
                state_ = State::Skip;
                break;
            }

            case State::Settings: {
                Http3ParseStatus status = settings_parser_.parse(in);
                if (status == Http3ParseStatus::Error) {
                    error_ = settings_parser_.error();
                    state_ = State::Error;
                    return Http3ParseStatus::Error;
                }
                if (status != Http3ParseStatus::Done) {
                    return status;
                }

                event.type = Http3ControlStreamEventType::Settings;
                event.settings = settings_parser_.settings();
                settings_parser_.reset();
                state_ = State::FrameHeader;
                return Http3ParseStatus::Done;
            }

            case State::Varint: {
                Http3ParseStatus status = varint_parser_.parse(in);
                if (status == Http3ParseStatus::Error) {
                    error_ = varint_parser_.error();
                    state_ = State::Error;
                    return Http3ParseStatus::Error;
                }
                if (status != Http3ParseStatus::Done) {
                    return status;
                }

                event.type = varint_event_type_;
                event.id = varint_parser_.value();
                varint_parser_.reset();
                varint_event_type_ = Http3ControlStreamEventType::None;
                state_ = State::FrameHeader;
                return Http3ParseStatus::Done;
            }

            case State::Skip: {
                Http3ParseStatus status = skip_parser_.parse(in);
                if (status != Http3ParseStatus::Done) {
                    return status;
                }
                skip_parser_.reset();
                state_ = State::FrameHeader;
                break;
            }

            case State::Error:
                return Http3ParseStatus::Error;
        }
    }
}

void Http3ControlStreamDecoder::reset() noexcept {
    frame_header_parser_.reset();
    settings_parser_.reset();
    varint_parser_.reset();
    skip_parser_.reset();
    header_ = {};
    error_ = {};
    state_ = State::FrameHeader;
    varint_event_type_ = Http3ControlStreamEventType::None;
    first_frame_ = true;
}

Http3ParseStatus Http3ControlStreamDecoder::fail(Http3ErrorCode h3_error, common::IoErr io_error) noexcept {
    error_ = {.h3_error = h3_error, .io_error = io_error};
    state_ = State::Error;
    return Http3ParseStatus::Error;
}

} // namespace fiber::http
