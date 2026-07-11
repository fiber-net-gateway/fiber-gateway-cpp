#include "Http2HeaderBlockQueue.h"

#include <coroutine>
#include <new>

#include "../../common/Assert.h"
#include "../../event/EventLoop.h"

namespace fiber::http::detail {

struct Http2HeaderBlockQueue::PollResult {
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

class Http2HeaderBlockQueue::HeaderReadAwaiter {
public:
    HeaderReadAwaiter(Http2HeaderBlockQueue &queue, std::chrono::milliseconds timeout) noexcept :
        queue_(&queue), timeout_(timeout) {}

    ~HeaderReadAwaiter() {
        if (!queue_) {
            return;
        }
        queue_->cancel_waiter(this);
        if (loop_ && timer_entry_.is_in_heap()) {
            loop_->cancel<HeaderReadAwaiter, &HeaderReadAwaiter::timer_entry_>(*this);
        }
    }

    bool await_ready() noexcept {
        if (!queue_) {
            return true;
        }
        PollResult state = queue_->poll();
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
        if (!queue_) {
            return false;
        }
        loop_ = &fiber::event::EventLoop::current();
        handle_ = handle;
        if (!queue_->arm_waiter(this)) {
            return false;
        }
        if (has_timer()) {
            loop_->post_at<HeaderReadAwaiter, &HeaderReadAwaiter::timer_entry_, &HeaderReadAwaiter::on_timeout>(
                    loop_->now() + timeout_, *this);
        }
        return true;
    }

    PollResult await_resume() noexcept {
        PollResult result{};
        if (!queue_) {
            result.kind = PollResult::Kind::Closed;
            result.error = common::IoErr::Canceled;
            return result;
        }
        if (loop_ && timer_entry_.is_in_heap()) {
            loop_->cancel<HeaderReadAwaiter, &HeaderReadAwaiter::timer_entry_>(*this);
        }
        if (queue_->waiter_ == this) {
            queue_->waiter_ = nullptr;
        }
        if (timed_out_) {
            result.kind = PollResult::Kind::TimedOut;
        } else {
            result = queue_->poll();
            FIBER_ASSERT(result.kind != PollResult::Kind::Wait);
        }
        queue_ = nullptr;
        loop_ = nullptr;
        handle_ = {};
        timed_out_ = false;
        resume_posted_ = false;
        return result;
    }

private:
    static void on_notify(HeaderReadAwaiter *awaiter) noexcept {
        if (!awaiter) {
            return;
        }
        awaiter->resume_posted_ = false;
        awaiter->resume();
    }

    static void on_timeout(HeaderReadAwaiter *awaiter) noexcept {
        if (!awaiter) {
            return;
        }
        awaiter->timed_out_ = true;
        if (awaiter->queue_ && awaiter->queue_->waiter_ == awaiter) {
            awaiter->queue_->waiter_ = nullptr;
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

    [[nodiscard]] bool has_timer() const noexcept {
        return timeout_.count() > 0 && timeout_ != std::chrono::milliseconds::max();
    }

    Http2HeaderBlockQueue *queue_ = nullptr;
    std::chrono::milliseconds timeout_{};
    fiber::event::EventLoop *loop_ = nullptr;
    std::coroutine_handle<> handle_{};
    fiber::event::EventLoop::NotifyEntry notify_entry_{};
    fiber::event::EventLoop::TimerEntry timer_entry_{};
    bool timed_out_ = false;
    bool resume_posted_ = false;

    friend class Http2HeaderBlockQueue;
};

Http2HeaderBlockQueue::Http2HeaderBlockQueue(mem::BufPool &pool) noexcept : pool_(&pool) {}

Http2HeaderBlockQueue::HeaderNode *Http2HeaderBlockQueue::allocate_node() noexcept {
    if (!pool_) {
        return nullptr;
    }
    void *mem = pool_->alloc(sizeof(HeaderNode), alignof(HeaderNode));
    if (!mem) {
        return nullptr;
    }
    return new (mem) HeaderNode(*pool_);
}

common::IoErr Http2HeaderBlockQueue::push_header_block(HeaderNode *node) noexcept {
    if (!node) {
        return common::IoErr::Invalid;
    }
    node->next = nullptr;
    if (!head_) {
        head_ = node;
        tail_ = node;
    } else {
        tail_->next = node;
        tail_ = node;
    }
    notify_waiter();
    return common::IoErr::None;
}

void Http2HeaderBlockQueue::close_input() noexcept {
    input_closed_ = true;
    notify_waiter();
}

void Http2HeaderBlockQueue::abort(common::IoErr reason) noexcept {
    if (abort_reason_ == common::IoErr::None) {
        abort_reason_ = reason;
    }
    notify_waiter();
}

fiber::async::Task<common::IoResult<const Http2ResponseHead *>>
Http2HeaderBlockQueue::read_header(std::chrono::milliseconds timeout) noexcept {
    PollResult state = co_await HeaderReadAwaiter(*this, timeout);
    switch (state.kind) {
        case PollResult::Kind::Readable:
            break;
        case PollResult::Kind::End:
            co_return static_cast<const Http2ResponseHead *>(nullptr);
        case PollResult::Kind::TimedOut:
            co_return std::unexpected(common::IoErr::TimedOut);
        case PollResult::Kind::Closed:
            co_return std::unexpected(state.error);
        case PollResult::Kind::Wait:
            FIBER_ASSERT(false);
            co_return std::unexpected(common::IoErr::Invalid);
    }

    HeaderNode *node = head_;
    FIBER_ASSERT(node != nullptr);
    head_ = node->next;
    if (!head_) {
        tail_ = nullptr;
    }
    node->next = nullptr;
    co_return &node->head;
}

Http2HeaderBlockQueue::PollResult Http2HeaderBlockQueue::poll() const noexcept {
    PollResult result{};
    if (head_) {
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
    }
    return result;
}

bool Http2HeaderBlockQueue::arm_waiter(HeaderReadAwaiter *awaiter) noexcept {
    if (!awaiter || poll().kind != PollResult::Kind::Wait) {
        return false;
    }
    FIBER_ASSERT(waiter_ == nullptr);
    waiter_ = awaiter;
    return true;
}

void Http2HeaderBlockQueue::cancel_waiter(HeaderReadAwaiter *awaiter) noexcept {
    if (waiter_ == awaiter) {
        waiter_ = nullptr;
    }
}

void Http2HeaderBlockQueue::notify_waiter() noexcept {
    if (!waiter_ || waiter_->resume_posted_ || waiter_->loop_ == nullptr) {
        return;
    }
    waiter_->resume_posted_ = true;
    waiter_->loop_->post<HeaderReadAwaiter, &HeaderReadAwaiter::notify_entry_, &HeaderReadAwaiter::on_notify>(*waiter_);
}

} // namespace fiber::http::detail
