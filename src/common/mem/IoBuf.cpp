#include "IoBuf.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <new>
#include <utility>

#include "../Assert.h"

namespace fiber::mem {

struct alignas(std::max_align_t) IoBuf::ControlBlock {
    alignas(std::atomic_ref<std::uint32_t>::required_alignment) std::uint32_t refcount = 1;
    std::size_t capacity = 0;
};

static_assert(alignof(std::max_align_t) >= std::atomic_ref<std::uint32_t>::required_alignment);

IoBuf::IoBuf(ControlBlock *control, std::uint8_t *view_begin, std::uint8_t *view_end, std::uint8_t *pos,
             std::uint8_t *last) noexcept :
    control_(control), view_begin_(view_begin), view_end_(view_end), pos_(pos), last_(last) {}

IoBuf::~IoBuf() {
    if (control_) {
        release(control_);
    }
}

IoBuf::IoBuf(const IoBuf &other) noexcept :
    control_(other.control_), view_begin_(other.view_begin_), view_end_(other.view_end_), pos_(other.pos_),
    last_(other.last_) {
    if (control_) {
        retain(control_);
    }
}

IoBuf &IoBuf::operator=(const IoBuf &other) noexcept {
    if (this == &other) {
        return *this;
    }
    ControlBlock *new_control = other.control_;
    if (new_control) {
        retain(new_control);
    }
    if (control_) {
        release(control_);
    }
    control_ = new_control;
    view_begin_ = other.view_begin_;
    view_end_ = other.view_end_;
    pos_ = other.pos_;
    last_ = other.last_;
    return *this;
}

IoBuf::IoBuf(IoBuf &&other) noexcept :
    control_(other.control_), view_begin_(other.view_begin_), view_end_(other.view_end_), pos_(other.pos_),
    last_(other.last_) {
    other.reset_handle();
}

IoBuf &IoBuf::operator=(IoBuf &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (control_) {
        release(control_);
    }
    control_ = other.control_;
    view_begin_ = other.view_begin_;
    view_end_ = other.view_end_;
    pos_ = other.pos_;
    last_ = other.last_;
    other.reset_handle();
    return *this;
}

IoBuf IoBuf::allocate(std::size_t capacity) noexcept {
    if (capacity == 0) {
        return {};
    }
    if (capacity > std::numeric_limits<std::size_t>::max() - sizeof(ControlBlock)) {
        return {};
    }

    void *raw = ::operator new(sizeof(ControlBlock) + capacity, std::nothrow);
    if (!raw) {
        return {};
    }

    auto *control = new (raw) ControlBlock{};
    control->capacity = capacity;

    std::uint8_t *begin = storage_begin(control);
    std::uint8_t *end = begin + capacity;
    return IoBuf(control, begin, end, begin, begin);
}

bool IoBuf::valid() const noexcept { return control_ != nullptr; }

IoBuf::operator bool() const noexcept { return valid(); }

std::size_t IoBuf::capacity() const noexcept { return control_ ? control_->capacity : 0; }

std::size_t IoBuf::view_size() const noexcept {
    return control_ ? static_cast<std::size_t>(view_end_ - view_begin_) : 0;
}

std::size_t IoBuf::readable() const noexcept { return control_ ? static_cast<std::size_t>(last_ - pos_) : 0; }

std::size_t IoBuf::writable() const noexcept { return control_ ? static_cast<std::size_t>(view_end_ - last_) : 0; }

std::size_t IoBuf::headroom() const noexcept { return control_ ? static_cast<std::size_t>(pos_ - view_begin_) : 0; }

std::size_t IoBuf::tailroom() const noexcept { return control_ ? static_cast<std::size_t>(view_end_ - last_) : 0; }

bool IoBuf::unique() const noexcept { return use_count() == 1; }

std::uint32_t IoBuf::use_count() const noexcept {
    if (!control_) {
        return 0;
    }
    std::atomic_ref<std::uint32_t> ref(control_->refcount);
    return ref.load(std::memory_order_relaxed);
}

std::uint8_t *IoBuf::data() noexcept { return storage_begin(control_); }

const std::uint8_t *IoBuf::data() const noexcept { return storage_begin(control_); }

std::uint8_t *IoBuf::view_begin() noexcept { return view_begin_; }

const std::uint8_t *IoBuf::view_begin() const noexcept { return view_begin_; }

std::uint8_t *IoBuf::view_end() noexcept { return view_end_; }

const std::uint8_t *IoBuf::view_end() const noexcept { return view_end_; }

std::uint8_t *IoBuf::readable_data() noexcept { return pos_; }

const std::uint8_t *IoBuf::readable_data() const noexcept { return pos_; }

std::uint8_t *IoBuf::writable_data() noexcept { return last_; }

const std::uint8_t *IoBuf::writable_data() const noexcept { return last_; }

void IoBuf::clear() noexcept {
    FIBER_ASSERT(control_ != nullptr);
    pos_ = view_begin_;
    last_ = view_begin_;
}

void IoBuf::reset() noexcept {
    FIBER_ASSERT(control_ != nullptr);
    std::uint8_t *begin = storage_begin(control_);
    std::uint8_t *end = begin + control_->capacity;
    view_begin_ = begin;
    view_end_ = end;
    pos_ = begin;
    last_ = begin;
}

void IoBuf::consume(std::size_t bytes) noexcept {
    FIBER_ASSERT(bytes <= readable());
    pos_ += bytes;
}

void IoBuf::commit(std::size_t bytes) noexcept {
    FIBER_ASSERT(bytes <= writable());
    last_ += bytes;
}

void IoBuf::swap(IoBuf &other) noexcept {
    std::swap(control_, other.control_);
    std::swap(view_begin_, other.view_begin_);
    std::swap(view_end_, other.view_end_);
    std::swap(pos_, other.pos_);
    std::swap(last_, other.last_);
}

IoBuf IoBuf::retain_slice(std::size_t offset, std::size_t len) const noexcept {
    return retain_slice_impl(offset, len, true);
}

IoBuf IoBuf::unsafe_retain_slice(std::size_t offset, std::size_t len) const noexcept {
    return retain_slice_impl(offset, len, false);
}

IoBuf IoBuf::retain_storage_slice(std::size_t offset, std::size_t len) const noexcept {
    FIBER_ASSERT(control_ != nullptr);
    FIBER_ASSERT(offset <= control_->capacity);
    FIBER_ASSERT(len <= control_->capacity - offset);

    retain(control_);
    std::uint8_t *begin = storage_begin(control_) + offset;
    std::uint8_t *end = begin + len;
    return IoBuf(control_, begin, end, begin, end);
}

bool IoBuf::same_storage(const IoBuf &other) const noexcept {
    return control_ != nullptr && control_ == other.control_;
}

bool IoBuf::try_merge_adjacent(IoBuf &&next) noexcept {
    if (control_ == nullptr || control_ != next.control_ || last_ != next.pos_) {
        return false;
    }

    IoBuf consumed = std::move(next);
    view_end_ = consumed.view_end_;
    last_ = consumed.last_;
    return true;
}

IoBuf IoBuf::retain_slice_impl(std::size_t offset, std::size_t len, bool safe) const noexcept {
    FIBER_ASSERT(control_ != nullptr);
    FIBER_ASSERT(offset <= readable());
    FIBER_ASSERT(len <= readable() - offset);

    if (safe) {
        retain(control_);
    } else {
        unsafe_retain(control_);
    }

    std::uint8_t *slice_begin = pos_ + offset;
    std::uint8_t *slice_end = slice_begin + len;
    return IoBuf(control_, slice_begin, slice_end, slice_begin, slice_end);
}

std::uint8_t *IoBuf::storage_begin(ControlBlock *control) noexcept {
    if (!control) {
        return nullptr;
    }
    return reinterpret_cast<std::uint8_t *>(control) + sizeof(ControlBlock);
}

const std::uint8_t *IoBuf::storage_begin(const ControlBlock *control) noexcept {
    if (!control) {
        return nullptr;
    }
    return reinterpret_cast<const std::uint8_t *>(control) + sizeof(ControlBlock);
}

void IoBuf::retain(ControlBlock *control) noexcept {
    FIBER_ASSERT(control != nullptr);
    std::atomic_ref<std::uint32_t> ref(control->refcount);
    std::uint32_t current = ref.load(std::memory_order_relaxed);
    for (;;) {
        FIBER_ASSERT(current > 0);
        FIBER_ASSERT(current < std::numeric_limits<std::uint32_t>::max());
        if (ref.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return;
        }
    }
}

void IoBuf::unsafe_retain(ControlBlock *control) noexcept {
    FIBER_ASSERT(control != nullptr);
    FIBER_ASSERT(control->refcount > 0);
    FIBER_ASSERT(control->refcount < std::numeric_limits<std::uint32_t>::max());
    ++control->refcount;
}

void IoBuf::release(ControlBlock *control) noexcept {
    FIBER_ASSERT(control != nullptr);
    std::atomic_ref<std::uint32_t> ref(control->refcount);
    std::uint32_t current = ref.load(std::memory_order_relaxed);
    for (;;) {
        FIBER_ASSERT(current > 0);
        std::uint32_t next = current - 1;
        if (ref.compare_exchange_weak(current, next, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            if (next == 0) {
                control->~ControlBlock();
                ::operator delete(control);
            }
            return;
        }
    }
}

void IoBuf::reset_handle() noexcept {
    control_ = nullptr;
    view_begin_ = nullptr;
    view_end_ = nullptr;
    pos_ = nullptr;
    last_ = nullptr;
}

} // namespace fiber::mem
