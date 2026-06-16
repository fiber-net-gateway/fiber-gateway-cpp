#include "QuicStreamSendQueue.h"

#include <expected>

#include "../common/Assert.h"

namespace fiber::quic {

class QuicStreamSendQueue::AppendAwaiter {
public:
    AppendAwaiter(QuicStreamSendQueue &queue, std::size_t bytes,
                  std::chrono::steady_clock::time_point deadline) noexcept :
        queue_(&queue), bytes_(bytes), deadline_(deadline) {}

    AppendAwaiter(const AppendAwaiter &) = delete;
    AppendAwaiter &operator=(const AppendAwaiter &) = delete;
    AppendAwaiter(AppendAwaiter &&) = delete;
    AppendAwaiter &operator=(AppendAwaiter &&) = delete;

    ~AppendAwaiter() {
        cancel_timer();
        if (queue_ != nullptr) {
            queue_->cancel_append_waiter(this);
        }
    }

    bool await_ready() noexcept {
        if (queue_ == nullptr || queue_->can_append_now(bytes_)) {
            return true;
        }
        if (timed_out(std::chrono::steady_clock::now())) {
            result_ = common::IoErr::TimedOut;
            completed_ = true;
            return true;
        }
        return false;
    }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
        if (queue_ == nullptr || queue_->can_append_now(bytes_)) {
            return false;
        }
        loop_ = event::EventLoop::current_or_null();
        FIBER_ASSERT(loop_ != nullptr);
        if (timed_out(loop_->now())) {
            result_ = common::IoErr::TimedOut;
            completed_ = true;
            loop_ = nullptr;
            return false;
        }
        if (queue_->append_waiter_ != nullptr) {
            result_ = common::IoErr::Busy;
            completed_ = true;
            loop_ = nullptr;
            return false;
        }

        handle_ = handle;
        queue_->append_waiter_ = this;
        arm_timer();
        return true;
    }

    common::IoErr await_resume() noexcept {
        common::IoErr result = result_;
        cancel_timer();
        if (queue_ != nullptr && queue_->append_waiter_ == this) {
            queue_->append_waiter_ = nullptr;
        }
        queue_ = nullptr;
        loop_ = nullptr;
        handle_ = {};
        result_ = common::IoErr::None;
        resume_posted_ = false;
        completed_ = false;
        return result;
    }

    [[nodiscard]] bool should_resume() const noexcept { return queue_ == nullptr || queue_->can_append_now(bytes_); }

    void complete(common::IoErr result) noexcept {
        if (completed_) {
            return;
        }
        completed_ = true;
        result_ = result;
        cancel_timer();
        post_resume();
    }

private:
    [[nodiscard]] bool has_timer() const noexcept { return deadline_ != std::chrono::steady_clock::time_point::max(); }

    [[nodiscard]] bool timed_out(std::chrono::steady_clock::time_point now) const noexcept {
        return has_timer() && now >= deadline_;
    }

    void arm_timer() noexcept {
        if (!has_timer() || loop_ == nullptr) {
            return;
        }
        loop_->post_at<AppendAwaiter, &AppendAwaiter::timer_entry_, &AppendAwaiter::on_timeout>(deadline_, *this);
    }

    void cancel_timer() noexcept {
        if (loop_ != nullptr && timer_entry_.is_in_heap()) {
            loop_->cancel<AppendAwaiter, &AppendAwaiter::timer_entry_>(*this);
        }
    }

    static void on_notify(AppendAwaiter *awaiter) noexcept {
        if (awaiter == nullptr) {
            return;
        }
        awaiter->resume_posted_ = false;
        auto handle = awaiter->handle_;
        awaiter->handle_ = {};
        if (handle) {
            handle.resume();
        }
    }

    static void on_timeout(AppendAwaiter *awaiter) noexcept {
        if (awaiter == nullptr) {
            return;
        }
        awaiter->complete(common::IoErr::TimedOut);
    }

    void post_resume() noexcept {
        if (resume_posted_ || loop_ == nullptr) {
            return;
        }
        resume_posted_ = true;
        loop_->post<AppendAwaiter, &AppendAwaiter::notify_entry_, &AppendAwaiter::on_notify>(*this);
    }

    QuicStreamSendQueue *queue_ = nullptr;
    std::size_t bytes_ = 0;
    std::chrono::steady_clock::time_point deadline_{std::chrono::steady_clock::time_point::max()};
    event::EventLoop *loop_ = nullptr;
    std::coroutine_handle<> handle_{};
    event::EventLoop::NotifyEntry notify_entry_{};
    event::EventLoop::TimerEntry timer_entry_{};
    common::IoErr result_ = common::IoErr::None;
    bool resume_posted_ = false;
    bool completed_ = false;
};

QuicStreamSendQueue::QuicStreamSendQueue(mem::IoBufNodePool &pool) noexcept : QuicStreamSendQueue(pool, Options{}) {}

QuicStreamSendQueue::QuicStreamSendQueue(mem::IoBufNodePool &pool, Options options) noexcept :
    buffer_(pool), buffer_limit_(options.buffer_limit), max_stream_data_(options.max_stream_data) {}

QuicStreamSendQueue::~QuicStreamSendQueue() {
    FIBER_ASSERT(append_waiter_ == nullptr);
    close();
}

common::IoResult<std::size_t> QuicStreamSendQueue::try_append(const mem::IoBuf &buf, bool fin) noexcept {
    const std::size_t bytes = buf.readable();
    auto checked = check_append_preconditions(bytes);
    if (!checked) {
        return std::unexpected(checked.error());
    }
    return buffer_.append(buf, fin);
}

common::IoResult<std::size_t> QuicStreamSendQueue::try_append_chain(mem::IoBufChain &chain) noexcept {
    if (!chain.bound()) {
        return std::unexpected(common::IoErr::Invalid);
    }

    const std::size_t bytes = chain.readable_bytes();
    auto checked = check_append_preconditions(bytes);
    if (!checked) {
        return std::unexpected(checked.error());
    }
    return buffer_.append_chain(chain);
}

async::Task<common::IoResult<std::size_t>> QuicStreamSendQueue::append(mem::IoBuf buf, bool fin,
                                                                       std::chrono::milliseconds timeout) noexcept {
    const std::size_t bytes = buf.readable();
    if (bytes > buffer_limit_) {
        co_return std::unexpected(common::IoErr::MessageTooLarge);
    }
    if (timeout < std::chrono::milliseconds::zero()) {
        timeout = std::chrono::milliseconds::zero();
    }

    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
    bool deadline_set = timeout == std::chrono::milliseconds::max();

    for (;;) {
        auto appended = try_append(buf, fin);
        if (appended || appended.error() != common::IoErr::WouldBlock) {
            co_return appended;
        }
        if (timeout == std::chrono::milliseconds::zero()) {
            co_return std::unexpected(common::IoErr::TimedOut);
        }
        if (!deadline_set) {
            auto *loop = event::EventLoop::current_or_null();
            FIBER_ASSERT(loop != nullptr);
            deadline = loop->now() + timeout;
            deadline_set = true;
        }

        common::IoErr wait_result = co_await AppendAwaiter(*this, bytes, deadline);
        if (wait_result != common::IoErr::None) {
            co_return std::unexpected(wait_result);
        }
    }
}

async::Task<common::IoResult<std::size_t>>
QuicStreamSendQueue::append_chain(mem::IoBufChain &chain, std::chrono::milliseconds timeout) noexcept {
    if (!chain.bound()) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    const std::size_t bytes = chain.readable_bytes();
    if (bytes > buffer_limit_) {
        co_return std::unexpected(common::IoErr::MessageTooLarge);
    }
    if (timeout < std::chrono::milliseconds::zero()) {
        timeout = std::chrono::milliseconds::zero();
    }

    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
    bool deadline_set = timeout == std::chrono::milliseconds::max();

    for (;;) {
        auto appended = try_append_chain(chain);
        if (appended || appended.error() != common::IoErr::WouldBlock) {
            co_return appended;
        }
        if (timeout == std::chrono::milliseconds::zero()) {
            co_return std::unexpected(common::IoErr::TimedOut);
        }
        if (!deadline_set) {
            auto *loop = event::EventLoop::current_or_null();
            FIBER_ASSERT(loop != nullptr);
            deadline = loop->now() + timeout;
            deadline_set = true;
        }

        common::IoErr wait_result = co_await AppendAwaiter(*this, bytes, deadline);
        if (wait_result != common::IoErr::None) {
            co_return std::unexpected(wait_result);
        }
    }
}

common::IoResult<QuicStreamSendBuffer::EncodedFrameResult>
QuicStreamSendQueue::encode_stream_frame(std::uint64_t stream_id, std::uint8_t *dst, std::size_t capacity) noexcept {
    return buffer_.encode_stream_frame(stream_id, dst, capacity);
}

common::IoResult<void> QuicStreamSendQueue::mark_acked(std::size_t offset, std::size_t length,
                                                       bool encoded_fin) noexcept {
    auto acked = buffer_.mark_acked(offset, length, encoded_fin);
    if (acked) {
        notify_append_waiter();
    }
    return acked;
}

common::IoResult<void> QuicStreamSendQueue::mark_failed(std::size_t offset, std::size_t length,
                                                        bool encoded_fin) noexcept {
    return buffer_.mark_failed(offset, length, encoded_fin);
}

common::IoResult<std::uint64_t> QuicStreamSendQueue::mark_reset() noexcept {
    auto reset = buffer_.mark_reset();
    if (reset) {
        notify_append_waiter(common::IoErr::BrokenPipe);
    }
    return reset;
}

void QuicStreamSendQueue::update_max_stream_data(std::uint64_t limit) noexcept {
    if (limit <= max_stream_data_) {
        return;
    }
    max_stream_data_ = limit;
    notify_append_waiter();
}

void QuicStreamSendQueue::close(common::IoErr reason) noexcept {
    if (closed_) {
        return;
    }
    closed_ = true;
    close_reason_ = reason == common::IoErr::None ? common::IoErr::Canceled : reason;
    notify_append_waiter(close_reason_);
}

std::size_t QuicStreamSendQueue::buffer_available() const noexcept {
    if (buffer_.buffered_bytes() >= buffer_limit_) {
        return 0;
    }
    return buffer_limit_ - buffer_.buffered_bytes();
}

std::uint64_t QuicStreamSendQueue::stream_data_available() const noexcept {
    const std::uint64_t appended = buffer_.total_appended_bytes();
    if (appended >= max_stream_data_) {
        return 0;
    }
    return max_stream_data_ - appended;
}

bool QuicStreamSendQueue::can_append_now(std::size_t bytes) const noexcept {
    if (terminal_append_error() != common::IoErr::None) {
        return true;
    }
    if (bytes > buffer_limit_) {
        return true;
    }
    return buffer_available() >= bytes && stream_data_available() >= bytes;
}

common::IoErr QuicStreamSendQueue::terminal_append_error() const noexcept {
    if (closed_) {
        return close_reason_ == common::IoErr::None ? common::IoErr::Canceled : close_reason_;
    }
    if (buffer_.reset()) {
        return common::IoErr::BrokenPipe;
    }
    return common::IoErr::None;
}

common::IoResult<void> QuicStreamSendQueue::check_append_preconditions(std::size_t bytes) const noexcept {
    const common::IoErr terminal = terminal_append_error();
    if (terminal != common::IoErr::None) {
        return std::unexpected(terminal);
    }
    if (bytes > buffer_limit_) {
        return std::unexpected(common::IoErr::MessageTooLarge);
    }
    if (buffer_available() < bytes || stream_data_available() < bytes) {
        return std::unexpected(common::IoErr::WouldBlock);
    }
    return {};
}

void QuicStreamSendQueue::notify_append_waiter(common::IoErr result) noexcept {
    AppendAwaiter *waiter = append_waiter_;
    if (waiter == nullptr) {
        return;
    }
    if (result != common::IoErr::None || waiter->should_resume()) {
        waiter->complete(result);
    }
}

void QuicStreamSendQueue::cancel_append_waiter(AppendAwaiter *awaiter) noexcept {
    if (append_waiter_ == awaiter) {
        append_waiter_ = nullptr;
    }
}

} // namespace fiber::quic
