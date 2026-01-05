#ifndef FIBER_ASYNC_SCHEDULER_H
#define FIBER_ASYNC_SCHEDULER_H

#include <coroutine>

namespace fiber::async {

struct IScheduler {
    virtual void post(std::coroutine_handle<> handle) = 0;
    virtual ~IScheduler() = default;
};

} // namespace fiber::async

#endif // FIBER_ASYNC_SCHEDULER_H
