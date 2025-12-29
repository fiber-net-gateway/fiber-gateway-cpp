//
// Created by dear on 2025/12/29.
//

#include "Buffer.h"

#include <cstring>

namespace fiber::mem {

Buffer::Buffer()
    : data_(nullptr), total_(0), size_(0) {}

Buffer::~Buffer() {
    std::free(data_);
}

void *Buffer::alloc(size_t size) {
    return std::malloc(size);
}

void Buffer::free(void *ptr) {
    std::free(ptr);
}

void *Buffer::realloc(void *ptr, size_t size) {
    return std::realloc(ptr, size);
}

void Buffer::clear() {
    size_ = 0;
}

bool Buffer::append(const char *data, size_t len) {
    if (len == 0) {
        return true;
    }
    if (!data) {
        return false;
    }
    if (!reserve(size_ + len)) {
        return false;
    }
    std::memcpy(data_ + size_, data, len);
    size_ += len;
    return true;
}

bool Buffer::append(char ch) {
    return append(&ch, 1);
}

const char *Buffer::data() const {
    return data_;
}

size_t Buffer::size() const {
    return size_;
}

size_t Buffer::get_size() const {
    return size_;
}

size_t Buffer::get_total() const {
    return total_;
}

bool Buffer::reserve(size_t desired) {
    if (desired <= total_) {
        return true;
    }
    size_t new_total = (total_ == 0) ? 64 : total_;
    while (new_total < desired) {
        new_total *= 2;
    }
    auto *new_data = static_cast<char *>(std::realloc(data_, new_total));
    if (!new_data) {
        return false;
    }
    data_ = new_data;
    total_ = new_total;
    return true;
}

} // namespace fiber::mem
