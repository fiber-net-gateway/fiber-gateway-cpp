#include "CoroutineFramePool.h"

#include "../common/Assert.h"

namespace fiber::async {

thread_local CoroutineFramePool *CoroutineFramePool::current_ = nullptr;

CoroutineFramePool::CoroutineFramePool(mem::Allocator *allocator)
    : allocator_(allocator) {
    static mem::Allocator default_allocator{};
    if (!allocator_) {
        allocator_ = &default_allocator;
    }
}

CoroutineFramePool::~CoroutineFramePool() {
    FIBER_ASSERT(in_use_ == 0);
    for (std::size_t i = 0; i < kClassCount; ++i) {
        FreeNode *node = free_lists_[i];
        while (node) {
            FreeNode *next = node->next;
            allocator_->free(node);
            node = next;
        }
        free_lists_[i] = nullptr;
    }
}

void *CoroutineFramePool::allocate(std::size_t size) {
    std::size_t total = size + sizeof(FrameHeader);
    std::size_t class_id = select_class(total);
    void *block = nullptr;
    if (class_id == kLargeClass) {
        block = allocator_->alloc(total);
    } else {
        block = alloc_block(class_id);
    }
    if (!block) {
        return nullptr;
    }
    auto *header = static_cast<FrameHeader *>(block);
    header->pool = this;
    header->class_id = static_cast<std::uint32_t>(class_id);
    header->size = static_cast<std::uint32_t>(total);
    ++in_use_;
    return reinterpret_cast<char *>(block) + sizeof(FrameHeader);
}

void CoroutineFramePool::deallocate(void *ptr) noexcept {
    if (!ptr) {
        return;
    }
    auto *block = reinterpret_cast<char *>(ptr) - sizeof(FrameHeader);
    auto *header = reinterpret_cast<FrameHeader *>(block);
#ifndef NDEBUG
    FIBER_ASSERT(header->pool == this);
    FIBER_ASSERT(current_ == this);
#endif
    std::uint32_t class_id = header->class_id;
    if (class_id == kLargeClass) {
        allocator_->free(block);
    } else {
        free_block(class_id, block);
    }
    if (in_use_ > 0) {
        --in_use_;
    }
}

CoroutineFramePool *CoroutineFramePool::current() noexcept {
    return current_;
}

void CoroutineFramePool::set_current(CoroutineFramePool *pool) noexcept {
    current_ = pool;
}

std::size_t CoroutineFramePool::select_class(std::size_t total) noexcept {
    for (std::size_t i = 0; i < kClassCount; ++i) {
        if (total <= kClassSizes[i]) {
            return i;
        }
    }
    return kLargeClass;
}

std::size_t CoroutineFramePool::class_size(std::size_t class_id) noexcept {
    if (class_id >= kClassCount) {
        return 0;
    }
    return kClassSizes[class_id];
}

void *CoroutineFramePool::alloc_block(std::size_t class_id) {
    FreeNode *node = free_lists_[class_id];
    if (node) {
        free_lists_[class_id] = node->next;
        return node;
    }
    std::size_t size = class_size(class_id);
    if (size == 0) {
        return nullptr;
    }
    return allocator_->alloc(size);
}

void CoroutineFramePool::free_block(std::size_t class_id, void *block) noexcept {
    if (class_id >= kClassCount || !block) {
        return;
    }
    auto *node = static_cast<FreeNode *>(block);
    node->next = free_lists_[class_id];
    free_lists_[class_id] = node;
}

CoroutineFrameAllocScope::CoroutineFrameAllocScope(CoroutineFramePool *pool) noexcept
    : prev_(CoroutineFramePool::current()) {
    CoroutineFramePool::set_current(pool);
}

CoroutineFrameAllocScope::~CoroutineFrameAllocScope() {
    CoroutineFramePool::set_current(prev_);
}

} // namespace fiber::async
