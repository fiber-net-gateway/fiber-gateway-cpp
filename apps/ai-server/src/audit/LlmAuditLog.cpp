#include "LlmAuditLog.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace fiber::ai_server {

LlmAuditBuffer::LlmAuditBuffer(std::size_t max_bytes) noexcept : max_bytes_(max_bytes) {}

LlmAuditBuffer::~LlmAuditBuffer() { clear(); }

bool LlmAuditBuffer::append(const char *data, std::size_t size) noexcept {
    if (failed_ || (data == nullptr && size != 0) || size_ > max_bytes_ || size > max_bytes_ - size_) {
        failed_ = true;
        return false;
    }
    std::size_t offset = 0;
    while (offset < size) {
        auto allocate_chunk = [&]() noexcept -> bool {
            const std::size_t remaining = max_bytes_ - size_;
            const std::size_t requested = std::max<std::size_t>(4096, std::min(kChunkBytes, size - offset));
            const std::size_t capacity = std::min(requested, remaining);
            auto *chunk = new (std::nothrow) Chunk;
            if (!chunk) {
                failed_ = true;
                return false;
            }
            chunk->bytes = mem::IoBuf::allocate(capacity);
            if (!chunk->bytes) {
                delete chunk;
                failed_ = true;
                return false;
            }
            if (tail_) {
                tail_->next = chunk;
            } else {
                head_ = chunk;
            }
            tail_ = chunk;
            return true;
        };
        if ((tail_ == nullptr || tail_->bytes.writable() == 0) && !allocate_chunk()) {
            return false;
        }
        std::size_t copy = std::min(size - offset, tail_->bytes.writable());
        if (copy < size - offset) {
            while (copy > 0 && (static_cast<unsigned char>(data[offset + copy]) & 0xc0) == 0x80) {
                --copy;
            }
            if (copy == 0) {
                if (!allocate_chunk()) {
                    return false;
                }
                continue;
            }
        }
        std::memcpy(tail_->bytes.writable_data(), data + offset, copy);
        tail_->bytes.commit(copy);
        size_ += copy;
        offset += copy;
    }
    return true;
}

bool LlmAuditBuffer::assign(std::string_view value) noexcept {
    clear();
    failed_ = false;
    return append(value);
}

void LlmAuditBuffer::clear() noexcept {
    Chunk *chunk = head_;
    while (chunk) {
        Chunk *next = chunk->next;
        delete chunk;
        chunk = next;
    }
    head_ = nullptr;
    tail_ = nullptr;
    size_ = 0;
}

LlmAuditBuffer::Cursor LlmAuditBuffer::cursor() const noexcept { return Cursor{.next = head_}; }

bool LlmAuditBuffer::next_chunk(void *context, const char *&data, std::size_t &size, bool &done) noexcept {
    auto &cursor = *static_cast<Cursor *>(context);
    if (!cursor.next) {
        data = nullptr;
        size = 0;
        done = true;
        return true;
    }
    const Chunk *chunk = cursor.next;
    cursor.next = chunk->next;
    data = reinterpret_cast<const char *>(chunk->bytes.readable_data());
    size = chunk->bytes.readable();
    done = cursor.next == nullptr;
    return true;
}

} // namespace fiber::ai_server
