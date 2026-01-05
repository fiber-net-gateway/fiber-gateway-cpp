#ifndef FIBER_ASYNC_COROUTINE_H
#define FIBER_ASYNC_COROUTINE_H

#include <coroutine>

#include "Scheduler.h"

namespace fiber::async {

class Yield {
public:
    explicit Yield(IScheduler &scheduler);

    bool await_ready() const noexcept;
    void await_suspend(std::coroutine_handle<> handle) const;
    void await_resume() const noexcept;

private:
    IScheduler *scheduler_ = nullptr;
};

} // namespace fiber::async

#endif // FIBER_ASYNC_COROUTINE_H
