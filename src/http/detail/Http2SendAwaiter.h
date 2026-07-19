#ifndef FIBER_HTTP_DETAIL_HTTP2_SEND_AWAITER_H
#define FIBER_HTTP_DETAIL_HTTP2_SEND_AWAITER_H

#include <chrono>
#include <coroutine>
#include <type_traits>

#include "../../common/IoError.h"
#include "../../event/EventLoop.h"
#include "../Http2Outbound.h"

namespace fiber::http::detail {

template<class Owner>
class SendAwaiterBase : public Http2OutboundOperation {
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
        if (loop_ && resume_posted_) {
            loop_->template cancel<SendAwaiterBase, &SendAwaiterBase::notify_entry_>(*this);
            resume_posted_ = false;
        }
        owner_->stream().disarm_outbound(*this);
        owner_ = nullptr;
    }

    [[nodiscard]] bool try_arm() noexcept { return owner_ && owner_->stream().try_arm_outbound(*this); }

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
        if (loop_ && resume_posted_) {
            loop_->template cancel<SendAwaiterBase, &SendAwaiterBase::notify_entry_>(*this);
            resume_posted_ = false;
        }
        if (owner_) {
            owner_->stream().disarm_outbound(*this);
        }
        owner_ = nullptr;
        loop_ = nullptr;
        handle_ = {};
        result_ = common::IoErr::None;
        completed_ = false;
        resume_posted_ = false;
        return result;
    }

    void on_outbound_abort(common::IoErr result) noexcept override { complete(result); }
    void complete(common::IoErr result) noexcept {
        if (completed_) {
            return;
        }
        completed_ = true;
        result_ = result;
        post_resume();
    }

protected:
    static void on_notify(SendAwaiterBase *awaiter) noexcept {
        if (!awaiter) {
            return;
        }
        awaiter->resume_posted_ = false;
        awaiter->resume();
    }

    static void on_timeout(SendAwaiterBase *awaiter) noexcept {
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
        loop_->template post_local<SendAwaiterBase, &SendAwaiterBase::notify_entry_, &SendAwaiterBase::on_notify>(
                *this);
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
    fiber::event::EventLoop::DeferEntry notify_entry_{};
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
    HeaderSendAwaiter(Owner &owner, std::chrono::milliseconds timeout,
                      Args &&...args) noexcept(std::is_nothrow_constructible_v<Op, Args...>) :
        Base(owner, timeout), op_(static_cast<Args &&>(args)...) {}

    ~HeaderSendAwaiter() override { this->on_destroy_cleanup(); }

    [[nodiscard]] common::IoErr start() noexcept {
        if (!this->owner_) {
            return common::IoErr::Invalid;
        }
        return op_.submit(*this->owner_, *this);
    }

    common::IoErr encode_outbound_batch(Http2Stream &stream, const Http2OutboundEncodeRequest &req,
                                        Http2OutboundEncodeTarget &target,
                                        Http2OutboundEncodeResult &result) noexcept override {
        if (!this->owner_) {
            return common::IoErr::Invalid;
        }
        return op_.encode_outbound_batch(*this->owner_, *this, stream, req, target, result);
    }

    [[nodiscard]] std::size_t pending_flow_controlled_bytes() const noexcept override {
        return op_.pending_flow_controlled_bytes();
    }

    void on_outbound_batch_sent(std::uint32_t flow_controlled_bytes, bool operation_final_batch) noexcept override {
        if (this->owner_) {
            op_.on_outbound_batch_sent(*this->owner_, *this, flow_controlled_bytes, operation_final_batch);
        }
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
    BodySendAwaiter(Owner &owner, std::chrono::milliseconds timeout,
                    Args &&...args) noexcept(std::is_nothrow_constructible_v<Op, Args...>) :
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
        return op_.submit(*this->owner_, *this);
    }

    common::IoErr encode_outbound_batch(Http2Stream &stream, const Http2OutboundEncodeRequest &req,
                                        Http2OutboundEncodeTarget &target,
                                        Http2OutboundEncodeResult &result) noexcept override {
        if (!this->owner_) {
            return common::IoErr::Invalid;
        }
        return op_.encode_outbound_batch(*this->owner_, *this, stream, req, target, result);
    }

    [[nodiscard]] std::size_t pending_flow_controlled_bytes() const noexcept override {
        return op_.pending_flow_controlled_bytes();
    }

    void on_outbound_batch_sent(std::uint32_t flow_controlled_bytes, bool operation_final_batch) noexcept override {
        if (this->owner_) {
            op_.on_outbound_batch_sent(*this->owner_, *this, flow_controlled_bytes, operation_final_batch);
        }
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

    void on_outbound_abort(common::IoErr result) noexcept override { this->complete(result); }

    Op op_;

private:
    void on_destroy_cleanup() noexcept override {
        if (this->owner_) {
            (void) this->owner_->cancel_queued_send();
        }
    }

    void on_timeout_ready() noexcept override {
        if (!this->owner_) {
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
            this->complete(common::IoErr::TimedOut);
        }
    }
};

} // namespace fiber::http::detail

#endif // FIBER_HTTP_DETAIL_HTTP2_SEND_AWAITER_H
