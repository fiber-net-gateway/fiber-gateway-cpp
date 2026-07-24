#include "SseCursor.h"

#include "../common/Assert.h"

namespace fiber::http {
namespace {

[[nodiscard]] bool is_line_end(char ch) noexcept { return ch == '\r' || ch == '\n'; }

} // namespace

void SseCursor::reset() noexcept {
    input_ = {};
    offset_ = 0;
    line_state_ = LineState::Start;
    input_active_ = false;
    final_ = false;
    complete_ = false;
    skip_lf_after_cr_ = false;
}

bool SseCursor::feed(std::string_view input) noexcept {
    if (input_active_ || final_ || complete_) {
        return false;
    }
    input_ = input;
    offset_ = 0;
    input_active_ = true;
    return true;
}

void SseCursor::finish() noexcept { final_ = true; }

SseCursorResult SseCursor::next() noexcept {
    if (complete_) {
        return {.status = SseCursorStatus::Complete};
    }

    for (;;) {
        if (!input_active_) {
            if (final_) {
                return finish_stream();
            }
            return {.status = SseCursorStatus::NeedMore};
        }
        if (offset_ == input_.size()) {
            return exhausted();
        }

        if (skip_lf_after_cr_) {
            skip_lf_after_cr_ = false;
            if (input_[offset_] == '\n') {
                ++offset_;
                continue;
            }
        }

        switch (line_state_) {
            case LineState::Start:
                if (input_[offset_] == ':') {
                    ++offset_;
                    line_state_ = LineState::Comment;
                    return {.status = SseCursorStatus::CommentStart};
                }
                if (is_line_end(input_[offset_])) {
                    return end_line();
                }
                line_state_ = LineState::FieldName;
                break;

            case LineState::FieldName: {
                const std::size_t begin = offset_;
                while (offset_ < input_.size() && input_[offset_] != ':' && !is_line_end(input_[offset_])) {
                    ++offset_;
                }
                if (offset_ != begin) {
                    return {
                            .status = SseCursorStatus::FieldName,
                            .fragment = input_.substr(begin, offset_ - begin),
                    };
                }
                if (input_[offset_] == ':') {
                    ++offset_;
                    line_state_ = LineState::BeforeValue;
                    return {.status = SseCursorStatus::Colon};
                }
                return end_line();
            }

            case LineState::BeforeValue:
                if (input_[offset_] == ' ') {
                    ++offset_;
                }
                line_state_ = LineState::FieldValue;
                break;

            case LineState::FieldValue: {
                const std::size_t begin = offset_;
                while (offset_ < input_.size() && !is_line_end(input_[offset_])) {
                    ++offset_;
                }
                if (offset_ != begin) {
                    return {
                            .status = SseCursorStatus::FieldValue,
                            .fragment = input_.substr(begin, offset_ - begin),
                    };
                }
                return end_line();
            }

            case LineState::Comment: {
                const std::size_t begin = offset_;
                while (offset_ < input_.size() && !is_line_end(input_[offset_])) {
                    ++offset_;
                }
                if (offset_ != begin) {
                    return {
                            .status = SseCursorStatus::Comment,
                            .fragment = input_.substr(begin, offset_ - begin),
                    };
                }
                return end_line();
            }
        }
    }
}

void SseCursor::clear_input() noexcept {
    input_ = {};
    offset_ = 0;
    input_active_ = false;
}

SseCursorResult SseCursor::end_line() noexcept {
    FIBER_ASSERT(input_active_);
    FIBER_ASSERT(offset_ < input_.size());
    FIBER_ASSERT(is_line_end(input_[offset_]));

    const bool event_end = line_state_ == LineState::Start;
    if (input_[offset_++] == '\r') {
        skip_lf_after_cr_ = true;
    }
    line_state_ = LineState::Start;
    return {.status = event_end ? SseCursorStatus::EventEnd : SseCursorStatus::LineEnd};
}

SseCursorResult SseCursor::exhausted() noexcept {
    clear_input();
    if (final_) {
        return finish_stream();
    }
    return {.status = SseCursorStatus::NeedMore};
}

SseCursorResult SseCursor::finish_stream() noexcept {
    if (line_state_ != LineState::Start) {
        line_state_ = LineState::Start;
        return {.status = SseCursorStatus::LineEnd};
    }
    complete_ = true;
    return {.status = SseCursorStatus::Complete};
}

} // namespace fiber::http
