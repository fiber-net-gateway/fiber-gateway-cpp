#include "LogRecord.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

namespace fiber::log {
namespace {

constexpr std::size_t kMaxChunkCapacity = 256 * 1024;

} // namespace

OwnedLogRecord *OwnedLogRecord::create(std::string_view logger_name, const AppenderId *targets,
                                       std::uint32_t target_count, LogLevel level, const char *file, std::uint32_t line,
                                       const char *function, std::uint64_t timestamp_us,
                                       std::uint32_t thread_id) noexcept {
    return new (std::nothrow)
            OwnedLogRecord(logger_name, targets, target_count, level, file, line, function, timestamp_us, thread_id);
}

OwnedLogRecord::OwnedLogRecord(std::string_view logger_name, const AppenderId *targets, std::uint32_t target_count,
                               LogLevel level, const char *file, std::uint32_t line, const char *function,
                               std::uint64_t timestamp_us, std::uint32_t thread_id) noexcept :
    logger_name_(logger_name), targets_(targets), target_count_(target_count), level_(level), file_(file),
    function_(function), line_(line), timestamp_us_(timestamp_us), thread_id_(thread_id) {}

OwnedLogRecord::~OwnedLogRecord() {
    LogMessageChunk *chunk = first_chunk_;
    while (chunk) {
        LogMessageChunk *next = chunk->next;
        std::free(chunk);
        chunk = next;
    }
}

bool OwnedLogRecord::allocate_chunk() noexcept {
    constexpr std::size_t header_size = offsetof(LogMessageChunk, data);
    if (next_chunk_capacity_ > std::numeric_limits<std::size_t>::max() - header_size ||
        allocated_bytes_ > std::numeric_limits<std::size_t>::max() - header_size - next_chunk_capacity_) {
        failed_ = true;
        return false;
    }

    const std::size_t allocation_size = header_size + next_chunk_capacity_;
    auto *chunk = static_cast<LogMessageChunk *>(std::malloc(allocation_size));
    if (!chunk) {
        failed_ = true;
        return false;
    }
    chunk->next = nullptr;
    chunk->size = 0;
    chunk->capacity = next_chunk_capacity_;
    if (last_chunk_) {
        last_chunk_->next = chunk;
    } else {
        first_chunk_ = chunk;
    }
    last_chunk_ = chunk;
    allocated_bytes_ += allocation_size;
    if (next_chunk_capacity_ < kMaxChunkCapacity) {
        next_chunk_capacity_ = std::min(kMaxChunkCapacity, next_chunk_capacity_ * 2);
    }
    return true;
}

bool OwnedLogRecord::append(std::string_view value) noexcept {
    if (failed_ || value.empty()) {
        return !failed_;
    }
    if (message_size_ > std::numeric_limits<std::size_t>::max() - value.size()) {
        failed_ = true;
        return false;
    }

    const char *current = value.data();
    std::size_t remaining = value.size();
    if (inline_message_size_ < kInlineMessageCapacity) {
        const std::size_t copy = std::min(remaining, kInlineMessageCapacity - inline_message_size_);
        std::memcpy(inline_message_ + inline_message_size_, current, copy);
        inline_message_size_ += copy;
        current += copy;
        remaining -= copy;
    }

    while (remaining > 0) {
        if (!last_chunk_ || last_chunk_->size == last_chunk_->capacity) {
            if (!allocate_chunk()) {
                return false;
            }
        }
        const std::size_t copy = std::min(remaining, last_chunk_->capacity - last_chunk_->size);
        std::memcpy(last_chunk_->data + last_chunk_->size, current, copy);
        last_chunk_->size += copy;
        current += copy;
        remaining -= copy;
    }
    message_size_ += value.size();
    return true;
}

} // namespace fiber::log
