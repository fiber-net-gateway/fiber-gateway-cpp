#ifndef FIBER_TEST_TEST_HELPERS_H
#define FIBER_TEST_TEST_HELPERS_H

#include <type_traits>
#include <utility>

#include "event/EventLoop.h"

namespace fiber::test {

template <typename F>
struct DeferTask {
    fiber::event::EventLoop::DeferEntry entry{};
    F fn;

    explicit DeferTask(F &&func) : fn(std::forward<F>(func)) {}

    static void run(DeferTask *task) {
        task->fn();
        delete task;
    }

    static void cancel(DeferTask *task) {
        delete task;
    }
};

template <typename F>
void post_task(fiber::event::EventLoop &loop, F &&fn) {
    using Task = DeferTask<std::decay_t<F>>;
    auto *task = new Task(std::forward<F>(fn));
    loop.post<Task, &Task::entry, &Task::run, &Task::cancel>(*task);
}

} // namespace fiber::test

#endif // FIBER_TEST_TEST_HELPERS_H
