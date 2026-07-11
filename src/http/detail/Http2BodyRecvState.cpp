#include "Http2BodyRecvState.h"

#include <coroutine>

#include "../../common/Assert.h"
#include "../../event/EventLoop.h"
#include "../Http2Stream.h"

namespace fiber::http::detail {

struct Http2BodyRecvState::PollResult {
    enum class Kind : std::uint8_t {
        Wait,
        Readable,
        End,
        TimedOut,
        Closed,
    };

    Kind kind = Kind::Wait;
    common::IoErr error = common::IoErr::None;
};

class Http2BodyRecvState::BodyReadAwaiter {
public:
    BodyReadAwaiter(Http2BodyRecvState &state, std::chrono::milliseconds timeout) noexcept :
        state_(&state), timeout_(timeout) {}

    BodyReadAwaiter(const BodyReadAwaiter &) = delete;
    BodyReadAwaiter &operator=(const BodyReadAwaiter &) = delete;
    BodyReadAwaiter(BodyReadAwaiter &&) = delete;
    BodyReadAwaiter &operator=(BodyReadAwaiter &&) = delete;

    ~BodyReadAwaiter() {
        if (!state_) {
            return;
        }
        state_->cancel_waiter(this);
        if (loop_ && timer_entry_.is_in_heap()) {
            loop_->cancel<BodyReadAwaiter, &BodyReadAwaiter::timer_entry_>(*this);
        }
    }

    bool await_ready() noexcept {
        if (!state_) {
            return true;
        }
        PollResult state = state_->poll();
        if (state.kind != PollResult::Kind::Wait) {
            return true;
        }
        if (timeout_.count() == 0) {
            timed_out_ = true;
            return true;
        }
        return false;
    }

    bool await_suspend(std::coroutine_handle<> handle) {
        if (!state_) {
            return false;
        }

        loop_ = &fiber::event::EventLoop::current();
        handle_ = handle;
        if (!state_->arm_waiter(this)) {
            return false;
        }
        if (has_timer()) {
            loop_->post_at<BodyReadAwaiter, &BodyReadAwaiter::timer_entry_, &BodyReadAwaiter::on_timeout>(
                    loop_->now() + timeout_, *this);
        }
        return true;
    }

    PollResult await_resume() noexcept {
        PollResult result{};
        if (!state_) {
            result.kind = PollResult::Kind::Closed;
            result.error = common::IoErr::Canceled;
            return result;
        }

        if (loop_ && timer_entry_.is_in_heap()) {
            loop_->cancel<BodyReadAwaiter, &BodyReadAwaiter::timer_entry_>(*this);
        }
        if (state_->waiter_ == this) {
            state_->waiter_ = nullptr;
        }

        if (timed_out_) {
            result.kind = PollResult::Kind::TimedOut;
        } else {
            result = state_->poll();
            FIBER_ASSERT(result.kind != PollResult::Kind::Wait);
        }

        state_ = nullptr;
        handle_ = {};
        loop_ = nullptr;
        timed_out_ = false;
        resume_posted_ = false;
        return result;
    }

private:
    static void on_notify(BodyReadAwaiter *awaiter) noexcept {
        if (!awaiter) {
            return;
        }
        awaiter->resume_posted_ = false;
        awaiter->resume();
    }

    static void on_timeout(BodyReadAwaiter *awaiter) noexcept {
        if (!awaiter) {
            return;
        }
        awaiter->timed_out_ = true;
        if (awaiter->state_ && awaiter->state_->waiter_ == awaiter) {
            awaiter->state_->waiter_ = nullptr;
        }
        awaiter->resume();
    }

    void resume() noexcept {
        auto handle = handle_;
        handle_ = {};
        if (handle) {
            handle.resume();
        }
    }

    bool has_timer() const noexcept { return timeout_.count() > 0 && timeout_ != std::chrono::milliseconds::max(); }

    Http2BodyRecvState *state_ = nullptr;
    std::chrono::milliseconds timeout_{};
    fiber::event::EventLoop *loop_ = nullptr;
    std::coroutine_handle<> handle_{};
    fiber::event::EventLoop::NotifyEntry notify_entry_{};
    fiber::event::EventLoop::TimerEntry timer_entry_{};
    bool timed_out_ = false;
    bool resume_posted_ = false;

    friend class Http2BodyRecvState;
};

Http2BodyRecvState::Http2BodyRecvState(mem::IoBufNodePool &node_pool) noexcept : queue_(node_pool) {}

common::IoErr Http2BodyRecvState::push_body(mem::IoBuf &&buf, bool end_stream) noexcept {
    const bool queued_data = buf.readable() != 0;
    if (queued_data && !queue_.append(std::move(buf))) {
        return common::IoErr::NoMem;
    }
    if (end_stream) {
        input_closed_ = true;
    }
    if (queued_data || end_stream) {
        notify_waiter();
    }
    return common::IoErr::None;
}

void Http2BodyRecvState::close_input() noexcept {
    input_closed_ = true;
    notify_waiter();
}

void Http2BodyRecvState::abort(common::IoErr reason) noexcept {
    if (abort_reason_ == common::IoErr::None) {
        abort_reason_ = reason;
    }
    notify_waiter();
}

fiber::async::Task<common::IoResult<mem::IoBufChain>>
Http2BodyRecvState::read_body(Http2Stream &stream, std::size_t max_bytes, std::chrono::milliseconds timeout) noexcept {
    mem::IoBufChain out(queue_.node_pool());
    if (max_bytes == 0) {
        if (queue_.readable_bytes() == 0 && input_closed_) {
            out.mark_complete();
        }
        co_return out;
    }

    PollResult state = co_await BodyReadAwaiter(*this, timeout);
    switch (state.kind) {
        case PollResult::Kind::Readable:
            break;
        case PollResult::Kind::End:
            out.mark_complete();
            co_return out;
        case PollResult::Kind::TimedOut:
            co_return std::unexpected(common::IoErr::TimedOut);
        case PollResult::Kind::Closed:
            co_return std::unexpected(state.error);
        case PollResult::Kind::Wait:
            FIBER_ASSERT(false);
            co_return std::unexpected(common::IoErr::Invalid);
    }

    const std::size_t queued_bytes = queue_.readable_bytes();
    const std::size_t take = std::min(max_bytes, queued_bytes);
    if (!queue_.take_prefix(take, out)) {
        co_return std::unexpected(common::IoErr::NoMem);
    }
    if (common::IoErr err = stream.maybe_replenish_recv_window(queue_.readable_bytes()); err != common::IoErr::None) {
        co_return std::unexpected(err);
    }
    if (queue_.readable_bytes() == 0 && input_closed_) {
        out.mark_complete();
    }
    co_return out;
}

Http2BodyRecvState::PollResult Http2BodyRecvState::poll() const noexcept {
    PollResult result{};
    if (queue_.readable_bytes() != 0) {
        result.kind = PollResult::Kind::Readable;
        return result;
    }
    if (input_closed_) {
        result.kind = PollResult::Kind::End;
        return result;
    }
    if (abort_reason_ != common::IoErr::None) {
        result.kind = PollResult::Kind::Closed;
        result.error = abort_reason_;
        return result;
    }
    return result;
}

bool Http2BodyRecvState::arm_waiter(BodyReadAwaiter *awaiter) noexcept {
    if (!awaiter || poll().kind != PollResult::Kind::Wait) {
        return false;
    }
    FIBER_ASSERT(waiter_ == nullptr);
    waiter_ = awaiter;
    return true;
}

void Http2BodyRecvState::cancel_waiter(BodyReadAwaiter *awaiter) noexcept {
    if (waiter_ == awaiter) {
        waiter_ = nullptr;
    }
}

void Http2BodyRecvState::notify_waiter() noexcept {
    if (!waiter_ || waiter_->resume_posted_ || waiter_->loop_ == nullptr) {
        return;
    }
    waiter_->resume_posted_ = true;
    waiter_->loop_->post<BodyReadAwaiter, &BodyReadAwaiter::notify_entry_, &BodyReadAwaiter::on_notify>(*waiter_);
}

} // namespace fiber::http::detail
