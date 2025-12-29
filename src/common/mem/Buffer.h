//
// Created by dear on 2025/12/29.
//

#ifndef FIBER_BUFFER_H
#define FIBER_BUFFER_H

#include <cstdlib>

namespace fiber::mem {
    class Buffer {
    public:
        Buffer();
        ~Buffer();
        [[nodiscard]] void *alloc(size_t size);
        void free(void *ptr);
        [[nodiscard]] void *realloc(void *ptr, size_t size);

        [[nodiscard]] size_t get_size() const;
        [[nodiscard]] size_t get_total() const;

    private:
        size_t total_;
        size_t size_;
    };
} // namespace fiber::common::mem

#endif // FIBER_BUFFER_H
