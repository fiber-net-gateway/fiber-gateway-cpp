#include "ThreadGroup.h"

#include "../common/Assert.h"

namespace fiber::async {

thread_local ThreadGroup::Thread *ThreadGroup::Thread::current_thread_ = nullptr;

ThreadGroup::Thread::Thread(ThreadGroup *group, std::size_t index)
    : group_(group),
      index_(index) {}

ThreadGroup::Thread &ThreadGroup::Thread::current() {
    FIBER_ASSERT(current_thread_ != nullptr);
    return *current_thread_;
}

void ThreadGroup::Thread::start(const RunFn &fn) {
    FIBER_ASSERT(fn);
    thread_ = std::jthread([this, fn](std::stop_token) {
        current_thread_ = this;
        fn(*this);
        current_thread_ = nullptr;
    });
}

void ThreadGroup::Thread::request_stop() {
    thread_.request_stop();
}

void ThreadGroup::Thread::join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

ThreadGroup::ThreadGroup(std::size_t size) {
    FIBER_ASSERT(size > 0);
    threads_.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        threads_.push_back(std::unique_ptr<Thread>(new Thread(this, i)));
    }
}

ThreadGroup::~ThreadGroup() {
    request_stop();
    join();
}

void ThreadGroup::start(RunFn fn) {
    FIBER_ASSERT(fn);
    bool expected = false;
    FIBER_ASSERT_MSG(started_.compare_exchange_strong(expected, true), "ThreadGroup already started");
    for (auto &thread : threads_) {
        thread->start(fn);
    }
}

void ThreadGroup::request_stop() {
    for (auto &thread : threads_) {
        thread->request_stop();
    }
}

void ThreadGroup::join() {
    for (auto &thread : threads_) {
        thread->join();
    }
}

ThreadGroup::Thread &ThreadGroup::at(std::size_t index) {
    FIBER_ASSERT(index < threads_.size());
    return *threads_[index];
}

const ThreadGroup::Thread &ThreadGroup::at(std::size_t index) const {
    FIBER_ASSERT(index < threads_.size());
    return *threads_[index];
}

} // namespace fiber::async
