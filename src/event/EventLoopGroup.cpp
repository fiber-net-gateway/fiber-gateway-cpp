#include "EventLoopGroup.h"

#include <utility>

#include "../common/Assert.h"

namespace fiber::event {

thread_local EventLoop *EventLoopGroup::current_loop_ = nullptr;

EventLoopGroup::EventLoopGroup(std::size_t size)
    : threads_(size) {
    FIBER_ASSERT(size > 0);
    loops_.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        loops_.push_back(std::make_unique<EventLoop>(this));
    }
}

EventLoopGroup::~EventLoopGroup() {
    stop();
    join();
}

void EventLoopGroup::start() {
    threads_.start([this](fiber::async::ThreadGroup::Thread &thread) {
        const auto index = thread.index();
        EventLoop &loop = *loops_[index];
        fiber::async::CoroutineFrameAllocScope alloc_scope(&loop.frame_pool());
        current_loop_ = &loop;
        loop.run();
        current_loop_ = nullptr;
    });
}

void EventLoopGroup::stop() {
    for (auto &loop : loops_) {
        loop->stop();
    }
}

void EventLoopGroup::join() {
    threads_.join();
}

EventLoop &EventLoopGroup::at(std::size_t index) {
    FIBER_ASSERT(index < loops_.size());
    return *loops_[index];
}

const EventLoop &EventLoopGroup::at(std::size_t index) const {
    FIBER_ASSERT(index < loops_.size());
    return *loops_[index];
}

EventLoop &EventLoopGroup::current() {
    FIBER_ASSERT(current_loop_ != nullptr);
    return *current_loop_;
}

void EventLoopGroup::post(TaskFn fn) {
    select_loop().post(std::move(fn));
}

void EventLoopGroup::post(std::coroutine_handle<> handle) {
    select_loop().post(handle);
}

EventLoop &EventLoopGroup::select_loop() {
    if (current_loop_) {
        return *current_loop_;
    }
    FIBER_ASSERT(!loops_.empty());
    auto index = next_.fetch_add(1, std::memory_order_relaxed);
    return *loops_[index % loops_.size()];
}

} // namespace fiber::event
