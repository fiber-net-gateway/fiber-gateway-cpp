#ifndef FIBER_HTTP_DETAIL_HTTP2_SEND_AWAITER_H
#define FIBER_HTTP_DETAIL_HTTP2_SEND_AWAITER_H

#include <chrono>
#include <coroutine>
#include <type_traits>

#include "../../common/Assert.h"
#include "../../common/IoError.h"
#include "../../event/EventLoop.h"
#include "../Http2Outbound.h"

namespace fiber::http::detail {

template<class Owner, class Op>
class Http2SendAwaiter {
public:
    using SuccessType = typename Op::SuccessType;
    using AwaitResult = common::IoResult<SuccessType>;

    template<class... Args>
    Http2SendAwaiter(Owner &owner, std::chrono::milliseconds timeout,
                     Args &&...args) noexcept(std::is_nothrow_constructible_v<Op, Args...>) :
        owner_(&owner), timeout_(timeout), op_(static_cast<Args &&>(args)...) {}

    Http2SendAwaiter(const Http2SendAwaiter &) = delete;
    Http2SendAwaiter &operator=(const Http2SendAwaiter &) = delete;
    Http2SendAwaiter(Http2SendAwaiter &&) = delete;
    Http2SendAwaiter &operator=(Http2SendAwaiter &&) = delete;

    ~Http2SendAwaiter() { cleanup(true); }

    bool await_ready() noexcept {
        start();
        if (completed_) {
            return true;
        }
        if (timeout_.count() != 0) {
            return false;
        }

        const bool canceled = owner_ && owner_->cancel_queued_send();
        FIBER_ASSERT(canceled);
        if (canceled) {
            complete(common::IoErr::TimedOut);
        }
        return completed_;
    }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
        if (!owner_ || completed_) {
            return false;
        }
        loop_ = &fiber::event::EventLoop::current();
        handle_ = handle;
        if (has_timer()) {
            loop_->template post_at<Http2SendAwaiter, &Http2SendAwaiter::timer_entry_, &Http2SendAwaiter::on_timeout>(
                    loop_->now() + timeout_, *this);
        }
        return true;
    }

    AwaitResult await_resume() noexcept {
        const common::IoErr result = result_;
        if (result != common::IoErr::None) {
            cleanup(false);
            return std::unexpected(result);
        }
        if constexpr (std::is_void_v<SuccessType>) {
            cleanup(false);
            return AwaitResult{};
        } else {
            SuccessType value = op_.success_result();
            cleanup(false);
            return AwaitResult{static_cast<SuccessType &&>(value)};
        }
    }

private:
    static common::IoErr on_encode(void *ctx, Http2Stream &stream, const Http2OutboundEncodeRequest &req,
                                   Http2OutboundEncodeTarget &target, Http2OutboundEncodeResult &result) noexcept {
        auto *awaiter = static_cast<Http2SendAwaiter *>(ctx);
        FIBER_ASSERT(awaiter != nullptr);
        if (!awaiter->owner_) {
            return common::IoErr::Invalid;
        }

        common::IoErr error = awaiter->op_.on_encode(*awaiter->owner_, stream, req, target, result);
        if (error != common::IoErr::None) {
            awaiter->complete(error);
        }
        return error;
    }

    static void on_send_done(void *ctx, const Http2OutboundSendResult &result) noexcept {
        auto *awaiter = static_cast<Http2SendAwaiter *>(ctx);
        FIBER_ASSERT(awaiter != nullptr);
        if (!awaiter->owner_ || awaiter->completed_) {
            return;
        }
        if (result.error != common::IoErr::None) {
            awaiter->complete(result.error);
            return;
        }

        awaiter->op_.on_send_done(*awaiter->owner_, result.flow_controlled_bytes, result.operation_final_batch);
        if (result.operation_final_batch) {
            awaiter->complete(common::IoErr::None);
        }
    }

    static void on_notify(Http2SendAwaiter *awaiter) noexcept {
        if (!awaiter) {
            return;
        }
        awaiter->resume_posted_ = false;
        auto handle = awaiter->handle_;
        awaiter->handle_ = {};
        if (handle) {
            handle.resume();
        }
    }

    static void on_timeout(Http2SendAwaiter *awaiter) noexcept {
        if (!awaiter || !awaiter->owner_ || awaiter->completed_) {
            return;
        }
        if (awaiter->owner_->cancel_queued_send()) {
            awaiter->complete(common::IoErr::TimedOut);
        }
    }

    void start() noexcept {
        if (!owner_) {
            complete(common::IoErr::Invalid);
            return;
        }

        const std::size_t pending_flow_controlled_bytes = [this]() noexcept {
            if constexpr (requires(const Op &op) { op.pending_flow_controlled_bytes(); }) {
                return op_.pending_flow_controlled_bytes();
            } else {
                return std::size_t{0};
            }
        }();

        if (!owner_->stream().try_arm_outbound(kOutboundOps, this, pending_flow_controlled_bytes)) {
            complete(common::IoErr::Already);
            return;
        }
        armed_ = true;

        if constexpr (requires(const Op &op) { op.should_complete_without_submit(); }) {
            if (op_.should_complete_without_submit()) {
                complete(common::IoErr::None);
                return;
            }
        }

        common::IoErr error = op_.submit(*owner_);
        if (error != common::IoErr::None) {
            complete(error);
        }
    }

    void complete(common::IoErr result) noexcept {
        if (completed_) {
            return;
        }
        completed_ = true;
        result_ = result;
        if (!loop_ || resume_posted_) {
            return;
        }
        resume_posted_ = true;
        loop_->template post_local<Http2SendAwaiter, &Http2SendAwaiter::notify_entry_, &Http2SendAwaiter::on_notify>(
                *this);
    }

    void cleanup(bool cancel_send) noexcept {
        if (loop_ && timer_entry_.is_in_heap()) {
            loop_->template cancel<Http2SendAwaiter, &Http2SendAwaiter::timer_entry_>(*this);
        }
        if (loop_ && resume_posted_) {
            loop_->template cancel<Http2SendAwaiter, &Http2SendAwaiter::notify_entry_>(*this);
            resume_posted_ = false;
        }
        if (owner_ && armed_) {
            if (cancel_send) {
                (void) owner_->cancel_queued_send();
            }
            owner_->stream().disarm_outbound(this);
        }
        armed_ = false;
        owner_ = nullptr;
        loop_ = nullptr;
        handle_ = {};
    }

    [[nodiscard]] bool has_timer() const noexcept {
        return timeout_.count() > 0 && timeout_ != std::chrono::milliseconds::max();
    }

    inline static constexpr Http2OutboundOperation::Ops kOutboundOps{
            .on_encode = &Http2SendAwaiter::on_encode,
            .on_send_done = &Http2SendAwaiter::on_send_done,
            .allow_partial_final_batch =
                    []() constexpr {
                        if constexpr (requires { Op::kAllowsPartialFinalBatch; }) {
                            return Op::kAllowsPartialFinalBatch;
                        } else {
                            return false;
                        }
                    }(),
    };

    Owner *owner_ = nullptr;
    std::chrono::milliseconds timeout_{};
    fiber::event::EventLoop *loop_ = nullptr;
    std::coroutine_handle<> handle_{};
    fiber::event::EventLoop::DeferEntry notify_entry_{};
    fiber::event::EventLoop::TimerEntry timer_entry_{};
    common::IoErr result_ = common::IoErr::None;
    Op op_;
    bool armed_ = false;
    bool completed_ = false;
    bool resume_posted_ = false;
};

} // namespace fiber::http::detail

#endif // FIBER_HTTP_DETAIL_HTTP2_SEND_AWAITER_H
