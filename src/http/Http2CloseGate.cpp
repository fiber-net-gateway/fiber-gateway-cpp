#include <fiber/http/Http2CloseGate.h>

#include <coroutine>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>

namespace fiber::http {

class Http2CloseGate::Joiner {
public:
    explicit Joiner(Http2CloseGate &gate) noexcept : gate_(&gate), loop_(&event::EventLoop::current()) {
        gate_->link_joiner(*this);
    }

    Joiner(const Joiner &) = delete;
    Joiner &operator=(const Joiner &) = delete;
    Joiner(Joiner &&) = delete;
    Joiner &operator=(Joiner &&) = delete;

    ~Joiner() {
        if (resume_posted_) {
            loop_->cancel<Joiner, &Joiner::notify_entry_>(*this);
            resume_posted_ = false;
        }
        if (linked_) {
            gate_->unlink_joiner(*this);
        }
    }

    [[nodiscard]] bool await_ready() const noexcept { return completed_; }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
        handle_ = handle;
        return true;
    }

    [[nodiscard]] common::IoErr await_resume() const noexcept { return result_; }

    // The caller unlinks first: a completed joiner is no longer part of the gate.
    void complete(common::IoErr reason) noexcept {
        if (completed_) {
            return;
        }
        completed_ = true;
        result_ = reason;
        if (resume_posted_) {
            return;
        }
        resume_posted_ = true;
        loop_->post_local<Joiner, &Joiner::notify_entry_, &Joiner::on_notify>(*this);
    }

private:
    static void on_notify(Joiner *joiner) noexcept {
        FIBER_ASSERT(joiner != nullptr);
        joiner->resume_posted_ = false;
        std::coroutine_handle<> handle = std::exchange(joiner->handle_, {});
        if (handle) {
            handle.resume();
        }
    }

    Http2CloseGate *gate_ = nullptr;
    event::EventLoop *loop_ = nullptr;
    std::coroutine_handle<> handle_{};
    event::EventLoop::DeferEntry notify_entry_{};
    common::IoErr result_ = common::IoErr::None;
    bool completed_ = false;
    bool resume_posted_ = false;

public:
    Joiner *prev_ = nullptr;
    Joiner *next_ = nullptr;
    bool linked_ = false;
};

Http2CloseGate::ObserverHook::~ObserverHook() {
    if (linked) {
        FIBER_ASSERT(gate != nullptr);
        gate->remove_observer(*this);
    }
}

Http2CloseGate::Http2CloseGate(event::EventLoop &loop, Http2Connection &connection) noexcept :
    loop_(&loop), connection_(&connection) {
    if (connection.state() == Http2Connection::State::Closed) {
        on_connection_closed();
    }
}

Http2CloseGate::~Http2CloseGate() {
    FIBER_ASSERT(phase_ != Phase::Dispatching);
    if (phase_ == Phase::Pending) {
        loop_->cancel<Http2CloseGate, &Http2CloseGate::completion_entry_>(*this);
    }
    while (joiner_head_ != nullptr) {
        Joiner *joiner = joiner_head_;
        unlink_joiner(*joiner);
        joiner->complete(common::IoErr::Canceled);
    }
    while (observer_head_ != nullptr) {
        remove_observer(*observer_head_);
    }
}

void Http2CloseGate::on_connection_closed() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(connection_->state() == Http2Connection::State::Closed);
    if (phase_ != Phase::Open) {
        return;
    }
    terminal_error_ = connection_->terminal_error();
    phase_ = Phase::Pending;
    loop_->post_local<Http2CloseGate, &Http2CloseGate::completion_entry_, &Http2CloseGate::on_completion>(*this);
}

void Http2CloseGate::on_completion(Http2CloseGate *gate) noexcept { gate->dispatch(); }

bool Http2CloseGate::closed() const noexcept { return phase_ == Phase::Complete; }

common::IoErr Http2CloseGate::terminal_error() const noexcept {
    return phase_ == Phase::Open ? connection_->terminal_error() : terminal_error_;
}

fiber::async::Task<Http2CloseGate::CloseResult> Http2CloseGate::join() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (closed()) {
        const common::IoErr reason = terminal_error();
        co_return reason == common::IoErr::None ? CloseResult{} : CloseResult(std::unexpected(reason));
    }

    Joiner joiner(*this);
    const common::IoErr reason = co_await joiner;
    co_return reason == common::IoErr::None ? CloseResult{} : CloseResult(std::unexpected(reason));
}

void Http2CloseGate::add_observer(ObserverHook &hook, ObserverCallback callback, void *ctx) noexcept {
    FIBER_ASSERT(phase_ == Phase::Open || phase_ == Phase::Pending);
    FIBER_ASSERT(!hook.linked);
    FIBER_ASSERT(callback != nullptr);
    hook.gate = this;
    hook.callback = callback;
    hook.ctx = ctx;
    hook.prev = observer_tail_;
    hook.next = nullptr;
    if (observer_tail_ != nullptr) {
        observer_tail_->next = &hook;
    } else {
        observer_head_ = &hook;
    }
    observer_tail_ = &hook;
    hook.linked = true;
}

void Http2CloseGate::remove_observer(ObserverHook &hook) noexcept {
    if (!hook.linked) {
        return;
    }
    if (hook.prev != nullptr) {
        hook.prev->next = hook.next;
    } else {
        observer_head_ = hook.next;
    }
    if (hook.next != nullptr) {
        hook.next->prev = hook.prev;
    } else {
        observer_tail_ = hook.prev;
    }
    hook.prev = nullptr;
    hook.next = nullptr;
    hook.linked = false;
    hook.gate = nullptr;
    hook.callback = nullptr;
    hook.ctx = nullptr;
}

void Http2CloseGate::dispatch() noexcept {
    FIBER_ASSERT(phase_ == Phase::Pending);
    phase_ = Phase::Dispatching;
    while (ObserverHook *hook = observer_head_) {
        ObserverCallback callback = hook->callback;
        void *observer_ctx = hook->ctx;
        remove_observer(*hook);
        callback(observer_ctx, *connection_, terminal_error_);
    }
    phase_ = Phase::Complete;
    while (Joiner *joiner = joiner_head_) {
        unlink_joiner(*joiner);
        joiner->complete(terminal_error_);
    }
}

void Http2CloseGate::link_joiner(Joiner &joiner) noexcept {
    FIBER_ASSERT(!joiner.linked_);
    joiner.prev_ = joiner_tail_;
    joiner.next_ = nullptr;
    if (joiner_tail_ != nullptr) {
        joiner_tail_->next_ = &joiner;
    } else {
        joiner_head_ = &joiner;
    }
    joiner_tail_ = &joiner;
    joiner.linked_ = true;
    ++joiner_count_;
}

void Http2CloseGate::unlink_joiner(Joiner &joiner) noexcept {
    FIBER_ASSERT(joiner.linked_);
    if (joiner.prev_ != nullptr) {
        joiner.prev_->next_ = joiner.next_;
    } else {
        joiner_head_ = joiner.next_;
    }
    if (joiner.next_ != nullptr) {
        joiner.next_->prev_ = joiner.prev_;
    } else {
        joiner_tail_ = joiner.prev_;
    }
    joiner.prev_ = nullptr;
    joiner.next_ = nullptr;
    joiner.linked_ = false;
    FIBER_ASSERT(joiner_count_ != 0);
    --joiner_count_;
}

} // namespace fiber::http
