#ifndef FIBER_ASYNC_WAIT_AWAITER_H
#define FIBER_ASYNC_WAIT_AWAITER_H

#include <chrono>
#include <coroutine>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"

namespace fiber::async {

// Shared machinery for a coroutine parked on a condition that some other party
// resolves on the same event loop: the deadline timer, the retraction-safe
// resume, and the completion guard. Derived types add the queue membership and
// the predicate; everything below is what they all had to repeat.
//
// Resumes go through the loop's cancellable local defer queue, never the MPSC
// notify queue: a hard-destroyed coroutine -- a task discarded by when_any
// after its result stopped being needed, say -- tears its awaiter down while a
// resume is still queued, and an MPSC entry cannot be retracted, so the loop
// would later pop it and call into freed memory. This is also why every party
// that completes a wait must run on the awaiter's own loop.
//
// A derived type hands over a detach callback so that whatever ends the wait --
// a notification, the deadline, or the awaiter's own destructor -- leaves no
// dangling queue link behind. Detaching is required to be idempotent.
class WaitAwaiter : public common::NonCopyable, public common::NonMovable {
public:
    using DetachFn = void (*)(WaitAwaiter &) noexcept;

    // Terminal outcome, idempotent: the first result wins, later ones are
    // ignored. Safe to call on a waiter that is still queued -- it detaches.
    // Completion is irreversible: a completed waiter must never be queued or
    // parked again. A non-terminal signal may re-park with the same deadline.
    void complete(common::IoErr result) noexcept;

    [[nodiscard]] bool completed() const noexcept { return completed_; }
    [[nodiscard]] common::IoErr result() const noexcept { return result_; }

protected:
    // `pending` is what result() reads until something completes the wait; it
    // is the "still waiting" sentinel of whatever protocol the derived type
    // speaks to its queue.
    WaitAwaiter(std::chrono::steady_clock::time_point deadline, DetachFn detach,
                common::IoErr pending = common::IoErr::None) noexcept :
        deadline_(deadline), detach_(detach), result_(pending) {}

    // Derived destructors run first, so a derived type may still touch its own
    // state; this only releases what the loop holds.
    ~WaitAwaiter() {
        cancel_resume();
        cancel_timer();
    }

    [[nodiscard]] bool has_timer() const noexcept { return deadline_ != std::chrono::steady_clock::time_point::max(); }
    [[nodiscard]] bool timed_out(std::chrono::steady_clock::time_point now) const noexcept {
        return has_timer() && now >= deadline_;
    }
    [[nodiscard]] event::EventLoop *loop() const noexcept { return loop_; }

    // Park: adopt the coroutine handle, bind the loop, and start the deadline.
    // The caller links itself into its queue first, so a completion arriving
    // from inside arm_timer() finds a consistent waiter. Calling this again for
    // a further round of waiting keeps the deadline already running.
    void begin_wait(std::coroutine_handle<> handle, event::EventLoop &loop) noexcept;

    // Release everything the loop holds and forget the handle, leaving the
    // loop association cleared. Completion remains irreversible.
    void end_wait() noexcept;

    void set_result(common::IoErr result) noexcept { result_ = result; }
    void mark_completed() noexcept { completed_ = true; }

    // Non-terminal wake, for a waiter that stays queued and re-checks its own
    // condition (see QuicLocalStreamGate). Terminal outcomes use complete().
    void post_resume() noexcept;
    void cancel_resume() noexcept;
    void cancel_timer() noexcept;
    void detach() noexcept;

private:
    void arm_timer() noexcept;
    static void on_notify(WaitAwaiter *awaiter) noexcept;
    static void on_timeout(WaitAwaiter *awaiter) noexcept;

    std::chrono::steady_clock::time_point deadline_{std::chrono::steady_clock::time_point::max()};
    DetachFn detach_ = nullptr;
    event::EventLoop *loop_ = nullptr;
    std::coroutine_handle<> handle_{};
    event::EventLoop::DeferEntry resume_entry_{};
    event::EventLoop::TimerEntry timer_entry_{};
    common::IoErr result_ = common::IoErr::None;
    bool resume_posted_ = false;
    bool completed_ = false;
};

} // namespace fiber::async

#endif // FIBER_ASYNC_WAIT_AWAITER_H
