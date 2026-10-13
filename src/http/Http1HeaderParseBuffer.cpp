#include <fiber/http/Http1HeaderParseBuffer.h>

#include <cstring>
#include <limits>

namespace fiber::http {

Http1HeaderParseBuffer::Http1HeaderParseBuffer(Http1HeaderParseBufferOptions options) noexcept : options_(options) {}

common::IoResult<void> Http1HeaderParseBuffer::ensure_init() noexcept {
    if (buf_) {
        return {};
    }

    std::size_t capacity = next_capacity();
    if (capacity == 0) {
        return std::unexpected(common::IoErr::NoMem);
    }

    buf_ = mem::IoBuf::allocate(capacity);
    if (!buf_) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return {};
}

common::IoResult<void> Http1HeaderParseBuffer::grow() noexcept {
    if (!buf_) {
        return std::unexpected(common::IoErr::Invalid);
    }

    auto next_result = allocate_next();
    if (!next_result) {
        return std::unexpected(next_result.error());
    }

    mem::IoBuf old = buf_;
    mem::IoBuf next = std::move(*next_result);
    std::size_t readable = old.readable();
    if (readable > 0) {
        std::memcpy(next.writable_data(), old.readable_data(), readable);
        next.commit(readable);
        next.consume(readable);
    }

    buf_ = std::move(next);
    ++growth_count_;
    clear_view_state();
    return {};
}

void Http1HeaderParseBuffer::reset() noexcept {
    buf_ = {};
    growth_count_ = 0;
    clear_view_state();
}

common::IoResult<void> Http1HeaderParseBuffer::pin_viewed_storage(mem::IoBufChain &owner) noexcept {
    if (!has_views_ || pinned_) {
        return {};
    }
    // Whole-storage refcount handle: the view range is irrelevant, the owner
    // chain only keeps the storage alive for views taken into it.
    mem::IoBuf pin = buf_;
    if (!owner.append(std::move(pin))) {
        return std::unexpected(common::IoErr::NoMem);
    }
    pinned_ = true;
    return {};
}

void Http1HeaderParseBuffer::clear_view_state() noexcept {
    has_views_ = false;
    pinned_ = false;
}

bool Http1HeaderParseBuffer::can_grow() const noexcept { return next_capacity() != 0; }

common::IoResult<mem::IoBuf> Http1HeaderParseBuffer::retain_suffix() const noexcept {
    if (!buf_) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (buf_.readable() == 0) {
        return mem::IoBuf{};
    }
    mem::IoBuf suffix = buf_.retain_slice(0, buf_.readable());
    if (!suffix) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return suffix;
}

std::size_t Http1HeaderParseBuffer::next_capacity() const noexcept {
    if (!buf_) {
        return options_.init_size;
    }
    if (growth_count_ >= options_.large_num) {
        return 0;
    }
    if (options_.large_size > std::numeric_limits<std::size_t>::max() - buf_.capacity()) {
        return 0;
    }
    return buf_.capacity() + options_.large_size;
}

common::IoResult<mem::IoBuf> Http1HeaderParseBuffer::allocate_next() const noexcept {
    std::size_t capacity = next_capacity();
    if (capacity == 0) {
        return std::unexpected(common::IoErr::NoMem);
    }
    mem::IoBuf next = mem::IoBuf::allocate(capacity);
    if (!next) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return next;
}

} // namespace fiber::http
