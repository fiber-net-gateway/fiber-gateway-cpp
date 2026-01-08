#ifndef FIBER_ASYNC_COROUTINE_PROMISE_BASE_H
#define FIBER_ASYNC_COROUTINE_PROMISE_BASE_H

#include <cstddef>
#include <new>

#include "../common/Assert.h"
#include "CoroutineFramePool.h"

namespace fiber::async {

class CoroutinePromiseBase {
public:
    static void *operator new(std::size_t size) {
        CoroutineFramePool *pool = CoroutineFramePool::current();
        FIBER_ASSERT(pool != nullptr);
        void *ptr = pool->allocate(size);
        if (!ptr) {
            throw std::bad_alloc();
        }
        return ptr;
    }

    static void operator delete(void *ptr) noexcept {
        if (!ptr) {
            return;
        }
        CoroutineFramePool *pool = CoroutineFramePool::current();
        FIBER_ASSERT(pool != nullptr);
        pool->deallocate(ptr);
    }

    static void operator delete(void *ptr, std::size_t) noexcept {
        operator delete(ptr);
    }
};

} // namespace fiber::async

#endif // FIBER_ASYNC_COROUTINE_PROMISE_BASE_H
