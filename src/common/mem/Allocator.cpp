//
// Created by dear on 2025/12/29.
//

#include "Allocator.h"

namespace fiber::mem {

void *Allocator::alloc(size_t size) {
    return std::malloc(size);
}

void Allocator::free(void *ptr) {
    std::free(ptr);
}

void *Allocator::realloc(void *ptr, size_t size) {
    return std::realloc(ptr, size);
}

} // namespace fiber::mem
