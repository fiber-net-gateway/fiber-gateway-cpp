#ifndef FIBER_ASYNC_COROUTINE_FRAME_POOL_H
#define FIBER_ASYNC_COROUTINE_FRAME_POOL_H

#include <cstddef>
#include <cstdint>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/Allocator.h"

namespace fiber::async {

class CoroutineFramePool : public common::NonCopyable, public common::NonMovable {
public:
    explicit CoroutineFramePool(mem::Allocator *allocator = nullptr);
    ~CoroutineFramePool();

    void *allocate(std::size_t size);
    void deallocate(void *ptr) noexcept;

    static CoroutineFramePool *current() noexcept;
    static void set_current(CoroutineFramePool *pool) noexcept;

private:
    struct FreeNode {
        FreeNode *next = nullptr;
    };

    struct alignas(std::max_align_t) FrameHeader {
        CoroutineFramePool *pool = nullptr;
        std::uint32_t class_id = 0;
        std::uint32_t size = 0;
    };

    static_assert(sizeof(FrameHeader) % alignof(std::max_align_t) == 0);

    static constexpr std::size_t kClassCount = 7;
    static constexpr std::uint32_t kLargeClass = 0xFFFFFFFFu;
    static constexpr std::size_t kClassSizes[kClassCount] = {
        64,
        128,
        256,
        512,
        1024,
        2048,
        4096
    };

    static std::size_t select_class(std::size_t total) noexcept;
    static std::size_t class_size(std::size_t class_id) noexcept;

    void *alloc_block(std::size_t class_id);
    void free_block(std::size_t class_id, void *block) noexcept;

    mem::Allocator *allocator_ = nullptr;
    FreeNode *free_lists_[kClassCount]{};
    std::size_t in_use_ = 0;

    static thread_local CoroutineFramePool *current_;
};

class CoroutineFrameAllocScope : public common::NonCopyable, public common::NonMovable {
public:
    explicit CoroutineFrameAllocScope(CoroutineFramePool *pool) noexcept;
    ~CoroutineFrameAllocScope();

private:
    CoroutineFramePool *prev_ = nullptr;
};

} // namespace fiber::async

#endif // FIBER_ASYNC_COROUTINE_FRAME_POOL_H
