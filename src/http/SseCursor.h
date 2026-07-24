#ifndef FIBER_HTTP_SSE_CURSOR_H
#define FIBER_HTTP_SSE_CURSOR_H

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fiber::http {

enum class SseCursorStatus : std::uint8_t {
    FieldName,
    Colon,
    FieldValue,
    CommentStart,
    Comment,
    LineEnd,
    EventEnd,
    NeedMore,
    Complete,
};

struct SseCursorResult {
    SseCursorStatus status = SseCursorStatus::NeedMore;
    std::string_view fragment;
};

class SseCursor {
public:
    SseCursor() noexcept = default;

    SseCursor(const SseCursor &) = delete;
    SseCursor &operator=(const SseCursor &) = delete;
    SseCursor(SseCursor &&) = delete;
    SseCursor &operator=(SseCursor &&) = delete;

    void reset() noexcept;

    // The input is borrowed until next() returns NeedMore or Complete. A new
    // chunk may only be fed after the previous chunk has been drained.
    [[nodiscard]] bool feed(std::string_view input) noexcept;

    // Marks the input stream as finished. Any active chunk must still be
    // drained. EOF closes a partial line before Complete, but never
    // synthesizes an EventEnd.
    void finish() noexcept;

    [[nodiscard]] bool input_finished() const noexcept { return final_; }

    // FieldName, FieldValue, and Comment fragments borrow the current input.
    // Only the first colon on a field line is returned as Colon. The optional
    // single ASCII space after it is omitted from FieldValue fragments. A
    // non-empty line without Colon returns FieldName fragments followed by
    // LineEnd, which represents an empty field value. EventEnd is returned
    // only for an empty line present in the input.
    [[nodiscard]] SseCursorResult next() noexcept;

private:
    enum class LineState : std::uint8_t {
        Start,
        FieldName,
        BeforeValue,
        FieldValue,
        Comment,
    };

    void clear_input() noexcept;
    [[nodiscard]] SseCursorResult end_line() noexcept;
    [[nodiscard]] SseCursorResult exhausted() noexcept;
    [[nodiscard]] SseCursorResult finish_stream() noexcept;

    std::string_view input_;
    std::size_t offset_ = 0;
    LineState line_state_ = LineState::Start;
    bool input_active_ = false;
    bool final_ = false;
    bool complete_ = false;
    bool skip_lf_after_cr_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_SSE_CURSOR_H
