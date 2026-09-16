#ifndef FIBER_NET_DETAIL_RW_FD_H
#define FIBER_NET_DETAIL_RW_FD_H

#include <chrono>
#include <coroutine>
#include <cstdint>

#include "../../common/Assert.h"
#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../../event/EventLoop.h"
#include "Efd.h"

namespace fiber::net::detail {

class RWFd;

// Completion bridge shared by the local-thread wait awaiters. A waiter is
// installed as an ordinary direction subscription (`on_event` + this ctx), so
// it obeys the same subscription identity and close rules as business
// callbacks; close() completes it without touching the RWFd again.
struct RWFdWaiterBase {
    using CompleteCallback = void (*)(RWFdWaiterBase *waiter, fiber::common::IoErr err) noexcept;

    static void on_event(void *ctx, fiber::common::IoErr err) noexcept;

    RWFd *rwfd_ = nullptr;
    fiber::event::IoEvent event_{fiber::event::IoEvent::None};
    CompleteCallback complete_callback_ = nullptr;
    std::coroutine_handle<> coro_ = nullptr;
};

// Per-direction readiness and its (at most one) subscription.
//
// State transitions:
//   Unknown/Blocked -> Ready : kernel ready hint; notifies a subscriber once.
//   Ready -> Ready          : never notifies.
//   Ready -> Blocked/...    : only via the I/O state feedback of read/write.
// EOF, half-close and fatal errors are NOT tracked here; the adapter layer
// (StreamFd) owns them and receives the raw kernel stream bits through the
// stream event sink instead.
class RWFd : public common::NonCopyable, public common::NonMovable {
public:
    using ReadyCallback = void (*)(void *ctx, fiber::common::IoErr err) noexcept;
    // Raw ReadHangup/Terminal kernel bits this layer does not interpret.
    using StreamEventCallback = void (*)(void *ctx, fiber::event::IoEvent events) noexcept;
    // Adapter-layer wait veto, consulted before suspending a waiter: true means
    // proceed without waiting (like an observed Ready), false means continue
    // with the ordinary readiness wait, an error completes the wait with it.
    using StreamWaitGate = fiber::common::IoResult<bool> (*)(void *ctx, fiber::event::IoEvent direction) noexcept;

    // Direction subscriptions detached by detach_for_close(), completed after
    // the adapter layer ran its own terminal completion. complete() only uses
    // the stored locals: a preceding completion may have destroyed the RWFd.
    struct DetachedCompletions {
        ReadyCallback read_callback = nullptr;
        void *read_ctx = nullptr;
        ReadyCallback write_callback = nullptr;
        void *write_ctx = nullptr;

        void complete(fiber::common::IoErr err) const noexcept;
    };

    enum class State : std::uint8_t { Unknown, Ready, Blocked };
    enum class Kind : std::uint8_t { Raw, Stream, Datagram };

    template<fiber::event::IoEvent Event>
    class WaitAwaiter;

    using WaitReadableAwaiter = WaitAwaiter<fiber::event::IoEvent::Read>;
    using WaitWritableAwaiter = WaitAwaiter<fiber::event::IoEvent::Write>;

    explicit RWFd(fiber::event::EventLoop &owner_loop, Kind kind = Kind::Raw);
    RWFd(fiber::event::EventLoop &owner_loop, int fd, Kind kind = Kind::Raw);
    ~RWFd();

    // ------------------------------------------------------------------
    // Synchronous I/O wrappers. They assert the current loop, always run the
    // lambda exactly once (never suppressed by readiness, never registered,
    // never waiting) and let the lambda feed the direction state back through
    // IoStateUpdate. Error/EOF interpretation stays in the lambda's layer.
    // ------------------------------------------------------------------
    template<typename F>
    [[nodiscard]] auto read(F io) noexcept(noexcept(io(std::declval<IoStateUpdate &>()))) {
        FIBER_ASSERT(current_loop().in_loop());
        IoStateUpdate update{read_event_.state};
        return io(update);
    }
    template<typename F>
    [[nodiscard]] auto write(F io) noexcept(noexcept(io(std::declval<IoStateUpdate &>()))) {
        FIBER_ASSERT(current_loop().in_loop());
        IoStateUpdate update{write_event_.state};
        return io(update);
    }

    // Inline, allocation-free state feedback for read/write lambdas.
    class IoStateUpdate {
    public:
        void mark_ready() noexcept { state_ = State::Ready; }
        void mark_blocked() noexcept { state_ = State::Blocked; }

    private:
        friend class RWFd;
        explicit IoStateUpdate(State &state) noexcept : state_(state) {}
        State &state_;
    };

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] fiber::event::EventLoop &owner_loop() const noexcept;
    [[nodiscard]] fiber::event::EventLoop &current_loop() const noexcept;
    // Current-loop alias for adapters that never hand over.
    [[nodiscard]] fiber::event::EventLoop &loop() const noexcept;
    [[nodiscard]] Kind kind() const noexcept { return kind_; }

    [[nodiscard]] State read_state() const noexcept { return read_event_.state; }
    [[nodiscard]] State write_state() const noexcept { return write_event_.state; }
    // True while the fd is registered with the current loop's poller.
    [[nodiscard]] bool registered() const noexcept { return efd_.registered(); }

    fiber::common::IoErr attach(int fd) noexcept;
    int release_fd() noexcept;
    void close();
    // close() split in two so an adapter layer can run its own terminal
    // completion between detaching and completing the direction subscriptions.
    [[nodiscard]] DetachedCompletions detach_for_close() noexcept;

    // Installs the adapter-layer sink that receives raw ReadHangup/Terminal
    // kernel bits (stream termination semantics live above this class), and
    // the gate consulted by the wait awaiters (known EOF/terminal must not
    // wait for a further edge).
    void set_stream_event_sink(void *ctx, StreamEventCallback callback) noexcept;
    void set_stream_wait_gate(void *ctx, StreamWaitGate gate) noexcept;
    // Ensures the fd listens for stream-state bits (RDHUP; ERR/HUP arrive with
    // any registration). Used by adapter/pool state observation, not by the
    // ordinary read/write subscriptions.
    fiber::common::IoErr ensure_state_observation() noexcept;

    // Subscriptions are persistent, transition-only (Unknown/Blocked -> Ready)
    // and require the direction not to be Ready: a Ready caller must advance
    // by doing I/O, not by waiting for another edge. Installing ensures the
    // direction is listened for (first demand ADDs, later demands extend by
    // MOD). Setters never invoke callbacks inline and never queue a ready
    // notification. Clearing removes only a matching callback/ctx pair and
    // keeps both readiness and kernel interest.
    fiber::common::IoErr set_read_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr set_write_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr clear_read_callback(ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr clear_write_callback(ReadyCallback callback, void *ctx) noexcept;

    // Handover, see Efd. detach requires no remaining subscriptions on the
    // current loop; adopt resets both direction states to Unknown and is the
    // only point where readiness history is dropped. Adoption also installs
    // the new loop's stop hook (lifecycle takeover is immediate even though
    // the epoll ADD stays lazy); it fails and closes the fd when the target
    // loop is already stopping.
    fiber::common::IoErr detach_for_handover() noexcept;
    fiber::common::IoErr adopt_loop(fiber::event::EventLoop &loop) noexcept;

    [[nodiscard]] WaitReadableAwaiter
    wait_readable(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] WaitWritableAwaiter
    wait_writable(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;

private:
    friend struct RWFdWaiterBase;

    template<fiber::event::IoEvent Event>
    friend class WaitAwaiter;

    class DispatchGuard {
    public:
        explicit DispatchGuard(RWFd &owner) noexcept;
        ~DispatchGuard() noexcept;

        DispatchGuard(const DispatchGuard &) = delete;
        DispatchGuard &operator=(const DispatchGuard &) = delete;
        DispatchGuard(DispatchGuard &&) = delete;
        DispatchGuard &operator=(DispatchGuard &&) = delete;

        [[nodiscard]] bool owner_destroyed() const noexcept { return owner_destroyed_; }

    private:
        RWFd *owner_ = nullptr;
        bool owner_destroyed_ = false;
    };

    struct Event {
        ReadyCallback callback = nullptr;
        void *ctx = nullptr;
        State state = State::Unknown;
    };

    fiber::common::IoErr install_callback(Event &event, std::uint64_t &generation, fiber::event::IoEvent direction,
                                          ReadyCallback callback, void *ctx) noexcept;
    bool remove_callback(Event &event, std::uint64_t &generation, fiber::event::IoEvent direction,
                         ReadyCallback callback, void *ctx) noexcept;
    fiber::common::IoErr ensure_listening(fiber::event::IoEvent events) noexcept;
    fiber::common::IoErr begin_wait(RWFdWaiterBase *waiter) noexcept;
    void cancel_wait(RWFdWaiterBase *waiter) noexcept;
    [[nodiscard]] bool has_callbacks() const noexcept;

    static void on_efd_events(void *sink, fiber::event::IoEvent events);
    static void on_loop_stop(RWFd *owner) noexcept;
    void handle_events(fiber::event::IoEvent events);

    Event read_event_{};
    Event write_event_{};
    std::uint64_t read_generation_ = 0;
    std::uint64_t write_generation_ = 0;
    // Destroyed-observer flag of the active dispatch guard, see DispatchGuard.
    bool *dispatch_destroyed_observer_ = nullptr;
    const Kind kind_;
    void *stream_sink_ctx_ = nullptr;
    StreamEventCallback stream_sink_ = nullptr;
    void *stream_gate_ctx_ = nullptr;
    StreamWaitGate stream_gate_ = nullptr;
    event::EventLoop::StopEntry stop_entry_{};

    Efd efd_;
};

// Local awaiter: waits, times out, cancels and resumes entirely on the fd's
// current loop. Cross-loop use must hand the fd over first; await_suspend
// asserts the calling thread is the current loop.
template<fiber::event::IoEvent Event>
class RWFd::WaitAwaiter : public RWFdWaiterBase {
public:
    explicit WaitAwaiter(RWFd &rwfd, std::chrono::milliseconds timeout) noexcept : timeout_(timeout) {
        static_assert(Event == fiber::event::IoEvent::Read || Event == fiber::event::IoEvent::Write);
        rwfd_ = &rwfd;
        event_ = Event;
        complete_callback_ = &WaitAwaiter::on_complete;
    }

    WaitAwaiter(const WaitAwaiter &) = delete;
    WaitAwaiter &operator=(const WaitAwaiter &) = delete;
    WaitAwaiter(WaitAwaiter &&) = delete;
    WaitAwaiter &operator=(WaitAwaiter &&) = delete;

    ~WaitAwaiter() {
        cancel_timer();
        if (!waiting_) {
            return;
        }
        FIBER_ASSERT(loop_ != nullptr);
        FIBER_ASSERT(loop_->in_loop());
        waiting_ = false;
        rwfd_->cancel_wait(this);
    }

    bool await_ready() noexcept {
        if (timeout_ > std::chrono::milliseconds::zero()) {
            return false;
        }
        err_ = fiber::common::IoErr::TimedOut;
        return true;
    }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
        FIBER_ASSERT(rwfd_->current_loop().in_loop());
        loop_ = &rwfd_->current_loop();
        coro_ = handle;

        // Adapter-layer states the raw readiness must not override: a known
        // EOF completes a read wait immediately, a terminal stream completes
        // both directions with its recorded error.
        if (rwfd_->stream_gate_ != nullptr) {
            const auto gated = rwfd_->stream_gate_(rwfd_->stream_gate_ctx_, Event);
            if (!gated) {
                err_ = gated.error();
                return false;
            }
            if (*gated) {
                err_ = fiber::common::IoErr::None;
                return false;
            }
        }

        const auto state = Event == fiber::event::IoEvent::Read ? rwfd_->read_state() : rwfd_->write_state();
        if (state == State::Ready) {
            // Ready: try I/O instead of waiting for another edge.
            err_ = fiber::common::IoErr::None;
            return false;
        }

        const auto installed = rwfd_->begin_wait(this);
        if (installed != fiber::common::IoErr::None) {
            err_ = installed;
            return false;
        }
        waiting_ = true;
        arm_timer();
        return true;
    }

    fiber::common::IoResult<void> await_resume() noexcept {
        waiting_ = false;
        cancel_timer();
        if (err_ == fiber::common::IoErr::None) {
            return {};
        }
        return std::unexpected(err_);
    }

private:
    void arm_timer() noexcept {
        if (timeout_ == std::chrono::milliseconds::max()) {
            return;
        }
        loop_->post_at<WaitAwaiter, &WaitAwaiter::timer_entry_, &WaitAwaiter::on_timeout>(loop_->now() + timeout_,
                                                                                          *this);
    }

    void cancel_timer() noexcept {
        if (!timer_entry_.is_in_heap()) {
            return;
        }
        FIBER_ASSERT(loop_ != nullptr);
        FIBER_ASSERT(loop_->in_loop());
        loop_->cancel<WaitAwaiter, &WaitAwaiter::timer_entry_>(*this);
    }

    static void on_complete(RWFdWaiterBase *base, fiber::common::IoErr err) noexcept {
        auto *awaiter = static_cast<WaitAwaiter *>(base);
        awaiter->err_ = err;
        awaiter->waiting_ = false;
        awaiter->cancel_timer();
        awaiter->coro_.resume();
    }

    static void on_timeout(WaitAwaiter *awaiter) noexcept {
        FIBER_ASSERT(awaiter != nullptr);
        FIBER_ASSERT(awaiter->waiting_);
        awaiter->waiting_ = false;
        awaiter->err_ = fiber::common::IoErr::TimedOut;
        awaiter->rwfd_->cancel_wait(awaiter);
        awaiter->coro_.resume();
    }

    std::chrono::milliseconds timeout_{};
    fiber::event::EventLoop *loop_ = nullptr;
    fiber::event::EventLoop::TimerEntry timer_entry_{};
    fiber::common::IoErr err_ = fiber::common::IoErr::None;
    bool waiting_ = false;
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_RW_FD_H
