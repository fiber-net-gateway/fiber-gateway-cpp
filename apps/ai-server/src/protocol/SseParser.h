#ifndef FIBER_AI_SERVER_SSE_PARSER_H
#define FIBER_AI_SERVER_SSE_PARSER_H

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <fiber/common/mem/IoBuf.h>
#include <fiber/http/SseCursor.h>

namespace fiber::ai_server {

enum class SseParseStatus : std::uint8_t {
    Event,
    NeedMore,
    Complete,
    Error,
};

enum class SseParseError : std::uint8_t {
    DataTooLarge,
    NoMemory,
    InvalidState,
};

struct SseEventView {
    std::string_view data;
};

class SseParser {
public:
    explicit SseParser(std::size_t max_data_bytes = 1024 * 1024) noexcept : max_data_bytes_(max_data_bytes) {}

    // The input must remain alive until next() returns NeedMore, Complete, or
    // Error. A completed event borrows the input until the following next().
    [[nodiscard]] bool feed(const mem::IoBuf &chunk) noexcept;
    [[nodiscard]] bool finish() noexcept;
    [[nodiscard]] SseParseStatus next() noexcept;

    [[nodiscard]] const SseEventView &event() const noexcept { return event_; }
    [[nodiscard]] SseParseError error() const noexcept { return error_; }

private:
    enum class DataStorage : std::uint8_t {
        None,
        BorrowedInput,
        RetainedSlice,
        Assembled,
    };

    [[nodiscard]] bool is_data_field() const noexcept;
    [[nodiscard]] bool start_data_line() noexcept;
    [[nodiscard]] bool append_data(std::string_view fragment) noexcept;
    [[nodiscard]] bool append_data_separator() noexcept;
    [[nodiscard]] bool ensure_assembled(std::size_t required) noexcept;
    void retain_borrowed_data() noexcept;
    void reset_line() noexcept;
    void reset_event() noexcept;
    void publish_event() noexcept;

    std::size_t max_data_bytes_ = 0;
    http::SseCursor cursor_;
    const mem::IoBuf *current_input_ = nullptr;
    mem::IoBuf retained_data_;
    mem::IoBuf assembled_data_;
    std::string_view data_view_;
    SseEventView event_;
    std::size_t data_size_ = 0;
    std::size_t field_name_size_ = 0;
    DataStorage data_storage_ = DataStorage::None;
    SseParseError error_ = SseParseError::InvalidState;
    bool field_name_matches_data_ = true;
    bool field_colon_seen_ = false;
    bool current_line_data_ = false;
    bool event_has_data_ = false;
    bool event_pending_ = false;
    bool final_ = false;
    bool failed_ = false;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_SSE_PARSER_H
