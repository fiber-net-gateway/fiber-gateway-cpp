#ifndef FIBER_BUF_POOL_H
#define FIBER_BUF_POOL_H

#include <cstddef>

#include "../NonCopyable.h"
#include "../NonMovable.h"

namespace fiber::mem {

class BufPool : public common::NonCopyable, public common::NonMovable {
public:
    explicit BufPool(size_t block_size = 4096) noexcept;
    ~BufPool() noexcept;

    void reset() noexcept;
    void *alloc(size_t size, size_t align = alignof(std::max_align_t)) noexcept;

    template<typename T>
    T *alloc(size_t n = 1) noexcept {
        size_t bytes = sizeof(T) * n;
        return static_cast<T *>(alloc(bytes, alignof(T)));
    }

private:
    struct Block {
        char *data = nullptr;
        size_t cap = 0;
        size_t used = 0;
        Block *next = nullptr;
    };

    struct LargeBlock {
        void *data = nullptr;
        LargeBlock *next = nullptr;
    };

    void *alloc_from_blocks(size_t size, size_t align) noexcept;
    void *alloc_large(size_t size, size_t align) noexcept;

    Block *allocate_block(size_t payload_cap) noexcept;

    static size_t psz;

    Block *head_ = nullptr;
    Block *current_ = nullptr;
    LargeBlock *large_head_ = nullptr;
    size_t block_size_ = 0;
};

} // namespace fiber::mem

#endif // FIBER_BUF_POOL_H
