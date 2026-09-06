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

Http2CloseGate::~Http2CloseGate() {
    while (joiner_head_ != nullptr) {
        Joiner *joiner = joiner_head_;
        unlink_joiner(*joiner);
        joiner->complete(common::IoErr::Canceled);
    }
    while (observer_head_ != nullptr) {
        remove_observer(*observer_head_);
    }
    if (installed_) {
        connection_->clear_closed_callback();
        installed_ = false;
    }
    connection_ = nullptr;
}

void Http2CloseGate::arm(Http2Connection &connection) noexcept {
    FIBER_ASSERT(connection_ == nullptr);
    connection_ = &connection;
    if (connection_->close_dispatched()) {
        // Nothing will call back: a failed start reports closure inline.
        closed_ = true;
        terminal_error_ = connection_->terminal_error();
        return;
    }
    // One gate per connection: the connection has a single callback slot.
    FIBER_ASSERT(!connection_->has_closed_callback());
    connection_->set_closed_callback(&Http2CloseGate::on_connection_closed, this);
    installed_ = true;
}

bool Http2CloseGate::closed() const noexcept {
    return closed_ || (connection_ != nullptr && connection_->close_dispatched());
}

common::IoErr Http2CloseGate::terminal_error() const noexcept {
    if (closed_) {
        return terminal_error_;
    }
    return connection_ != nullptr ? connection_->terminal_error() : common::IoErr::None;
}

fiber::async::Task<Http2CloseGate::CloseResult> Http2CloseGate::join() noexcept {
    if (connection_ == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (closed()) {
        const common::IoErr reason = terminal_error();
        co_return reason == common::IoErr::None ? CloseResult{} : CloseResult(std::unexpected(reason));
    }

    Joiner joiner(*this);
    const common::IoErr reason = co_await joiner;
    co_return reason == common::IoErr::None ? CloseResult{} : CloseResult(std::unexpected(reason));
}

void Http2CloseGate::add_observer(ObserverHook &hook, ObserverCallback callback, void *ctx) noexcept {
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

void Http2CloseGate::on_connection_closed(void *ctx, Http2Connection &, CloseResult result) noexcept {
    auto *gate = static_cast<Http2CloseGate *>(ctx);
    FIBER_ASSERT(gate != nullptr);
    gate->dispatch(result ? common::IoErr::None : result.error());
}

void Http2CloseGate::dispatch(common::IoErr reason) noexcept {
    if (closed_) {
        return;
    }
    closed_ = true;
    terminal_error_ = reason;
    installed_ = false;

    // Observers run first and inline: they unwind their own references to the
    // connection before any joiner can resume and tear it down.
    FIBER_ASSERT(connection_ != nullptr);
    ObserverHook *hook = observer_head_;
    while (hook != nullptr) {
        ObserverHook *next = hook->next;
        ObserverCallback callback = hook->callback;
        void *observer_ctx = hook->ctx;
        remove_observer(*hook);
        if (callback != nullptr) {
            callback(observer_ctx, *connection_, reason);
        }
        hook = next;
    }

    while (joiner_head_ != nullptr) {
        Joiner *joiner = joiner_head_;
        unlink_joiner(*joiner);
        joiner->complete(reason);
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
