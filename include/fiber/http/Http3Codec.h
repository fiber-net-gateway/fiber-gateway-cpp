#ifndef FIBER_HTTP_HTTP3_CODEC_H
#define FIBER_HTTP_HTTP3_CODEC_H

#include <cstdint>

#include "../common/IoError.h"
#include "../common/mem/IoBufChain.h"
#include "Http3Protocol.h"

namespace fiber::http {

enum class Http3ParseStatus : std::uint8_t {
    NeedMore,
    Done,
    Error,
};

struct Http3ParseError {
    Http3ErrorCode h3_error = Http3ErrorCode::GeneralProtocolError;
    common::IoErr io_error = common::IoErr::None;
};

class Http3VarintParser {
public:
    [[nodiscard]] Http3ParseStatus parse(mem::IoBufChain &in) noexcept;
    [[nodiscard]] std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] Http3ParseError error() const noexcept { return error_; }
    void reset() noexcept;

private:
    std::uint64_t value_ = 0;
    Http3ParseError error_{};
    std::uint8_t target_len_ = 0;
    std::uint8_t read_len_ = 0;
    bool complete_ = false;
};

class Http3FrameHeaderParser {
public:
    [[nodiscard]] Http3ParseStatus parse(mem::IoBufChain &in) noexcept;
    [[nodiscard]] Http3FrameHeader header() const noexcept { return header_; }
    [[nodiscard]] Http3ParseError error() const noexcept { return error_; }
    void reset() noexcept;

private:
    enum class State : std::uint8_t {
        Type,
        Length,
        Done,
        Error,
    };

    Http3VarintParser varint_{};
    Http3FrameHeader header_{};
    Http3ParseError error_{};
    State state_ = State::Type;
};

class Http3PayloadSkipParser {
public:
    void start(std::uint64_t length) noexcept;
    [[nodiscard]] Http3ParseStatus parse(mem::IoBufChain &in) noexcept;
    [[nodiscard]] std::uint64_t remaining() const noexcept { return remaining_; }
    [[nodiscard]] Http3ParseError error() const noexcept { return error_; }
    void reset() noexcept;

private:
    std::uint64_t remaining_ = 0;
    Http3ParseError error_{};
    bool started_ = false;
};

class Http3FrameVarintParser {
public:
    void start(std::uint64_t payload_length) noexcept;
    [[nodiscard]] Http3ParseStatus parse(mem::IoBufChain &in) noexcept;
    [[nodiscard]] std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] Http3ParseError error() const noexcept { return error_; }
    void reset() noexcept;

private:
    [[nodiscard]] Http3ParseStatus fail(common::IoErr io_error = common::IoErr::Invalid) noexcept;

    std::uint64_t value_ = 0;
    std::uint64_t payload_length_ = 0;
    Http3ParseError error_{.h3_error = Http3ErrorCode::FrameError};
    std::uint8_t target_len_ = 0;
    std::uint8_t read_len_ = 0;
    bool started_ = false;
    bool complete_ = false;
};

class Http3SettingsParser {
public:
    void start(std::uint64_t payload_length) noexcept;
    [[nodiscard]] Http3ParseStatus parse(mem::IoBufChain &in) noexcept;
    [[nodiscard]] const Http3Settings &settings() const noexcept { return settings_; }
    [[nodiscard]] Http3ParseError error() const noexcept { return error_; }
    void reset() noexcept;

private:
    enum class State : std::uint8_t {
        Id,
        Value,
        Done,
        Error,
    };

    [[nodiscard]] Http3ParseStatus parse_limited_varint(mem::IoBufChain &in, std::uint64_t &out) noexcept;
    [[nodiscard]] Http3ParseStatus fail(common::IoErr io_error = common::IoErr::Invalid) noexcept;
    [[nodiscard]] Http3ParseStatus apply_setting(std::uint64_t id, std::uint64_t value) noexcept;
    void reset_integer() noexcept;

    Http3Settings settings_{};
    Http3ParseError error_{.h3_error = Http3ErrorCode::SettingsError};
    State state_ = State::Done;
    std::uint64_t remaining_ = 0;
    std::uint64_t id_ = 0;
    std::uint64_t integer_value_ = 0;
    std::uint8_t integer_target_len_ = 0;
    std::uint8_t integer_read_len_ = 0;
    bool integer_started_ = false;
    bool saw_qpack_max_table_capacity_ = false;
    bool saw_qpack_blocked_streams_ = false;
    bool saw_max_field_section_size_ = false;
    bool saw_enable_connect_protocol_ = false;
};

class Http3QpackPrefixIntParser {
public:
    void start(std::uint8_t first, std::uint8_t prefix_bits) noexcept;
    [[nodiscard]] Http3ParseStatus parse(mem::IoBufChain &in) noexcept;
    [[nodiscard]] std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] Http3ParseError error() const noexcept { return error_; }
    void reset() noexcept;

private:
    [[nodiscard]] Http3ParseStatus fail(common::IoErr io_error = common::IoErr::Invalid) noexcept;

    std::uint64_t value_ = 0;
    Http3ParseError error_{};
    std::uint8_t shift_ = 0;
    bool started_ = false;
    bool complete_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP3_CODEC_H
