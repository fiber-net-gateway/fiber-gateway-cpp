#include "QuicStreamSendQueue.h"

#include <algorithm>
#include <expected>
#include <utility>

#include "../common/Assert.h"
#include "../event/EventLoop.h"
#include "QuicCursor.h"
#include "QuicTransportCodec.h"

namespace fiber::quic {

namespace {

constexpr std::uint8_t kStreamFrameFin = 0x01;
constexpr std::uint8_t kStreamFrameLen = 0x02;
constexpr std::uint8_t kStreamFrameOff = 0x04;

} // namespace

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
    pool_(&pool), buffer_limit_(options.buffer_limit), max_stream_data_(options.max_stream_data) {}

QuicStreamSendQueue::~QuicStreamSendQueue() {
    FIBER_ASSERT(append_waiter_ == nullptr);
    clear_extents();
}

common::IoResult<std::size_t> QuicStreamSendQueue::try_append(const mem::IoBuf &buf, bool fin) noexcept {
    const std::size_t bytes = buf.readable();
    auto checked = check_append_preconditions(bytes);
    if (!checked) {
        return std::unexpected(checked.error());
    }

    if (fin_acked_) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (fin_appended_ && (bytes > 0 || fin)) {
        return std::unexpected(common::IoErr::Invalid);
    }

    if (bytes > 0) {
        mem::IoBuf slice = buf.retain_slice(0, bytes);
        if (!slice) {
            return std::unexpected(common::IoErr::NoMem);
        }

        mem::IoBufNode *extent = pool_->alloc();
        if (extent == nullptr) {
            return std::unexpected(common::IoErr::NoMem);
        }

        extent->offset = total_appended_bytes_;
        extent->state = static_cast<std::uint8_t>(QuicSendExtentState::Ready);

        if (tail_ != nullptr && tail_->buf && slice.same_storage(tail_->buf) &&
            tail_->buf.try_merge_adjacent(std::move(slice))) {
            pool_->release(extent);
        } else {
            extent->buf = std::move(slice);
            extent->next = nullptr;
            if (tail_ != nullptr) {
                tail_->next = extent;
            } else {
                head_ = extent;
            }
            tail_ = extent;
            if (ready_head_ == nullptr) {
                ready_head_ = extent;
            }
            ++active_extent_count_;
        }

        ready_bytes_ += bytes;
        total_appended_bytes_ += bytes;
    }

    if (fin) {
        fin_appended_ = true;
        final_size_ = total_appended_bytes_;
    }

    return bytes;
}

common::IoResult<std::size_t> QuicStreamSendQueue::try_append_chain(mem::IoBufChain &chain) noexcept {
    if (!chain.bound() || &chain.node_pool() != pool_) {
        return std::unexpected(common::IoErr::Invalid);
    }

    const bool chain_complete = chain.complete();
    const std::size_t bytes = chain.readable_bytes();
    auto checked = check_append_preconditions(bytes);
    if (!checked) {
        return std::unexpected(checked.error());
    }

    if (fin_acked_) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (fin_appended_ && (bytes > 0 || chain_complete)) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::size_t appended = 0;
    for (;;) {
        mem::IoBufNode *extent = chain.pop_front_node();
        if (extent == nullptr) {
            break;
        }

        const std::size_t readable = extent->buf.readable();
        if (readable == 0) {
            pool_->release(extent);
            continue;
        }

        extent->offset = total_appended_bytes_;
        extent->state = static_cast<std::uint8_t>(QuicSendExtentState::Ready);
        extent->next = nullptr;

        if (tail_ != nullptr && tail_->buf && extent->buf.same_storage(tail_->buf) &&
            tail_->buf.try_merge_adjacent(std::move(extent->buf))) {
            pool_->release(extent);
        } else {
            if (tail_ != nullptr) {
                tail_->next = extent;
            } else {
                head_ = extent;
            }
            tail_ = extent;
            if (ready_head_ == nullptr) {
                ready_head_ = extent;
            }
            ++active_extent_count_;
        }

        ready_bytes_ += readable;
        total_appended_bytes_ += readable;
        appended += readable;
    }

    if (chain_complete) {
        fin_appended_ = true;
        final_size_ = total_appended_bytes_;
        chain.clear_complete();
    }

    return appended;
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
    if (!chain.bound() || &chain.node_pool() != pool_) {
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

common::IoResult<QuicStreamSendQueue::EncodedFrameResult>
QuicStreamSendQueue::encode_stream_frame(std::uint64_t stream_id, std::uint8_t *dst, std::size_t capacity) noexcept {
    EncodedFrameResult result{};
    if (reset_sent_) {
        return result;
    }

    mem::IoBufNode *cur = ready_head_;

    if (cur == nullptr) {
        if (!has_pending_fin() || buffered_bytes() > 0) {
            return result;
        }

        const std::uint64_t offset = total_appended_bytes_ - buffered_bytes();
        std::size_t base_hdr = 1 + quic_varint_len(stream_id);
        if (offset > 0) {
            base_hdr += quic_varint_len(offset);
        }
        if (capacity < base_hdr) {
            return result;
        }

        QuicWriteCursor wc(dst, capacity);
        std::uint8_t type_byte = 0x08;
        if (offset > 0) {
            type_byte |= kStreamFrameOff;
        }
        type_byte |= kStreamFrameFin;
        auto r = wc.write_u8(type_byte);
        if (!r) {
            return std::unexpected(r.error());
        }
        r = quic_write_varint(wc, stream_id);
        if (!r) {
            return std::unexpected(r.error());
        }
        if (offset > 0) {
            r = quic_write_varint(wc, offset);
            if (!r) {
                return std::unexpected(r.error());
            }
        }

        result.offset = static_cast<std::size_t>(offset);
        result.data_len = 0;
        result.encoded_len = wc.offset();
        result.has_length = false;
        result.fin = true;
        result.encoded = true;
        fin_inflight_ = true;
        return result;
    }

    const std::size_t data_len = cur->buf.readable();
    std::size_t base_hdr = 1 + quic_varint_len(stream_id);
    if (cur->offset > 0) {
        base_hdr += quic_varint_len(cur->offset);
    }

    bool use_len = false;
    std::size_t actual_data = data_len;

    if (base_hdr < capacity) {
        const std::size_t remaining = capacity - base_hdr;
        const std::size_t len_bytes = quic_varint_len(data_len);

        if (len_bytes + data_len <= remaining) {
            use_len = true;
            actual_data = data_len;
        } else if (data_len >= remaining) {
            use_len = false;
            actual_data = remaining;
        } else if (remaining >= 2) {
            use_len = true;
            std::size_t max_data = remaining - 1;
            for (int i = 0; i < 2; ++i) {
                std::size_t revised = remaining - quic_varint_len(max_data);
                if (revised == max_data) {
                    break;
                }
                max_data = revised;
            }
            if (max_data == 0) {
                return result;
            }
            actual_data = std::min(max_data, data_len);
        } else {
            return result;
        }
    } else {
        return result;
    }

    bool encode_fin = false;
    if (fin_appended_ && !fin_inflight_ && actual_data == data_len && is_last_ready_extent(cur)) {
        encode_fin = true;
    }

    if (actual_data < data_len) {
        mem::IoBufNode *remainder = pool_->alloc();
        if (remainder == nullptr) {
            return std::unexpected(common::IoErr::NoMem);
        }
        remainder->offset = cur->offset + actual_data;
        remainder->state = static_cast<std::uint8_t>(QuicSendExtentState::Ready);
        remainder->buf = cur->buf.retain_slice(actual_data, data_len - actual_data);
        remainder->next = cur->next;
        cur->buf = cur->buf.retain_slice(0, actual_data);
        cur->next = remainder;
        if (cur == tail_) {
            tail_ = remainder;
        }
        ++active_extent_count_;
    }

    QuicWriteCursor wc(dst, capacity);
    std::uint8_t type_byte = 0x08;
    if (cur->offset > 0) {
        type_byte |= kStreamFrameOff;
    }
    if (use_len) {
        type_byte |= kStreamFrameLen;
    }
    if (encode_fin) {
        type_byte |= kStreamFrameFin;
    }

    auto r = wc.write_u8(type_byte);
    if (!r) {
        return std::unexpected(r.error());
    }
    r = quic_write_varint(wc, stream_id);
    if (!r) {
        return std::unexpected(r.error());
    }
    if (cur->offset > 0) {
        r = quic_write_varint(wc, cur->offset);
        if (!r) {
            return std::unexpected(r.error());
        }
    }
    if (use_len) {
        r = quic_write_varint(wc, actual_data);
        if (!r) {
            return std::unexpected(r.error());
        }
    }
    r = wc.write_bytes(cur->buf.readable_data(), actual_data);
    if (!r) {
        return std::unexpected(r.error());
    }

    cur->state = static_cast<std::uint8_t>(QuicSendExtentState::Inflight);
    ready_bytes_ -= actual_data;
    inflight_bytes_ += actual_data;

    while (ready_head_ != nullptr && ready_head_->state == static_cast<std::uint8_t>(QuicSendExtentState::Inflight)) {
        ready_head_ = ready_head_->next;
    }

    if (encode_fin) {
        fin_inflight_ = true;
    }

    result.offset = static_cast<std::size_t>(cur->offset);
    result.data_len = actual_data;
    result.encoded_len = wc.offset();
    result.has_length = use_len;
    result.fin = encode_fin;
    result.encoded = true;
    return result;
}

common::IoResult<void> QuicStreamSendQueue::mark_acked(std::size_t offset, std::size_t length,
                                                       bool encoded_fin) noexcept {
    if (reset_sent_) {
        return {};
    }

    const std::size_t end = offset + length;
    mem::IoBufNode *prev = nullptr;
    mem::IoBufNode *cur = head_;

    while (cur != nullptr) {
        mem::IoBufNode *next = cur->next;
        const std::size_t extent_end = static_cast<std::size_t>(cur->offset) + cur->buf.readable();

        if (cur->state == static_cast<std::uint8_t>(QuicSendExtentState::Inflight) &&
            static_cast<std::size_t>(cur->offset) >= offset && extent_end <= end) {

            if (prev != nullptr) {
                prev->next = next;
            } else {
                head_ = next;
            }
            if (cur == tail_) {
                tail_ = prev;
            }

            inflight_bytes_ -= cur->buf.readable();
            pool_->release(cur);
            --active_extent_count_;
        } else {
            prev = cur;
        }

        cur = next;
    }

    ready_head_ = head_;
    while (ready_head_ != nullptr && ready_head_->state == static_cast<std::uint8_t>(QuicSendExtentState::Inflight)) {
        ready_head_ = ready_head_->next;
    }

    if (encoded_fin) {
        fin_acked_ = true;
        fin_inflight_ = false;
    }

    notify_append_waiter();
    return {};
}

common::IoResult<void> QuicStreamSendQueue::mark_failed(std::size_t offset, std::size_t length,
                                                        bool encoded_fin) noexcept {
    if (reset_sent_) {
        return {};
    }

    const std::size_t end = offset + length;
    mem::IoBufNode *cur = head_;

    while (cur != nullptr) {
        mem::IoBufNode *next = cur->next;
        const std::size_t extent_end = static_cast<std::size_t>(cur->offset) + cur->buf.readable();

        if (cur->state == static_cast<std::uint8_t>(QuicSendExtentState::Inflight) &&
            static_cast<std::size_t>(cur->offset) >= offset && extent_end <= end) {

            cur->state = static_cast<std::uint8_t>(QuicSendExtentState::Ready);
            const std::size_t bytes = cur->buf.readable();
            inflight_bytes_ -= bytes;
            ready_bytes_ += bytes;

            try_merge_with_next(cur);
        }

        cur = next;
    }

    ready_head_ = head_;
    while (ready_head_ != nullptr && ready_head_->state == static_cast<std::uint8_t>(QuicSendExtentState::Inflight)) {
        ready_head_ = ready_head_->next;
    }

    if (encoded_fin) {
        fin_inflight_ = false;
    }

    return {};
}

common::IoResult<std::uint64_t> QuicStreamSendQueue::reset(std::uint64_t error_code) noexcept {
    if (reset_sent_) {
        return final_size_;
    }
    if (fin_appended_) {
        return std::unexpected(common::IoErr::Invalid);
    }

    final_size_ = total_appended_bytes_;
    reset_error_code_ = error_code;
    reset_sent_ = true;
    fin_inflight_ = false;
    fin_acked_ = false;
    clear_extents();
    notify_append_waiter(common::IoErr::BrokenPipe);
    return final_size_;
}

void QuicStreamSendQueue::update_max_stream_data(std::uint64_t limit) noexcept {
    if (limit <= max_stream_data_) {
        return;
    }
    max_stream_data_ = limit;
    notify_append_waiter();
}

std::size_t QuicStreamSendQueue::buffer_available() const noexcept {
    if (buffered_bytes() >= buffer_limit_) {
        return 0;
    }
    return buffer_limit_ - buffered_bytes();
}

std::uint64_t QuicStreamSendQueue::stream_data_available() const noexcept {
    if (total_appended_bytes_ >= max_stream_data_) {
        return 0;
    }
    return max_stream_data_ - total_appended_bytes_;
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
    if (reset_sent_) {
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

bool QuicStreamSendQueue::is_last_ready_extent(const mem::IoBufNode *extent) const noexcept {
    if (extent->next != nullptr) {
        return false;
    }
    return ready_head_ == extent || ready_head_ == nullptr;
}

void QuicStreamSendQueue::clear_extents() noexcept {
    mem::IoBufNode *cur = head_;
    while (cur != nullptr) {
        mem::IoBufNode *next = cur->next;
        pool_->release(cur);
        cur = next;
    }
    head_ = nullptr;
    tail_ = nullptr;
    ready_head_ = nullptr;
    ready_bytes_ = 0;
    inflight_bytes_ = 0;
    active_extent_count_ = 0;
}

void QuicStreamSendQueue::try_merge_with_next(mem::IoBufNode *extent) noexcept {
    mem::IoBufNode *next = extent->next;
    if (next == nullptr) {
        return;
    }
    if (extent->state != static_cast<std::uint8_t>(QuicSendExtentState::Ready) ||
        next->state != static_cast<std::uint8_t>(QuicSendExtentState::Ready)) {
        return;
    }
    if (static_cast<std::size_t>(extent->offset) + extent->buf.readable() != static_cast<std::size_t>(next->offset)) {
        return;
    }
    if (!extent->buf.same_storage(next->buf)) {
        return;
    }
    if (!extent->buf.try_merge_adjacent(std::move(next->buf))) {
        return;
    }

    extent->next = next->next;
    if (next == tail_) {
        tail_ = extent;
    }
    pool_->release(next);
    --active_extent_count_;
}

} // namespace fiber::quic
