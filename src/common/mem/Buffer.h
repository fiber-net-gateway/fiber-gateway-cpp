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
        Buffer(const Buffer &) = delete;
        Buffer &operator=(const Buffer &) = delete;
        Buffer(Buffer &&) = delete;
        Buffer &operator=(Buffer &&) = delete;

        [[nodiscard]] void *alloc(size_t size);
        void free(void *ptr);
        [[nodiscard]] void *realloc(void *ptr, size_t size);

        void clear();
        [[nodiscard]] bool append(const char *data, size_t len);
        [[nodiscard]] bool append(char ch);
        [[nodiscard]] const char *data() const;
        [[nodiscard]] size_t size() const;

        [[nodiscard]] size_t get_size() const;
        [[nodiscard]] size_t get_total() const;

    private:
        [[nodiscard]] bool reserve(size_t desired);

        char *data_;
        size_t total_;
        size_t size_;
    };
} // namespace fiber::mem

#endif // FIBER_BUFFER_H
