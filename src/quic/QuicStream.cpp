#include "QuicStream.h"

#include <algorithm>
#include <coroutine>
#include <cstddef>
#include <expected>
#include <limits>
#include <utility>

#include "../common/Assert.h"
#include "../event/EventLoop.h"
#include "QuicConnection.h"

namespace fiber::quic {

namespace {

constexpr std::uint64_t kStreamTypeMask = 0x02;

} // namespace

class QuicStream::WriteAwaiter {
public:
    WriteAwaiter(QuicStream &stream, std::chrono::steady_clock::time_point deadline) noexcept :
        stream_(&stream), deadline_(deadline) {}

    WriteAwaiter(const WriteAwaiter &) = delete;
    WriteAwaiter &operator=(const WriteAwaiter &) = delete;
    WriteAwaiter(WriteAwaiter &&) = delete;
    WriteAwaiter &operator=(WriteAwaiter &&) = delete;

    ~WriteAwaiter() {
        cancel_timer();
        unlink_connection_window_wait();
        if (stream_ != nullptr) {
            stream_->cancel_write_waiter(this);
        }
    }

    bool await_ready() noexcept {
        if (stream_ == nullptr || should_resume()) {
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
        if (stream_ == nullptr || should_resume()) {
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
        if (stream_->write_waiter_ != nullptr) {
            result_ = common::IoErr::Busy;
            completed_ = true;
            loop_ = nullptr;
            return false;
        }

        handle_ = handle;
        stream_->write_waiter_ = this;
        maybe_wait_for_connection_window();
        arm_timer();
        return true;
    }

    common::IoErr await_resume() noexcept {
        common::IoErr result = result_;
        cancel_timer();
        unlink_connection_window_wait();
        if (stream_ != nullptr && stream_->write_waiter_ == this) {
            stream_->write_waiter_ = nullptr;
        }
        stream_ = nullptr;
        loop_ = nullptr;
        handle_ = {};
        result_ = common::IoErr::None;
        resume_posted_ = false;
        completed_ = false;
        return result;
    }

    [[nodiscard]] bool should_resume() const noexcept {
        return stream_ == nullptr || stream_->terminal_write_error() != common::IoErr::None ||
               stream_->write_available() > 0;
    }

    void complete(common::IoErr result) noexcept {
        if (completed_) {
            return;
        }
        completed_ = true;
        result_ = result;
        cancel_timer();
        unlink_connection_window_wait();
        post_resume();
    }

    void maybe_wait_for_connection_window() noexcept {
        if (stream_ == nullptr) {
            return;
        }
        if (!stream_->blocked_by_connection_window()) {
            unlink_connection_window_wait();
            return;
        }
        stream_->conn_->wait_for_peer_data(*this);
    }

    void unlink_connection_window_wait() noexcept {
        if (stream_ == nullptr || stream_->conn_ == nullptr || !peer_data_wait_link_.linked()) {
            return;
        }
        stream_->conn_->cancel_peer_data_wait(*this);
    }

private:
    [[nodiscard]] static WriteAwaiter *from_peer_data_wait_link(common::IntrusiveListHook *hook) noexcept {
        if (hook == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<WriteAwaiter *>(reinterpret_cast<std::uint8_t *>(hook) -
                                                offsetof(WriteAwaiter, peer_data_wait_link_));
    }

    [[nodiscard]] bool has_timer() const noexcept { return deadline_ != std::chrono::steady_clock::time_point::max(); }

    [[nodiscard]] bool timed_out(std::chrono::steady_clock::time_point now) const noexcept {
        return has_timer() && now >= deadline_;
    }

    void arm_timer() noexcept {
        if (!has_timer() || loop_ == nullptr) {
            return;
        }
        loop_->post_at<WriteAwaiter, &WriteAwaiter::timer_entry_, &WriteAwaiter::on_timeout>(deadline_, *this);
    }

    void cancel_timer() noexcept {
        if (loop_ != nullptr && timer_entry_.is_in_heap()) {
            loop_->cancel<WriteAwaiter, &WriteAwaiter::timer_entry_>(*this);
        }
    }

    static void on_notify(WriteAwaiter *awaiter) noexcept {
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

    static void on_timeout(WriteAwaiter *awaiter) noexcept {
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
        loop_->post<WriteAwaiter, &WriteAwaiter::notify_entry_, &WriteAwaiter::on_notify>(*this);
    }

    QuicStream *stream_ = nullptr;
    std::chrono::steady_clock::time_point deadline_{std::chrono::steady_clock::time_point::max()};
    event::EventLoop *loop_ = nullptr;
    std::coroutine_handle<> handle_{};
    event::EventLoop::NotifyEntry notify_entry_{};
    event::EventLoop::TimerEntry timer_entry_{};
    common::IntrusiveListHook peer_data_wait_link_{};
    common::IoErr result_ = common::IoErr::None;
    bool resume_posted_ = false;
    bool completed_ = false;

    friend class QuicConnection;
};

void QuicConnection::wait_for_peer_data(QuicStream::WriteAwaiter &awaiter) noexcept {
    common::IntrusiveListHook &hook = awaiter.peer_data_wait_link_;
    if (hook.linked()) {
        return;
    }

    hook.prev = peer_data_wait_tail_;
    hook.next = nullptr;
    if (peer_data_wait_tail_ != nullptr) {
        peer_data_wait_tail_->next = &hook;
    } else {
        peer_data_wait_head_ = &hook;
    }
    peer_data_wait_tail_ = &hook;
    hook.in_list = true;
}

void QuicConnection::cancel_peer_data_wait(QuicStream::WriteAwaiter &awaiter) noexcept {
    common::IntrusiveListHook &hook = awaiter.peer_data_wait_link_;
    if (!hook.linked()) {
        return;
    }

    if (hook.prev != nullptr) {
        hook.prev->next = hook.next;
    } else {
        peer_data_wait_head_ = hook.next;
    }
    if (hook.next != nullptr) {
        hook.next->prev = hook.prev;
    } else {
        peer_data_wait_tail_ = hook.prev;
    }
    hook.prev = nullptr;
    hook.next = nullptr;
    hook.in_list = false;
}

void QuicConnection::notify_peer_data_waiters(common::IoErr result) noexcept {
    while (peer_data_wait_head_ != nullptr) {
        QuicStream::WriteAwaiter *awaiter = QuicStream::WriteAwaiter::from_peer_data_wait_link(peer_data_wait_head_);
        cancel_peer_data_wait(*awaiter);
        awaiter->complete(result);
    }
}

void QuicStream::Lease::reset() noexcept {
    if (!stream_) {
        return;
    }
    QuicStream *stream = stream_;
    stream_ = nullptr;
    stream->release();
}

QuicStream::QuicStream(std::uint64_t stream_id, mem::IoBufNodePool &recv_extent_pool) noexcept :
    stream_id_(stream_id), recv_queue_(recv_extent_pool), send_queue_(recv_extent_pool) {}

QuicStream::QuicStream(std::uint64_t stream_id, mem::IoBufNodePool &recv_extent_pool,
                       QuicStreamRecvQueue::Options recv_options) noexcept :
    stream_id_(stream_id), recv_queue_(recv_extent_pool, recv_options), send_queue_(recv_extent_pool) {}

QuicStream::~QuicStream() = default;

std::uint64_t QuicStream::sequence() const noexcept { return stream_sequence(stream_id_); }

QuicStreamType QuicStream::type() const noexcept {
    return bidirectional() ? QuicStreamType::Bidirectional : QuicStreamType::Unidirectional;
}

bool QuicStream::bidirectional() const noexcept { return is_bidirectional_stream_id(stream_id_); }

bool QuicStream::unidirectional() const noexcept { return is_unidirectional_stream_id(stream_id_); }

common::IoResult<std::size_t> QuicStream::try_read(std::size_t max_bytes, mem::IoBufChain &out) noexcept {
    auto taken = recv_queue_.try_take(max_bytes, out);
    if (!taken) {
        return std::unexpected(taken.error());
    }
    sync_recv_state_from_queue();
    maybe_extend_recv_flow_control();
    return *taken;
}

async::Task<common::IoResult<std::size_t>> QuicStream::read(std::size_t max_bytes, mem::IoBufChain &out,
                                                            std::chrono::milliseconds timeout) noexcept {
    auto taken = co_await recv_queue_.take(max_bytes, out, timeout);
    if (!taken) {
        co_return std::unexpected(taken.error());
    }
    sync_recv_state_from_queue();
    maybe_extend_recv_flow_control();
    co_return *taken;
}

common::IoResult<std::size_t> QuicStream::try_write(const mem::IoBuf &buf, bool fin) noexcept {
    const std::size_t bytes = buf.readable();
    const common::IoErr terminal = terminal_write_error();
    if (terminal != common::IoErr::None) {
        return std::unexpected(terminal);
    }

    mem::IoBuf append_buf = buf;
    bool append_fin = fin;
    if (bytes > 0) {
        const std::size_t available = write_available();
        if (available == 0) {
            return std::unexpected(common::IoErr::WouldBlock);
        }
        const std::size_t append_bytes = std::min(bytes, available);
        append_fin = fin && append_bytes == bytes;
        if (append_bytes < bytes) {
            append_buf = buf.retain_slice(0, append_bytes);
            if (!append_buf) {
                return std::unexpected(common::IoErr::NoMem);
            }
        }
    }

    auto appended = send_queue_.try_append(append_buf, append_fin);
    if (appended && *appended > 0 && conn_ != nullptr) {
        const bool reserved = conn_->reserve_peer_data(*appended);
        FIBER_ASSERT(reserved);
    }
    if (appended && conn_ != nullptr && has_send_work()) {
        (void) conn_->queue_stream_frame(*this);
    }
    return appended;
}

common::IoResult<std::size_t> QuicStream::try_write(mem::IoBufChain &chain) noexcept {
    if (!chain.bound() || &chain.node_pool() != &send_queue_.node_pool()) {
        return std::unexpected(common::IoErr::Invalid);
    }

    const std::size_t bytes = chain.readable_bytes();
    const common::IoErr terminal = terminal_write_error();
    if (terminal != common::IoErr::None) {
        return std::unexpected(terminal);
    }

    mem::IoBufChain *append_chain = &chain;
    mem::IoBufChain prefix(chain.node_pool());
    if (bytes > 0) {
        const std::size_t available = write_available();
        if (available == 0) {
            return std::unexpected(common::IoErr::WouldBlock);
        }
        const std::size_t append_bytes = std::min(bytes, available);
        if (append_bytes < bytes) {
            if (!chain.take_prefix(append_bytes, prefix)) {
                return std::unexpected(common::IoErr::NoMem);
            }
            append_chain = &prefix;
        }
    }

    auto appended = send_queue_.try_append_chain(*append_chain);
    if (appended && *appended > 0 && conn_ != nullptr) {
        const bool reserved = conn_->reserve_peer_data(*appended);
        FIBER_ASSERT(reserved);
    }
    if (appended && conn_ != nullptr && has_send_work()) {
        (void) conn_->queue_stream_frame(*this);
    }
    return appended;
}

async::Task<common::IoResult<std::size_t>> QuicStream::write(mem::IoBuf buf, bool fin,
                                                             std::chrono::milliseconds timeout) noexcept {
    if (timeout < std::chrono::milliseconds::zero()) {
        timeout = std::chrono::milliseconds::zero();
    }

    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
    bool deadline_set = timeout == std::chrono::milliseconds::max();

    for (;;) {
        auto written = try_write(buf, fin);
        if (written || written.error() != common::IoErr::WouldBlock) {
            co_return written;
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

        common::IoErr wait_result = co_await WriteAwaiter(*this, deadline);
        if (wait_result != common::IoErr::None) {
            co_return std::unexpected(wait_result);
        }
    }
}

async::Task<common::IoResult<std::size_t>> QuicStream::write(mem::IoBufChain &chain,
                                                             std::chrono::milliseconds timeout) noexcept {
    if (timeout < std::chrono::milliseconds::zero()) {
        timeout = std::chrono::milliseconds::zero();
    }

    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
    bool deadline_set = timeout == std::chrono::milliseconds::max();

    for (;;) {
        auto written = try_write(chain);
        if (written || written.error() != common::IoErr::WouldBlock) {
            co_return written;
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

        common::IoErr wait_result = co_await WriteAwaiter(*this, deadline);
        if (wait_result != common::IoErr::None) {
            co_return std::unexpected(wait_result);
        }
    }
}

common::IoResult<void> QuicStream::stop_read(std::uint64_t error_code) noexcept {
    if (recv_queue_.stop_sending()) {
        return {};
    }
    if (conn_ != nullptr) {
        auto queued = conn_->queue_stop_sending_frame(stream_id_, error_code);
        if (!queued) {
            return std::unexpected(queued.error());
        }
    }
    recv_queue_.stop_receiving(error_code);
    sync_recv_state_from_queue();
    return {};
}

common::IoResult<void> QuicStream::reset(std::uint64_t error_code) noexcept {
    auto final_size = send_queue_.reset(error_code);
    if (!final_size) {
        return std::unexpected(final_size.error());
    }
    notify_write_waiter(common::IoErr::BrokenPipe);
    if (conn_ == nullptr) {
        return {};
    }
    return conn_->queue_reset_stream_frame(stream_id_, error_code, *final_size);
}

common::IoResult<std::uint64_t> QuicStream::on_stream_data_recv(const std::uint8_t *src, std::size_t length,
                                                                std::uint64_t offset, bool fin) noexcept {
    const std::uint64_t old_end = recv_queue_.received_end_offset();
    QuicSlice data{src, length};

    auto inserted = recv_queue_.recv_stream_data(offset, data, fin);
    if (!inserted) {
        return std::unexpected(inserted.error());
    }
    sync_recv_state_from_queue();
    return recv_queue_.received_end_offset() - old_end;
}

common::IoResult<std::uint64_t> QuicStream::on_remote_reset(std::uint64_t error_code,
                                                            std::uint64_t final_size) noexcept {
    const std::uint64_t old_end = recv_queue_.received_end_offset();
    auto reset = recv_queue_.recv_reset(error_code, final_size);
    if (!reset) {
        return std::unexpected(reset.error());
    }
    sync_recv_state_from_queue();
    return recv_queue_.received_end_offset() - old_end;
}

common::IoResult<void> QuicStream::on_remote_stop_sending(std::uint64_t error_code) noexcept {
    return reset(error_code);
}

void QuicStream::on_max_stream_data(std::uint64_t limit) noexcept {
    if (limit > max_stream_data_) {
        max_stream_data_ = limit;
    }
    notify_write_waiter();
    if (conn_ != nullptr && has_send_work()) {
        (void) conn_->queue_stream_frame(*this);
    }
}

bool QuicStream::has_send_work() const noexcept { return send_queue_.has_send_work(); }

common::IoResult<QuicStreamFrameEncodeStatus> QuicStream::encode_stream_frame(QuicOutputFrame &frame, std::uint8_t *dst,
                                                                              std::size_t capacity) noexcept {
    if (!attached_to_connection_ || !has_send_work()) {
        stream_send_pending_ = false;
        return QuicStreamFrameEncodeStatus::Skipped;
    }

    auto encoded = send_queue_.encode_stream_frame(stream_id_, dst, capacity);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    if (!encoded->encoded) {
        return QuicStreamFrameEncodeStatus::Blocked;
    }

    stream_send_pending_ = false;
    frame.type = QuicFrameType::Stream;
    frame.encoded_len = encoded->encoded_len;
    frame.u.stream.length = static_cast<std::uint32_t>(encoded->data_len);
    frame.u.stream.stream_id = stream_id_;
    frame.u.stream.offset = encoded->offset;
    frame.u.stream.has_length = encoded->has_length;
    frame.u.stream.fin = encoded->fin;

    if (has_send_work() && conn_ != nullptr) {
        (void) conn_->queue_stream_frame(*this);
    }
    return QuicStreamFrameEncodeStatus::Encoded;
}

common::IoResult<void> QuicStream::mark_send_acked(std::size_t offset, std::size_t length, bool fin) noexcept {
    auto acked = send_queue_.mark_acked(offset, length, fin);
    if (acked) {
        notify_write_waiter();
    }
    return acked;
}

common::IoResult<void> QuicStream::mark_send_failed(std::size_t offset, std::size_t length, bool fin) noexcept {
    return send_queue_.mark_failed(offset, length, fin);
}

void QuicStream::maybe_extend_recv_flow_control() noexcept {
    if (conn_ == nullptr || !recv_queue_.should_extend_max_stream_data()) {
        return;
    }
    const std::uint64_t limit = recv_queue_.next_max_stream_data_limit();
    auto queued = conn_->queue_max_stream_data_frame(stream_id_, limit);
    if (queued) {
        recv_queue_.update_max_stream_data(limit);
    }
}

void QuicStream::mark_app_released() noexcept { app_released_ = true; }

bool QuicStream::ready_for_connection_release() const noexcept {
    return app_released_ && (recv_queue_.finished() || recv_queue_.reset_received()) && send_queue_.empty() &&
           !stream_send_pending_;
}

bool QuicStream::ready_for_destruction() const noexcept { return !attached_to_connection_ && ref_count_ == 0; }

std::uint64_t QuicStream::stream_sequence(std::uint64_t stream_id) noexcept { return stream_id >> 2U; }

bool QuicStream::is_bidirectional_stream_id(std::uint64_t stream_id) noexcept {
    return (stream_id & kStreamTypeMask) == 0;
}

bool QuicStream::is_unidirectional_stream_id(std::uint64_t stream_id) noexcept {
    return !is_bidirectional_stream_id(stream_id);
}

void QuicStream::attach_to_connection(QuicConnection &conn) noexcept {
    conn_ = &conn;
    attached_to_connection_ = true;
    max_stream_data_ = std::max(max_stream_data_, conn.initial_stream_send_limit(stream_id_));
}

void QuicStream::detach_from_connection() noexcept {
    conn_ = nullptr;
    attached_to_connection_ = false;
}

void QuicStream::retain() noexcept { ++ref_count_; }

void QuicStream::release() noexcept {
    --ref_count_;
    if (ready_for_destruction()) {
        delete this;
    }
}

void QuicStream::sync_recv_state_from_queue() noexcept {
    if (recv_queue_.reset_received()) {
        recv_state_ = QuicStreamRecvState::ResetRecvd;
        return;
    }
    if (recv_queue_.stop_sending()) {
        recv_state_ = QuicStreamRecvState::Stopped;
        return;
    }
    if (recv_queue_.finished()) {
        recv_state_ = QuicStreamRecvState::Closed;
        return;
    }
    if (recv_queue_.has_final_size()) {
        recv_state_ = QuicStreamRecvState::SizeKnown;
        return;
    }
    recv_state_ = QuicStreamRecvState::Open;
}

std::uint64_t QuicStream::stream_data_available() const noexcept {
    const std::uint64_t appended = send_queue_.total_appended_bytes();
    if (appended >= max_stream_data_) {
        return 0;
    }
    return max_stream_data_ - appended;
}

std::size_t QuicStream::write_available() const noexcept {
    std::uint64_t available = send_queue_.buffer_available();
    available = std::min(available, stream_data_available());
    if (conn_ != nullptr) {
        available = std::min(available, conn_->peer_data_available());
    }
    if (available > std::numeric_limits<std::size_t>::max()) {
        return std::numeric_limits<std::size_t>::max();
    }
    return static_cast<std::size_t>(available);
}

bool QuicStream::blocked_by_connection_window() const noexcept {
    if (conn_ == nullptr || terminal_write_error() != common::IoErr::None) {
        return false;
    }
    return send_queue_.buffer_available() > 0 && stream_data_available() > 0 && conn_->peer_data_available() == 0;
}

common::IoErr QuicStream::terminal_write_error() const noexcept {
    if (send_queue_.reset_sent()) {
        return common::IoErr::BrokenPipe;
    }
    if (conn_ != nullptr && conn_->closing()) {
        return common::IoErr::Canceled;
    }
    return common::IoErr::None;
}

void QuicStream::notify_write_waiter(common::IoErr result) noexcept {
    WriteAwaiter *waiter = write_waiter_;
    if (waiter == nullptr) {
        return;
    }
    if (result != common::IoErr::None || waiter->should_resume()) {
        waiter->complete(result);
        return;
    }
    waiter->maybe_wait_for_connection_window();
}

void QuicStream::cancel_write_waiter(WriteAwaiter *awaiter) noexcept {
    if (write_waiter_ == awaiter) {
        write_waiter_ = nullptr;
    }
}

} // namespace fiber::quic
