#ifndef FIBER_HTTP_DETAIL_HTTP2_SEND_AWAITER_H
#define FIBER_HTTP_DETAIL_HTTP2_SEND_AWAITER_H

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <type_traits>
#include <utility>

#include "../../common/Assert.h"
#include "../../common/IoError.h"
#include "../../event/EventLoop.h"
#include "../Http2Outbound.h"
#include "../Http2Stream.h"

namespace fiber::http::detail {

// Runs one send operation on a stream. The operation encodes its frames:
//
//   static constexpr Http2OutboundKind kOutboundKind;  // Headers or Data
//   static constexpr bool kAllowsPartialFinalBatch;    // optional
//   std::size_t flow_controlled_bytes() const;          // Data only
//   bool end_stream() const;                            // Data only
//   common::IoErr on_encode(const Http2OutboundEncodeRequest &, Http2OutboundEncodeTarget &,
//                           Http2OutboundEncodeResult &);
//
// A DATA send resolves to the payload bytes it sent.
template<class Op>
class Http2SendAwaiter {
    static_assert(Op::kOutboundKind != Http2OutboundKind::None);
    static constexpr bool kData = Op::kOutboundKind == Http2OutboundKind::Data;

public:
    using SuccessType = std::conditional_t<kData, std::size_t, void>;
    using AwaitResult = common::IoResult<SuccessType>;

    template<class... Args>
    Http2SendAwaiter(Http2Stream &stream, std::chrono::milliseconds timeout,
                     Args &&...args) noexcept(std::is_nothrow_constructible_v<Op, Args...>) :
        stream_(stream), timeout_(timeout), loop_(fiber::event::EventLoop::current()),
        op_(static_cast<Args &&>(args)...) {}

    Http2SendAwaiter(const Http2SendAwaiter &) = delete;
    Http2SendAwaiter &operator=(const Http2SendAwaiter &) = delete;
    Http2SendAwaiter(Http2SendAwaiter &&) = delete;
    Http2SendAwaiter &operator=(Http2SendAwaiter &&) = delete;

    ~Http2SendAwaiter() { cleanup(true); }

    bool await_ready() noexcept {
        start();
        if (completed_ || timeout_.count() != 0) {
            return completed_;
        }

        // A zero timeout never waits, and a send only just queued has nothing
        // encoded yet.
        const bool canceled = stream_.cancel_queued_outbound();
        FIBER_ASSERT(canceled);
        complete(common::IoErr::TimedOut);
        return true;
    }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
        handle_ = handle;
        if (has_timer()) {
            loop_.post_at<Http2SendAwaiter, &Http2SendAwaiter::timer_entry_, &Http2SendAwaiter::on_timeout>(
                    loop_.now() + timeout_, *this);
        }
        return true;
    }

    AwaitResult await_resume() noexcept {
        cleanup(false);
        if (result_ != common::IoErr::None) {
            return std::unexpected(result_);
        }
        if constexpr (kData) {
            return AwaitResult{sent_bytes_};
        } else {
            return AwaitResult{};
        }
    }

private:
    static common::IoErr on_encode(void *ctx, const Http2OutboundEncodeRequest &req, Http2OutboundEncodeTarget &target,
                                   Http2OutboundEncodeResult &result) noexcept {
        auto *awaiter = static_cast<Http2SendAwaiter *>(ctx);
        // A failed encode reaches on_send_done through the connection.
        const common::IoErr err = awaiter->op_.on_encode(req, target, result);
        if (err == common::IoErr::None) {
            awaiter->sent_bytes_ += result.flow_controlled_bytes;
        }
        return err;
    }

    static void on_send_done(void *ctx, const Http2OutboundSendResult &result) noexcept {
        auto *awaiter = static_cast<Http2SendAwaiter *>(ctx);
        if (result.error != common::IoErr::None) {
            awaiter->complete(result.error);
        } else if (result.operation_final_batch) {
            awaiter->complete(common::IoErr::None);
        }
    }

    static void on_notify(Http2SendAwaiter *awaiter) noexcept {
        FIBER_ASSERT(awaiter->handle_);
        std::exchange(awaiter->handle_, {}).resume();
    }

    static void on_timeout(Http2SendAwaiter *awaiter) noexcept {
        if (awaiter->completed_) {
            return;
        }
        // An encoded batch must reach the wire, so the timeout only withdraws
        // a send that is still queued.
        if (awaiter->stream_.cancel_queued_outbound()) {
            awaiter->complete(common::IoErr::TimedOut);
        }
    }

    void start() noexcept {
        std::size_t flow_controlled_bytes = 0;
        if constexpr (kData) {
            flow_controlled_bytes = op_.flow_controlled_bytes();
            if (flow_controlled_bytes == 0 && !op_.end_stream()) {
                // Nothing to send, but the write still answers as one would.
                complete(stream_.outbound_idle_status());
                return;
            }
        }

        const common::IoErr err = stream_.try_arm_outbound(kOutboundOps, this, flow_controlled_bytes);
        if (err != common::IoErr::None) {
            complete(err);
            return;
        }
        armed_ = true;
    }

    void complete(common::IoErr result) noexcept {
        if (completed_) {
            return;
        }
        completed_ = true;
        result_ = result;
        loop_.post_local<Http2SendAwaiter, &Http2SendAwaiter::notify_entry_, &Http2SendAwaiter::on_notify>(*this);
    }

    // `abandon` is the destructor path, where the send may still be pending:
    // the stream withdraws or detaches it instead of expecting an idle hook.
    void cleanup(bool abandon) noexcept {
        if (timer_entry_.is_in_heap()) {
            loop_.cancel<Http2SendAwaiter, &Http2SendAwaiter::timer_entry_>(*this);
        }
        if (notify_entry_.is_in_queue()) {
            loop_.cancel<Http2SendAwaiter, &Http2SendAwaiter::notify_entry_>(*this);
        }
        if (armed_) {
            armed_ = false;
            if (abandon) {
                stream_.abandon_outbound(this);
            } else {
                stream_.disarm_outbound(this);
            }
        }
        handle_ = {};
    }

    [[nodiscard]] bool has_timer() const noexcept {
        return timeout_.count() > 0 && timeout_ != std::chrono::milliseconds::max();
    }

    inline static constexpr Http2OutboundOperation::Ops kOutboundOps{
            .on_encode = &Http2SendAwaiter::on_encode,
            .on_send_done = &Http2SendAwaiter::on_send_done,
            .kind = Op::kOutboundKind,
            .allow_partial_final_batch =
                    []() constexpr {
                        if constexpr (requires { Op::kAllowsPartialFinalBatch; }) {
                            return Op::kAllowsPartialFinalBatch;
                        } else {
                            return false;
                        }
                    }(),
    };

    Http2Stream &stream_;
    std::chrono::milliseconds timeout_{};
    fiber::event::EventLoop &loop_;
    std::coroutine_handle<> handle_{};
    fiber::event::EventLoop::DeferEntry notify_entry_{};
    fiber::event::EventLoop::TimerEntry timer_entry_{};
    common::IoErr result_ = common::IoErr::None;
    Op op_;
    std::size_t sent_bytes_ = 0;
    bool armed_ = false;
    bool completed_ = false;
};

} // namespace fiber::http::detail

#endif // FIBER_HTTP_DETAIL_HTTP2_SEND_AWAITER_H
