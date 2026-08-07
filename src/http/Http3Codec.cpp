#include <fiber/http/Http3Codec.h>

#include <algorithm>

#include <fiber/common/Assert.h>

namespace fiber::http {

namespace {

constexpr std::uint64_t kMaxHttp3Integer = (1ULL << 62U) - 1U;

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

Http3ParseStatus Http3VarintParser::parse(mem::IoBufChain &in) noexcept {
    if (complete_) {
        return Http3ParseStatus::Done;
    }

    for (;;) {
        std::uint8_t byte = 0;
        if (!consume_byte(in, byte)) {
            return Http3ParseStatus::NeedMore;
        }

        if (read_len_ == 0) {
            target_len_ = static_cast<std::uint8_t>(1U << (byte >> 6U));
            value_ = byte & 0x3fU;
            read_len_ = 1;
            if (target_len_ == 1) {
                complete_ = true;
                return Http3ParseStatus::Done;
            }
            continue;
        }

        value_ = (value_ << 8U) | byte;
        ++read_len_;
        if (read_len_ == target_len_) {
            complete_ = true;
            return Http3ParseStatus::Done;
        }
    }
}

void Http3VarintParser::reset() noexcept {
    value_ = 0;
    error_ = {};
    target_len_ = 0;
    read_len_ = 0;
    complete_ = false;
}

Http3ParseStatus Http3FrameHeaderParser::parse(mem::IoBufChain &in) noexcept {
    for (;;) {
        switch (state_) {
            case State::Type: {
                Http3ParseStatus status = varint_.parse(in);
                if (status != Http3ParseStatus::Done) {
                    return status;
                }
                header_.type = varint_.value();
                varint_.reset();
                state_ = State::Length;
                break;
            }
            case State::Length: {
                Http3ParseStatus status = varint_.parse(in);
                if (status != Http3ParseStatus::Done) {
                    return status;
                }
                header_.length = varint_.value();
                varint_.reset();
                state_ = State::Done;
                return Http3ParseStatus::Done;
            }
            case State::Done:
                return Http3ParseStatus::Done;
            case State::Error:
                return Http3ParseStatus::Error;
        }
    }
}

void Http3FrameHeaderParser::reset() noexcept {
    varint_.reset();
    header_ = {};
    error_ = {};
    state_ = State::Type;
}

void Http3PayloadSkipParser::start(std::uint64_t length) noexcept {
    remaining_ = length;
    error_ = {};
    started_ = true;
}

Http3ParseStatus Http3PayloadSkipParser::parse(mem::IoBufChain &in) noexcept {
    if (!started_) {
        return Http3ParseStatus::Error;
    }
    while (remaining_ != 0) {
        const std::uint64_t available = static_cast<std::uint64_t>(in.readable_bytes());
        if (available == 0) {
            return Http3ParseStatus::NeedMore;
        }
        const std::uint64_t take = std::min(available, remaining_);
        in.consume_and_compact(static_cast<std::size_t>(take));
        remaining_ -= take;
    }
    return Http3ParseStatus::Done;
}

void Http3PayloadSkipParser::reset() noexcept {
    remaining_ = 0;
    error_ = {};
    started_ = false;
}

void Http3FrameVarintParser::start(std::uint64_t payload_length) noexcept {
    reset();
    payload_length_ = payload_length;
    started_ = true;
}

Http3ParseStatus Http3FrameVarintParser::parse(mem::IoBufChain &in) noexcept {
    if (!started_) {
        return fail();
    }
    if (complete_) {
        return Http3ParseStatus::Done;
    }
    if (payload_length_ == 0 || payload_length_ > 8) {
        return fail();
    }

    while (read_len_ < payload_length_) {
        std::uint8_t byte = 0;
        if (!consume_byte(in, byte)) {
            return Http3ParseStatus::NeedMore;
        }
        if (read_len_ == 0) {
            target_len_ = static_cast<std::uint8_t>(1U << (byte >> 6U));
            if (target_len_ != payload_length_) {
                return fail();
            }
            value_ = byte & 0x3fU;
        } else {
            value_ = (value_ << 8U) | byte;
        }
        ++read_len_;
    }

    complete_ = true;
    return Http3ParseStatus::Done;
}

void Http3FrameVarintParser::reset() noexcept {
    value_ = 0;
    payload_length_ = 0;
    error_ = {.h3_error = Http3ErrorCode::FrameError};
    target_len_ = 0;
    read_len_ = 0;
    started_ = false;
    complete_ = false;
}

Http3ParseStatus Http3FrameVarintParser::fail(common::IoErr io_error) noexcept {
    error_ = {.h3_error = Http3ErrorCode::FrameError, .io_error = io_error};
    started_ = true;
    complete_ = false;
    return Http3ParseStatus::Error;
}

void Http3SettingsParser::start(std::uint64_t payload_length) noexcept {
    reset();
    remaining_ = payload_length;
    state_ = payload_length == 0 ? State::Done : State::Id;
}

Http3ParseStatus Http3SettingsParser::parse(mem::IoBufChain &in) noexcept {
    for (;;) {
        switch (state_) {
            case State::Id: {
                if (remaining_ == 0) {
                    state_ = State::Done;
                    return Http3ParseStatus::Done;
                }
                Http3ParseStatus status = parse_limited_varint(in, id_);
                if (status != Http3ParseStatus::Done) {
                    return status;
                }
                state_ = State::Value;
                break;
            }
            case State::Value: {
                if (remaining_ == 0 && !integer_started_) {
                    return fail();
                }
                std::uint64_t value = 0;
                Http3ParseStatus status = parse_limited_varint(in, value);
                if (status != Http3ParseStatus::Done) {
                    return status;
                }
                status = apply_setting(id_, value);
                if (status != Http3ParseStatus::Done) {
                    return status;
                }
                state_ = remaining_ == 0 ? State::Done : State::Id;
                if (state_ == State::Done) {
                    return Http3ParseStatus::Done;
                }
                break;
            }
            case State::Done:
                return Http3ParseStatus::Done;
            case State::Error:
                return Http3ParseStatus::Error;
        }
    }
}

void Http3SettingsParser::reset() noexcept {
    settings_ = {};
    error_ = {.h3_error = Http3ErrorCode::SettingsError};
    state_ = State::Done;
    remaining_ = 0;
    id_ = 0;
    reset_integer();
    saw_qpack_max_table_capacity_ = false;
    saw_qpack_blocked_streams_ = false;
    saw_max_field_section_size_ = false;
    saw_enable_connect_protocol_ = false;
}

Http3ParseStatus Http3SettingsParser::parse_limited_varint(mem::IoBufChain &in, std::uint64_t &out) noexcept {
    if (!integer_started_) {
        if (remaining_ == 0) {
            return fail();
        }

        std::uint8_t first = 0;
        if (!consume_byte(in, first)) {
            return Http3ParseStatus::NeedMore;
        }
        --remaining_;

        integer_target_len_ = static_cast<std::uint8_t>(1U << (first >> 6U));
        if (remaining_ < static_cast<std::uint64_t>(integer_target_len_ - 1U)) {
            return fail();
        }

        integer_value_ = first & 0x3fU;
        integer_read_len_ = 1;
        integer_started_ = true;
        if (integer_target_len_ == 1) {
            out = integer_value_;
            reset_integer();
            return Http3ParseStatus::Done;
        }
    }

    while (integer_read_len_ < integer_target_len_) {
        std::uint8_t byte = 0;
        if (!consume_byte(in, byte)) {
            return Http3ParseStatus::NeedMore;
        }
        --remaining_;
        integer_value_ = (integer_value_ << 8U) | byte;
        ++integer_read_len_;
    }

    out = integer_value_;
    reset_integer();
    return Http3ParseStatus::Done;
}

Http3ParseStatus Http3SettingsParser::fail(common::IoErr io_error) noexcept {
    error_ = {.h3_error = Http3ErrorCode::SettingsError, .io_error = io_error};
    state_ = State::Error;
    return Http3ParseStatus::Error;
}

Http3ParseStatus Http3SettingsParser::apply_setting(std::uint64_t id, std::uint64_t value) noexcept {
    switch (static_cast<Http3SettingId>(id)) {
        case Http3SettingId::QpackMaxTableCapacity:
            if (saw_qpack_max_table_capacity_) {
                return fail(common::IoErr::Already);
            }
            saw_qpack_max_table_capacity_ = true;
            settings_.qpack_max_table_capacity = value;
            return Http3ParseStatus::Done;
        case Http3SettingId::QpackBlockedStreams:
            if (saw_qpack_blocked_streams_) {
                return fail(common::IoErr::Already);
            }
            saw_qpack_blocked_streams_ = true;
            settings_.qpack_blocked_streams = value;
            return Http3ParseStatus::Done;
        case Http3SettingId::MaxFieldSectionSize:
            if (saw_max_field_section_size_) {
                return fail(common::IoErr::Already);
            }
            saw_max_field_section_size_ = true;
            settings_.max_field_section_size = value;
            return Http3ParseStatus::Done;
        case Http3SettingId::EnableConnectProtocol:
            if (saw_enable_connect_protocol_ || value > 1) {
                return fail(common::IoErr::Already);
            }
            saw_enable_connect_protocol_ = true;
            settings_.enable_connect_protocol = value == 1;
            return Http3ParseStatus::Done;
        default:
            return Http3ParseStatus::Done;
    }
}

void Http3SettingsParser::reset_integer() noexcept {
    integer_value_ = 0;
    integer_target_len_ = 0;
    integer_read_len_ = 0;
    integer_started_ = false;
}

void Http3QpackPrefixIntParser::start(std::uint8_t first, std::uint8_t prefix_bits) noexcept {
    FIBER_ASSERT(prefix_bits > 0 && prefix_bits < 8);
    reset();
    const std::uint64_t prefix_max = (1ULL << prefix_bits) - 1ULL;
    value_ = first & prefix_max;
    started_ = true;
    complete_ = value_ < prefix_max;
}

Http3ParseStatus Http3QpackPrefixIntParser::parse(mem::IoBufChain &in) noexcept {
    if (!started_) {
        return fail();
    }
    if (complete_) {
        return Http3ParseStatus::Done;
    }

    for (;;) {
        std::uint8_t byte = 0;
        if (!consume_byte(in, byte)) {
            return Http3ParseStatus::NeedMore;
        }

        const std::uint64_t chunk = byte & 0x7fU;
        if (shift_ >= 63U && chunk != 0) {
            return fail();
        }
        if (shift_ > 56U && (byte & 0x80U) != 0) {
            return fail();
        }

        const std::uint64_t contribution = chunk << shift_;
        if (contribution > kMaxHttp3Integer || value_ > kMaxHttp3Integer - contribution) {
            return fail();
        }
        value_ += contribution;
        if ((byte & 0x80U) == 0) {
            complete_ = true;
            return Http3ParseStatus::Done;
        }
        if (shift_ > 56U) {
            return fail();
        }
        shift_ = static_cast<std::uint8_t>(shift_ + 7U);
    }
}

void Http3QpackPrefixIntParser::reset() noexcept {
    value_ = 0;
    error_ = {};
    shift_ = 0;
    started_ = false;
    complete_ = false;
}

Http3ParseStatus Http3QpackPrefixIntParser::fail(common::IoErr io_error) noexcept {
    error_ = {.h3_error = Http3ErrorCode::GeneralProtocolError, .io_error = io_error};
    started_ = true;
    complete_ = false;
    return Http3ParseStatus::Error;
}

} // namespace fiber::http
