#ifndef FIBER_ASYNC_THREAD_GROUP_H
#define FIBER_ASYNC_THREAD_GROUP_H

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <stop_token>
#include <thread>
#include <vector>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"

namespace fiber::async {

class ThreadGroup : public common::NonCopyable, public common::NonMovable {
public:
    class Thread : public common::NonCopyable, public common::NonMovable {
    public:
        using RunFn = std::function<void(Thread &)>;

        std::size_t index() const noexcept {
            return index_;
        }

        ThreadGroup &group() const noexcept {
            return *group_;
        }

        std::stop_token stop_token() const noexcept {
            return thread_.get_stop_token();
        }

        static Thread &current();

    private:
        friend class ThreadGroup;

        Thread(ThreadGroup *group, std::size_t index);
        void start(const RunFn &fn);
        void request_stop();
        void join();

        ThreadGroup *group_ = nullptr;
        std::size_t index_ = 0;
        std::jthread thread_{};
        static thread_local Thread *current_thread_;
    };

    using RunFn = Thread::RunFn;

    explicit ThreadGroup(std::size_t size);
    ~ThreadGroup();

    void start(RunFn fn);
    void request_stop();
    void join();

    std::size_t size() const noexcept {
        return threads_.size();
    }

    Thread &at(std::size_t index);
    const Thread &at(std::size_t index) const;

private:
    std::vector<std::unique_ptr<Thread>> threads_;
    std::atomic<bool> started_{false};
};

} // namespace fiber::async

#endif // FIBER_ASYNC_THREAD_GROUP_H
