#ifndef FIBER_HTTP_DETAIL_HTTP2_SEND_AWAITER_H
#define FIBER_HTTP_DETAIL_HTTP2_SEND_AWAITER_H

#include <chrono>
#include <coroutine>
#include <type_traits>

#include "../../common/IoError.h"
#include "../../event/EventLoop.h"

namespace fiber::http::detail {

template<class Owner>
class SendAwaiterBase {
public:
    SendAwaiterBase(Owner &owner, std::chrono::milliseconds timeout) noexcept : owner_(&owner), timeout_(timeout) {}

    SendAwaiterBase(const SendAwaiterBase &) = delete;
    SendAwaiterBase &operator=(const SendAwaiterBase &) = delete;
    SendAwaiterBase(SendAwaiterBase &&) = delete;
    SendAwaiterBase &operator=(SendAwaiterBase &&) = delete;

    virtual ~SendAwaiterBase() {
        if (!owner_) {
            return;
        }
        if (loop_ && timer_entry_.is_in_heap()) {
            loop_->template cancel<SendAwaiterBase, &SendAwaiterBase::timer_entry_>(*this);
        }
        if (owner_->send_awaiter_ == this) {
            owner_->send_awaiter_ = nullptr;
        }
        owner_ = nullptr;
    }

    [[nodiscard]] bool try_arm() noexcept {
        if (!owner_ || owner_->send_awaiter_ != nullptr) {
            return false;
        }
        owner_->send_awaiter_ = this;
        return true;
    }

    [[nodiscard]] Owner *owner() const noexcept { return owner_; }

    bool await_ready() noexcept {
        if (!owner_ || completed_) {
            return true;
        }
        if (timeout_.count() == 0) {
            on_timeout_ready();
            return true;
        }
        return false;
    }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
        if (!owner_ || completed_) {
            return false;
        }
        loop_ = &fiber::event::EventLoop::current();
        handle_ = handle;
        if (has_timer()) {
            loop_->template post_at<SendAwaiterBase, &SendAwaiterBase::timer_entry_, &SendAwaiterBase::on_timeout>(
                loop_->now() + timeout_, *this);
        }
        return true;
    }

    [[nodiscard]] common::IoErr take_result() noexcept {
        common::IoErr result = result_;
        if (loop_ && timer_entry_.is_in_heap()) {
            loop_->template cancel<SendAwaiterBase, &SendAwaiterBase::timer_entry_>(*this);
        }
        if (owner_ && owner_->send_awaiter_ == this) {
            owner_->send_awaiter_ = nullptr;
        }
        owner_ = nullptr;
        loop_ = nullptr;
        handle_ = {};
        result_ = common::IoErr::None;
        completed_ = false;
        resume_posted_ = false;
        return result;
    }

    virtual void on_abort(common::IoErr result) noexcept { complete(result); }
    virtual void on_stream_send_window_available() noexcept {}
    void complete(common::IoErr result) noexcept {
        if (completed_) {
            return;
        }
        completed_ = true;
        result_ = result;
        post_resume();
    }

protected:
    static void on_notify(SendAwaiterBase *awaiter) {
        if (!awaiter) {
            return;
        }
        awaiter->resume_posted_ = false;
        awaiter->resume();
    }

    static void on_timeout(SendAwaiterBase *awaiter) {
        if (!awaiter || awaiter->completed_) {
            return;
        }
        awaiter->on_timeout_fired();
    }

    void post_resume() noexcept {
        if (resume_posted_ || !loop_) {
            return;
        }
        resume_posted_ = true;
        loop_->template post<SendAwaiterBase, &SendAwaiterBase::notify_entry_, &SendAwaiterBase::on_notify>(*this);
    }

    void resume() noexcept {
        auto handle = handle_;
        handle_ = {};
        if (handle) {
            handle.resume();
        }
    }

    [[nodiscard]] bool has_timer() const noexcept {
        return timeout_.count() > 0 && timeout_ != std::chrono::milliseconds::max();
    }

    virtual void on_destroy_cleanup() noexcept {}
    virtual void on_timeout_ready() noexcept = 0;
    virtual void on_timeout_fired() noexcept = 0;

    Owner *owner_ = nullptr;
    std::chrono::milliseconds timeout_{};
    fiber::event::EventLoop *loop_ = nullptr;
    std::coroutine_handle<> handle_{};
    fiber::event::EventLoop::NotifyEntry notify_entry_{};
    fiber::event::EventLoop::TimerEntry timer_entry_{};
    common::IoErr result_ = common::IoErr::None;
    bool completed_ = false;
    bool resume_posted_ = false;
};

template<class Owner, class Op>
class HeaderSendAwaiter : public SendAwaiterBase<Owner> {
public:
    using Base = SendAwaiterBase<Owner>;
    using SuccessType = typename Op::SuccessType;
    using AwaitResult = common::IoResult<SuccessType>;

    template<class... Args>
    HeaderSendAwaiter(Owner &owner, std::chrono::milliseconds timeout, Args &&...args) noexcept(
        std::is_nothrow_constructible_v<Op, Args...>) :
        Base(owner, timeout), op_(static_cast<Args &&>(args)...) {}

    ~HeaderSendAwaiter() override { this->on_destroy_cleanup(); }

    [[nodiscard]] common::IoErr start() noexcept {
        if (!this->owner_) {
            return common::IoErr::Invalid;
        }
        return op_.submit(*this->owner_, *this);
    }

    AwaitResult await_resume() noexcept {
        common::IoErr result = this->take_result();
        if (result != common::IoErr::None) {
            return std::unexpected(result);
        }
        if constexpr (std::is_void_v<SuccessType>) {
            return AwaitResult{};
        } else {
            return AwaitResult{op_.success_result(*this)};
        }
    }

    Op op_;

private:
    void on_destroy_cleanup() noexcept override {
        if (this->owner_) {
            (void) this->owner_->cancel_queued_send();
        }
    }

    void on_timeout_ready() noexcept override {
        if (this->owner_ && this->owner_->cancel_queued_send()) {
            this->completed_ = true;
            this->result_ = common::IoErr::TimedOut;
        }
    }

    void on_timeout_fired() noexcept override {
        if (!this->owner_ || !this->owner_->cancel_queued_send()) {
            return;
        }
        this->complete(common::IoErr::TimedOut);
    }
};

template<class Owner, class Op>
class BodySendAwaiter : public SendAwaiterBase<Owner> {
public:
    using Base = SendAwaiterBase<Owner>;
    using SuccessType = typename Op::SuccessType;
    using AwaitResult = common::IoResult<SuccessType>;

    template<class... Args>
    BodySendAwaiter(Owner &owner, std::chrono::milliseconds timeout, Args &&...args) noexcept(
        std::is_nothrow_constructible_v<Op, Args...>) :
        Base(owner, timeout), op_(static_cast<Args &&>(args)...) {}

    ~BodySendAwaiter() override { this->on_destroy_cleanup(); }

    [[nodiscard]] common::IoErr start() noexcept {
        if (!this->owner_) {
            return common::IoErr::Invalid;
        }
        if (op_.should_complete_without_submit()) {
            this->complete(common::IoErr::None);
            return common::IoErr::None;
        }
        if (op_.needs_stream_window() && this->owner_->stream().send_window() <= 0) {
            waiting_stream_window_ = true;
            return common::IoErr::None;
        }
        return request_submit();
    }

    AwaitResult await_resume() noexcept {
        common::IoErr result = this->take_result();
        if (result != common::IoErr::None) {
            return std::unexpected(result);
        }
        if constexpr (std::is_void_v<SuccessType>) {
            return AwaitResult{};
        } else {
            return AwaitResult{op_.success_result(*this)};
        }
    }

    void on_abort(common::IoErr result) noexcept override {
        if (this->owner_) {
            (void) this->owner_->cancel_queued_send();
        }
        waiting_stream_window_ = false;
        this->complete(result);
    }

    void on_stream_send_window_available() noexcept override {
        if (this->completed_ || !this->owner_ || !waiting_stream_window_) {
            return;
        }
        if (resume_submit_posted_ || this->loop_ == nullptr) {
            return;
        }
        resume_submit_posted_ = true;
        this->loop_
            ->template post<BodySendAwaiter, &BodySendAwaiter::submit_notify_entry_, &BodySendAwaiter::on_submit_notify>(
            *this);
    }

    Op op_;
    void clear_waiting_stream_window() noexcept { waiting_stream_window_ = false; }
    void mark_waiting_stream_window() noexcept { waiting_stream_window_ = true; }
    [[nodiscard]] bool waiting_stream_window() const noexcept { return waiting_stream_window_; }
    bool waiting_stream_window_ = false;

private:
    static void on_submit_notify(BodySendAwaiter *awaiter) {
        if (!awaiter) {
            return;
        }
        awaiter->resume_submit_posted_ = false;
        awaiter->try_submit_from_window_signal();
    }

    void on_destroy_cleanup() noexcept override {
        if (this->owner_) {
            (void) this->owner_->cancel_queued_send();
        }
        waiting_stream_window_ = false;
        resume_submit_posted_ = false;
    }

    void on_timeout_ready() noexcept override {
        if (!this->owner_) {
            return;
        }
        if (waiting_stream_window_) {
            this->complete(common::IoErr::TimedOut);
            return;
        }
        if (this->owner_->cancel_queued_send()) {
            this->complete(common::IoErr::TimedOut);
        }
    }

    void on_timeout_fired() noexcept override {
        if (!this->owner_) {
            return;
        }
        if (this->owner_->cancel_queued_send()) {
            waiting_stream_window_ = false;
            this->complete(common::IoErr::TimedOut);
            return;
        }
        if (waiting_stream_window_) {
            this->complete(common::IoErr::TimedOut);
        }
    }

    [[nodiscard]] common::IoErr request_submit() noexcept {
        if (!this->owner_) {
            return common::IoErr::Invalid;
        }
        waiting_stream_window_ = false;
        return op_.submit(*this->owner_, *this);
    }

    void try_submit_from_window_signal() noexcept {
        if (this->completed_ || !this->owner_ || !waiting_stream_window_) {
            return;
        }
        if (op_.needs_stream_window() && this->owner_->stream().send_window() <= 0) {
            return;
        }
        common::IoErr err = request_submit();
        if (err != common::IoErr::None) {
            this->complete(err);
        }
    }

    bool resume_submit_posted_ = false;
    fiber::event::EventLoop::NotifyEntry submit_notify_entry_{};
};

} // namespace fiber::http::detail

#endif // FIBER_HTTP_DETAIL_HTTP2_SEND_AWAITER_H
