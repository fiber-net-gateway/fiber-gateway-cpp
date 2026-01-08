#ifndef FIBER_EVENT_EVENT_LOOP_GROUP_H
#define FIBER_EVENT_EVENT_LOOP_GROUP_H

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <vector>

#include "../async/ThreadGroup.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "EventLoop.h"

namespace fiber::event {

class EventLoopGroup : public common::NonCopyable,
                       public common::NonMovable {
public:
    using TaskFn = EventLoop::TaskFn;

    explicit EventLoopGroup(std::size_t size);
    ~EventLoopGroup();

    void start();
    void stop();
    void join();

    std::size_t size() const noexcept {
        return loops_.size();
    }

    EventLoop &at(std::size_t index);
    const EventLoop &at(std::size_t index) const;

    static EventLoop &current();

    void post(TaskFn fn);
    void post(std::coroutine_handle<> handle);

private:
    EventLoop &select_loop();

    std::vector<std::unique_ptr<EventLoop>> loops_;
    fiber::async::ThreadGroup threads_;
    std::atomic<std::size_t> next_{0};
    static thread_local EventLoop *current_loop_;
};

} // namespace fiber::event

#endif // FIBER_EVENT_EVENT_LOOP_GROUP_H
