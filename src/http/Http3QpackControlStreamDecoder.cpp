#include "Http3QpackControlStreamDecoder.h"

namespace fiber::http {

namespace {

[[nodiscard]] bool consume_byte(mem::IoBufChain &in, std::uint8_t &out) noexcept {
    mem::IoBuf *buf = in.first_readable();
    if (buf == nullptr || buf->readable() == 0) {
        return false;
    }
    out = *buf->readable_data();
    in.consume_and_compact(1);
    return true;
}

} // namespace

Http3ParseStatus Http3QpackEncoderStreamDecoder::parse(mem::IoBufChain &in) noexcept {
    for (;;) {
        switch (state_) {
            case State::Instruction: {
                std::uint8_t byte = 0;
                if (!consume_byte(in, byte)) {
                    return Http3ParseStatus::NeedMore;
                }
                if ((byte & 0xe0U) != 0x20U) {
                    return fail();
                }
                prefix_parser_.start(byte, 5);
                state_ = State::Capacity;
                break;
            }

            case State::Capacity: {
                Http3ParseStatus status = prefix_parser_.parse(in);
                if (status == Http3ParseStatus::Error) {
                    return fail(prefix_parser_.error().io_error);
                }
                if (status != Http3ParseStatus::Done) {
                    return status;
                }
                if (prefix_parser_.value() != 0) {
                    return fail();
                }
                prefix_parser_.reset();
                state_ = State::Instruction;
                break;
            }

            case State::Error:
                return Http3ParseStatus::Error;
        }
    }
}

void Http3QpackEncoderStreamDecoder::reset() noexcept {
    prefix_parser_.reset();
    error_ = {.h3_error = Http3ErrorCode::QpackEncoderStreamError};
    state_ = State::Instruction;
}

Http3ParseStatus Http3QpackEncoderStreamDecoder::fail(common::IoErr io_error) noexcept {
    error_ = {.h3_error = Http3ErrorCode::QpackEncoderStreamError, .io_error = io_error};
    state_ = State::Error;
    return Http3ParseStatus::Error;
}

Http3ParseStatus Http3QpackDecoderStreamDecoder::parse(mem::IoBufChain &in,
                                                       Http3QpackDecoderStreamEvent &event) noexcept {
    event = {};

    for (;;) {
        switch (state_) {
            case State::Instruction: {
                std::uint8_t byte = 0;
                if (!consume_byte(in, byte)) {
                    return Http3ParseStatus::NeedMore;
                }
                if ((byte & 0x80U) != 0) {
                    return fail();
                }
                if ((byte & 0x40U) == 0) {
                    return fail();
                }

                prefix_parser_.start(byte, 6);
                state_ = State::StreamCancellation;
                break;
            }

            case State::StreamCancellation: {
                Http3ParseStatus status = prefix_parser_.parse(in);
                if (status == Http3ParseStatus::Error) {
                    return fail(prefix_parser_.error().io_error);
                }
                if (status != Http3ParseStatus::Done) {
                    return status;
                }

                event.type = Http3QpackDecoderStreamEventType::StreamCancellation;
                event.stream_id = prefix_parser_.value();
                prefix_parser_.reset();
                state_ = State::Instruction;
                return Http3ParseStatus::Done;
            }

            case State::Error:
                return Http3ParseStatus::Error;
        }
    }
}

void Http3QpackDecoderStreamDecoder::reset() noexcept {
    prefix_parser_.reset();
    error_ = {.h3_error = Http3ErrorCode::QpackDecoderStreamError};
    state_ = State::Instruction;
}

Http3ParseStatus Http3QpackDecoderStreamDecoder::fail(common::IoErr io_error) noexcept {
    error_ = {.h3_error = Http3ErrorCode::QpackDecoderStreamError, .io_error = io_error};
    state_ = State::Error;
    return Http3ParseStatus::Error;
}

} // namespace fiber::http
