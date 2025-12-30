//
// Created by dear on 2025/12/29.
//

#ifndef FIBER_ALLOCATOR_H
#define FIBER_ALLOCATOR_H

#include <cstdlib>

namespace fiber::mem {
    class Allocator {
    public:
        Allocator() = default;
        ~Allocator() = default;
        Allocator(const Allocator &) = delete;
        Allocator &operator=(const Allocator &) = delete;
        Allocator(Allocator &&) = delete;
        Allocator &operator=(Allocator &&) = delete;

        [[nodiscard]] void *alloc(size_t size);
        void free(void *ptr);
        [[nodiscard]] void *realloc(void *ptr, size_t size);
    };
} // namespace fiber::mem

#endif // FIBER_ALLOCATOR_H
