#include "FormattedLogRecord.h"

#include <cstring>

#include "../common/Assert.h"

namespace fiber::log {
namespace {

constexpr char kLogNewline = '\n';

} // namespace

FormattedLogRecord::Cursor::Cursor(const FormattedLogRecord &record) noexcept :
    record_(&record), chunk_(record.record_->first_chunk()), stage_(Stage::Prefix) {
    normalize();
}

void FormattedLogRecord::Cursor::normalize() noexcept {
    for (;;) {
        switch (stage_) {
            case Stage::Prefix:
                if (record_->prefix_size_ > 0) {
                    return;
                }
                stage_ = Stage::InlineMessage;
                break;
            case Stage::InlineMessage:
                if (record_->record_->inline_message_size() > 0) {
                    return;
                }
                stage_ = Stage::MessageChunks;
                break;
            case Stage::MessageChunks:
                while (chunk_ && chunk_->size == 0) {
                    chunk_ = chunk_->next;
                }
                if (chunk_) {
                    return;
                }
                stage_ = Stage::Newline;
                break;
            case Stage::Newline:
            case Stage::Done:
                return;
        }
    }
}

LogSegment FormattedLogRecord::Cursor::current() const noexcept {
    switch (stage_) {
        case Stage::Prefix:
            return {.data = record_->prefix_ + offset_, .size = record_->prefix_size_ - offset_};
        case Stage::InlineMessage:
            return {
                    .data = record_->record_->inline_message() + offset_,
                    .size = record_->record_->inline_message_size() - offset_,
            };
        case Stage::MessageChunks:
            return {.data = chunk_->data + offset_, .size = chunk_->size - offset_};
        case Stage::Newline:
            return {.data = &kLogNewline + offset_, .size = 1 - offset_};
        case Stage::Done:
            return {};
    }
    return {};
}

void FormattedLogRecord::Cursor::advance(std::size_t bytes) noexcept {
    while (bytes > 0 && stage_ != Stage::Done) {
        const LogSegment segment = current();
        FIBER_ASSERT(segment.size > 0);
        if (bytes < segment.size) {
            offset_ += bytes;
            return;
        }
        bytes -= segment.size;
        offset_ = 0;
        switch (stage_) {
            case Stage::Prefix:
                stage_ = Stage::InlineMessage;
                break;
            case Stage::InlineMessage:
                stage_ = Stage::MessageChunks;
                break;
            case Stage::MessageChunks:
                chunk_ = chunk_->next;
                break;
            case Stage::Newline:
                stage_ = Stage::Done;
                break;
            case Stage::Done:
                break;
        }
        normalize();
    }
    FIBER_ASSERT(bytes == 0);
}

bool FormattedLogRecord::copy_to(char *destination, std::size_t capacity) const noexcept {
    if (!destination || capacity < total_size_) {
        return false;
    }
    Cursor position(*this);
    std::size_t offset = 0;
    while (!position.done()) {
        const LogSegment segment = position.current();
        std::memcpy(destination + offset, segment.data, segment.size);
        offset += segment.size;
        position.advance(segment.size);
    }
    return offset == total_size_;
}

} // namespace fiber::log
