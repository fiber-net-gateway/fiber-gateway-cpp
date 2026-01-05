#include "Coroutine.h"

namespace fiber::async {

Yield::Yield(IScheduler &scheduler)
    : scheduler_(&scheduler) {
}

bool Yield::await_ready() const noexcept {
    return false;
}

void Yield::await_suspend(std::coroutine_handle<> handle) const {
    if (scheduler_) {
        scheduler_->post(handle);
    } else {
        handle.resume();
    }
}

void Yield::await_resume() const noexcept {
}

} // namespace fiber::async
