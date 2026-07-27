#ifndef FIBER_LOG_FORMATTED_LOG_RECORD_H
#define FIBER_LOG_FORMATTED_LOG_RECORD_H

#include <cstddef>

#include "LogRecord.h"

namespace fiber::log {

struct LogSegment {
    const char *data = nullptr;
    std::size_t size = 0;
};

class FormattedLogRecord {
public:
    class Cursor {
    public:
        Cursor() noexcept = default;
        explicit Cursor(const FormattedLogRecord &record) noexcept;

        [[nodiscard]] bool done() const noexcept { return stage_ == Stage::Done; }
        [[nodiscard]] LogSegment current() const noexcept;
        void advance(std::size_t bytes) noexcept;

    private:
        enum class Stage : std::uint8_t {
            Prefix,
            InlineMessage,
            MessageChunks,
            Newline,
            Done,
        };

        void normalize() noexcept;

        const FormattedLogRecord *record_ = nullptr;
        const LogMessageChunk *chunk_ = nullptr;
        std::size_t offset_ = 0;
        Stage stage_ = Stage::Done;
    };

    FormattedLogRecord() noexcept = default;

    [[nodiscard]] Cursor cursor() const noexcept { return Cursor(*this); }
    [[nodiscard]] std::size_t size() const noexcept { return total_size_; }
    [[nodiscard]] const OwnedLogRecord &record() const noexcept { return *record_; }
    [[nodiscard]] bool copy_to(char *destination, std::size_t capacity) const noexcept;

private:
    friend class LogFormatter;

    const OwnedLogRecord *record_ = nullptr;
    const char *prefix_ = nullptr;
    std::size_t prefix_size_ = 0;
    std::size_t total_size_ = 0;
};

} // namespace fiber::log

#endif // FIBER_LOG_FORMATTED_LOG_RECORD_H
