#include "SseParser.h"

#include <algorithm>
#include <cstring>

#include <fiber/common/Assert.h>

namespace fiber::ai_server {
namespace {

constexpr std::string_view kDataField = "data";
constexpr std::size_t kInitialAssemblyBytes = 256;

std::string_view io_buf_view(const mem::IoBuf &buffer) noexcept {
    return {reinterpret_cast<const char *>(buffer.readable_data()), buffer.readable()};
}

} // namespace

bool SseParser::feed(const mem::IoBuf &chunk) noexcept {
    if (failed_ || final_) {
        error_ = SseParseError::InvalidState;
        failed_ = true;
        return false;
    }
    const std::string_view input = io_buf_view(chunk);
    if (!cursor_.feed(input)) {
        error_ = SseParseError::InvalidState;
        failed_ = true;
        return false;
    }
    current_input_ = &chunk;
    return true;
}

bool SseParser::finish() noexcept {
    if (failed_ || final_) {
        error_ = SseParseError::InvalidState;
        failed_ = true;
        return false;
    }
    final_ = true;
    cursor_.finish();
    return true;
}

bool SseParser::is_data_field() const noexcept {
    return field_name_matches_data_ && field_name_size_ == kDataField.size();
}

bool SseParser::start_data_line() noexcept {
    if (event_has_data_ && !append_data_separator()) {
        return false;
    }
    event_has_data_ = true;
    current_line_data_ = true;
    return true;
}

bool SseParser::append_data(std::string_view fragment) noexcept {
    if (fragment.size() > max_data_bytes_ || data_size_ > max_data_bytes_ - fragment.size()) {
        error_ = SseParseError::DataTooLarge;
        failed_ = true;
        return false;
    }
    if (fragment.empty()) {
        return true;
    }

    if (data_storage_ == DataStorage::None && data_size_ == 0) {
        FIBER_ASSERT(current_input_ != nullptr);
        const std::string_view input = io_buf_view(*current_input_);
        FIBER_ASSERT(fragment.data() >= input.data());
        FIBER_ASSERT(fragment.data() + fragment.size() <= input.data() + input.size());
        data_view_ = fragment;
        data_size_ = fragment.size();
        data_storage_ = DataStorage::BorrowedInput;
        return true;
    }

    if (!ensure_assembled(data_size_ + fragment.size())) {
        return false;
    }
    std::memcpy(assembled_data_.writable_data(), fragment.data(), fragment.size());
    assembled_data_.commit(fragment.size());
    data_size_ += fragment.size();
    data_view_ = io_buf_view(assembled_data_);
    return true;
}

bool SseParser::append_data_separator() noexcept {
    if (data_size_ >= max_data_bytes_) {
        error_ = SseParseError::DataTooLarge;
        failed_ = true;
        return false;
    }
    if (!ensure_assembled(data_size_ + 1)) {
        return false;
    }
    *assembled_data_.writable_data() = '\n';
    assembled_data_.commit(1);
    ++data_size_;
    data_view_ = io_buf_view(assembled_data_);
    return true;
}

bool SseParser::ensure_assembled(std::size_t required) noexcept {
    if (data_storage_ == DataStorage::Assembled && assembled_data_.writable() >= required - data_size_) {
        return true;
    }

    std::size_t capacity = assembled_data_ ? assembled_data_.capacity() : 0;
    if (capacity < required) {
        capacity = std::min(max_data_bytes_, std::max(kInitialAssemblyBytes, capacity));
        while (capacity < required) {
            if (capacity > max_data_bytes_ / 2) {
                capacity = max_data_bytes_;
            } else {
                capacity *= 2;
            }
        }
    }

    if (data_storage_ == DataStorage::Assembled) {
        mem::IoBuf replacement = mem::IoBuf::allocate(capacity);
        if (!replacement) {
            error_ = SseParseError::NoMemory;
            failed_ = true;
            return false;
        }
        FIBER_ASSERT(assembled_data_.readable() == data_size_);
        if (data_size_ != 0) {
            std::memcpy(replacement.writable_data(), assembled_data_.readable_data(), data_size_);
            replacement.commit(data_size_);
        }
        assembled_data_ = std::move(replacement);
        data_view_ = io_buf_view(assembled_data_);
        return true;
    }

    if (assembled_data_ && assembled_data_.capacity() >= required) {
        assembled_data_.clear();
    } else {
        mem::IoBuf replacement = mem::IoBuf::allocate(capacity);
        if (!replacement) {
            error_ = SseParseError::NoMemory;
            failed_ = true;
            return false;
        }
        assembled_data_ = std::move(replacement);
    }

    if (data_size_ != 0) {
        FIBER_ASSERT(data_view_.size() == data_size_);
        std::memcpy(assembled_data_.writable_data(), data_view_.data(), data_view_.size());
        assembled_data_.commit(data_view_.size());
    }
    retained_data_ = {};
    data_storage_ = DataStorage::Assembled;
    data_view_ = io_buf_view(assembled_data_);
    return true;
}

void SseParser::retain_borrowed_data() noexcept {
    if (data_storage_ != DataStorage::BorrowedInput) {
        return;
    }
    FIBER_ASSERT(current_input_ != nullptr);
    const std::string_view input = io_buf_view(*current_input_);
    FIBER_ASSERT(data_view_.data() >= input.data());
    FIBER_ASSERT(data_view_.data() + data_view_.size() <= input.data() + input.size());
    const std::size_t offset = static_cast<std::size_t>(data_view_.data() - input.data());
    retained_data_ = current_input_->retain_slice(offset, data_view_.size());
    data_view_ = io_buf_view(retained_data_);
    data_storage_ = DataStorage::RetainedSlice;
}

void SseParser::reset_line() noexcept {
    field_name_size_ = 0;
    field_name_matches_data_ = true;
    field_colon_seen_ = false;
    current_line_data_ = false;
}

void SseParser::reset_event() noexcept {
    reset_line();
    retained_data_ = {};
    if (assembled_data_) {
        assembled_data_.clear();
    }
    data_view_ = {};
    event_ = {};
    data_size_ = 0;
    data_storage_ = DataStorage::None;
    event_has_data_ = false;
}

void SseParser::publish_event() noexcept {
    if (data_storage_ == DataStorage::Assembled) {
        data_view_ = io_buf_view(assembled_data_);
    } else if (data_storage_ == DataStorage::RetainedSlice) {
        data_view_ = io_buf_view(retained_data_);
    }
    event_ = SseEventView{.data = data_view_};
    event_pending_ = true;
}

SseParseStatus SseParser::next() noexcept {
    if (failed_) {
        return SseParseStatus::Error;
    }
    if (event_pending_) {
        event_pending_ = false;
        reset_event();
    }

    for (;;) {
        const http::SseCursorResult result = cursor_.next();
        switch (result.status) {
            case http::SseCursorStatus::FieldName:
                for (const char value: result.fragment) {
                    if (field_name_size_ >= kDataField.size() || value != kDataField[field_name_size_]) {
                        field_name_matches_data_ = false;
                    }
                    ++field_name_size_;
                }
                break;

            case http::SseCursorStatus::Colon:
                field_colon_seen_ = true;
                if (is_data_field() && !start_data_line()) {
                    return SseParseStatus::Error;
                }
                break;

            case http::SseCursorStatus::FieldValue:
                if (current_line_data_ && !append_data(result.fragment)) {
                    return SseParseStatus::Error;
                }
                break;

            case http::SseCursorStatus::CommentStart:
                field_name_matches_data_ = false;
                break;

            case http::SseCursorStatus::Comment:
                break;

            case http::SseCursorStatus::LineEnd:
                if (!field_colon_seen_ && is_data_field() && !start_data_line()) {
                    return SseParseStatus::Error;
                }
                reset_line();
                break;

            case http::SseCursorStatus::EventEnd:
                reset_line();
                if (event_has_data_) {
                    publish_event();
                    return SseParseStatus::Event;
                }
                reset_event();
                break;

            case http::SseCursorStatus::NeedMore:
                retain_borrowed_data();
                current_input_ = nullptr;
                return SseParseStatus::NeedMore;

            case http::SseCursorStatus::Complete:
                current_input_ = nullptr;
                if (event_has_data_) {
                    publish_event();
                    return SseParseStatus::Event;
                }
                return SseParseStatus::Complete;
        }
    }
}

} // namespace fiber::ai_server
