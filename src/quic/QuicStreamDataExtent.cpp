#include "QuicStreamDataExtent.h"

#include <new>

namespace fiber::quic {

QuicStreamDataExtentPool::~QuicStreamDataExtentPool() { clear(); }

QuicStreamDataExtent *QuicStreamDataExtentPool::alloc() noexcept {
    if (free_head_ != nullptr) {
        QuicStreamDataExtent *extent = free_head_;
        free_head_ = extent->next;
        --cached_count_;
        extent->start = 0;
        extent->end = 0;
        extent->block_index = 0;
        extent->state = 0;
        extent->view = {};
        extent->next = nullptr;
        return extent;
    }
    return new (std::nothrow) QuicStreamDataExtent{};
}

void QuicStreamDataExtentPool::release(QuicStreamDataExtent *extent) noexcept {
    if (extent == nullptr) {
        return;
    }

    extent->start = 0;
    extent->end = 0;
    extent->block_index = 0;
    extent->state = 0;
    extent->view = {};
    if (cached_count_ < kQuicStreamDataExtentPoolMaxCached) {
        extent->next = free_head_;
        free_head_ = extent;
        ++cached_count_;
        return;
    }

    delete extent;
}

void QuicStreamDataExtentPool::clear() noexcept {
    QuicStreamDataExtent *extent = free_head_;
    while (extent != nullptr) {
        QuicStreamDataExtent *next = extent->next;
        delete extent;
        extent = next;
    }
    free_head_ = nullptr;
    cached_count_ = 0;
}

} // namespace fiber::quic
