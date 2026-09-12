#ifndef FIBER_ASYNC_LOCAL_WAIT_GROUP_H
#define FIBER_ASYNC_LOCAL_WAIT_GROUP_H

#include <coroutine>
#include <cstddef>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "WaitAwaiter.h"

namespace fiber::async {

// Loop-local counterpart of WaitGroup: a counter plus the coroutines parked on
// it reaching zero. No mutex, no allocation -- waiters are intrusive
// WaitAwaiters that live in the awaiting coroutine's frame, and a completed
// join resumes through the loop's cancellable local defer queue.
//
// add() and done() with no waiter parked only touch the counter, so a
// quiescent off-loop owner may still call them; whatever completes a parked
// join must run on the joining coroutine's loop.
class LocalWaitGroup : public common::NonCopyable, public common::NonMovable {
public:
    class JoinAwaiter : public WaitAwaiter {
    public:
        explicit JoinAwaiter(LocalWaitGroup &group) noexcept :
            WaitAwaiter(std::chrono::steady_clock::time_point::max(), &JoinAwaiter::detach_from_group,
                        common::IoErr::WouldBlock),
            group_(&group) {}
        ~JoinAwaiter() { detach(); }

        bool await_ready() noexcept;
        bool await_suspend(std::coroutine_handle<> handle) noexcept;
        void await_resume() noexcept;

    private:
        friend class LocalWaitGroup;
        static void detach_from_group(WaitAwaiter &base) noexcept;

        LocalWaitGroup *group_ = nullptr;
        JoinAwaiter *prev_ = nullptr;
        JoinAwaiter *next_ = nullptr;
        bool linked_ = false;
    };

    LocalWaitGroup() noexcept = default;
    // A parked waiter outliving its group is a lifetime bug; asserted.
    ~LocalWaitGroup();

    void add(std::size_t n = 1) noexcept;
    // Completes every parked join when the count reaches zero.
    void done() noexcept;
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] std::size_t count() const noexcept { return count_; }
    [[nodiscard]] bool has_waiters() const noexcept { return head_ != nullptr; }
    // Suspends until the count is zero; ready immediately when it already is.
    [[nodiscard]] JoinAwaiter join() noexcept { return JoinAwaiter(*this); }

private:
    void link(JoinAwaiter &waiter) noexcept;
    void unlink(JoinAwaiter &waiter) noexcept;

    std::size_t count_ = 0;
    JoinAwaiter *head_ = nullptr;
    JoinAwaiter *tail_ = nullptr;
};

} // namespace fiber::async

#endif // FIBER_ASYNC_LOCAL_WAIT_GROUP_H
