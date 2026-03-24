#include "Http2OutboundScheduler.h"

#include <cstring>
#include <new>

#include "../common/Assert.h"
#include "HttpTransport.h"

namespace fiber::http {

struct Http2OutboundScheduler::Slab {
    explicit Slab(std::size_t cap) noexcept : capacity(cap) {}

    [[nodiscard]] std::uint8_t *data() noexcept {
        return reinterpret_cast<std::uint8_t *>(this + 1);
    }

    [[nodiscard]] const std::uint8_t *data() const noexcept {
        return reinterpret_cast<const std::uint8_t *>(this + 1);
    }

    [[nodiscard]] std::size_t writable_bytes() const noexcept {
        return capacity - commit_pos;
    }

    static Slab *allocate(std::size_t capacity) noexcept {
        void *mem = ::operator new(sizeof(Slab) + capacity, std::nothrow);
        if (!mem) {
            return nullptr;
        }
        return new (mem) Slab(capacity);
    }

    static void destroy(Slab *slab) noexcept {
        if (!slab) {
            return;
        }
        slab->~Slab();
        ::operator delete(slab);
    }

    Slab *next = nullptr;
    std::size_t read_pos = 0;
    std::size_t commit_pos = 0;
    std::size_t capacity = 0;
};

Http2OutboundScheduler::Http2OutboundScheduler(HttpTransport *transport, std::size_t slab_capacity,
                                               std::chrono::milliseconds write_timeout,
                                               std::uint32_t peer_max_frame_size) noexcept
    : transport_(transport),
      write_timeout_(write_timeout),
      slab_capacity_(slab_capacity),
      peer_max_frame_size_(peer_max_frame_size) {
}

Http2OutboundScheduler::~Http2OutboundScheduler() {
    FIBER_ASSERT(!send_loop_running_);

    clear_all_stream_state();

    while (head_slab_) {
        Slab *next = head_slab_->next;
        Slab::destroy(head_slab_);
        head_slab_ = next;
    }
    tail_slab_ = nullptr;

    if (cached_empty_slab_) {
        Slab::destroy(cached_empty_slab_);
        cached_empty_slab_ = nullptr;
    }
}

bool Http2OutboundEncodeTarget::empty() const noexcept {
    return slot_used_ == 0 && tail_chain_.readable_bytes() == 0;
}

std::size_t Http2OutboundEncodeTarget::total_bytes() const noexcept {
    return slot_used_ + tail_chain_.readable_bytes();
}

void Http2OutboundEncodeTarget::reset(std::uint8_t *slot, std::size_t capacity) noexcept {
    slot_ = slot;
    slot_capacity_ = capacity;
    slot_used_ = 0;
    slot_reserved_ = 0;
    tail_chain_.clear();
}

common::IoErr Http2OutboundEncodeTarget::reserve_slot(std::size_t bytes, std::uint8_t *&dst) noexcept {
    dst = nullptr;
    if (slot_reserved_ != 0 || bytes == 0) {
        return common::IoErr::Invalid;
    }
    if (tail_chain_.readable_bytes() != 0) {
        return common::IoErr::Invalid;
    }
    if (!slot_ || bytes > slot_available()) {
        return common::IoErr::NoMem;
    }
    dst = slot_ + slot_used_;
    slot_reserved_ = bytes;
    return common::IoErr::None;
}

void Http2OutboundEncodeTarget::commit_slot(std::size_t bytes) noexcept {
    FIBER_ASSERT(slot_reserved_ != 0);
    FIBER_ASSERT(bytes <= slot_reserved_);
    slot_used_ += bytes;
    slot_reserved_ = 0;
}

void Http2OutboundEncodeTarget::rollback_slot() noexcept {
    slot_reserved_ = 0;
}

common::IoErr Http2OutboundEncodeTarget::append_copy(const void *src, std::size_t bytes) noexcept {
    if (!src || bytes == 0 || slot_reserved_ != 0) {
        return common::IoErr::Invalid;
    }

    if (tail_chain_.readable_bytes() == 0 && bytes <= slot_available()) {
        std::uint8_t *dst = nullptr;
        common::IoErr err = reserve_slot(bytes, dst);
        if (err != common::IoErr::None) {
            return err;
        }
        std::memcpy(dst, src, bytes);
        commit_slot(bytes);
        return common::IoErr::None;
    }

    mem::IoBuf buf = mem::IoBuf::allocate(bytes);
    if (!buf) {
        return common::IoErr::NoMem;
    }
    std::memcpy(buf.writable_data(), src, bytes);
    buf.commit(bytes);
    return tail_chain_.append(std::move(buf)) ? common::IoErr::None : common::IoErr::NoMem;
}

common::IoErr Http2OutboundEncodeTarget::append_buffer(mem::IoBuf &&buf) noexcept {
    if (!buf || buf.readable() == 0 || slot_reserved_ != 0) {
        return common::IoErr::Invalid;
    }
    return tail_chain_.append(std::move(buf)) ? common::IoErr::None : common::IoErr::NoMem;
}

common::IoErr Http2OutboundEncodeTarget::append_chain(mem::IoBufChain &&chain) noexcept {
    if (chain.empty() || chain.readable_bytes() == 0 || slot_reserved_ != 0) {
        return common::IoErr::Invalid;
    }

    const std::size_t bytes = chain.readable_bytes();
    return chain.take_prefix(bytes, tail_chain_) ? common::IoErr::None : common::IoErr::NoMem;
}

void Http2OutboundEncodeTarget::clear() noexcept {
    slot_used_ = 0;
    slot_reserved_ = 0;
    tail_chain_.clear();
}

Http2OutboundScheduler::WaitForWorkAwaiter::~WaitForWorkAwaiter() {
    if (!queue_) {
        return;
    }
    queue_->cancel_waiter(this);
}

bool Http2OutboundScheduler::WaitForWorkAwaiter::await_ready() const noexcept {
    return queue_ == nullptr || queue_->should_wake_waiter();
}

bool Http2OutboundScheduler::WaitForWorkAwaiter::await_suspend(std::coroutine_handle<> handle) noexcept {
    if (!queue_) {
        return false;
    }

    loop_ = fiber::event::EventLoop::current_or_null();
    FIBER_ASSERT(loop_ != nullptr);
    handle_ = handle;
    return queue_->arm_waiter(this);
}

void Http2OutboundScheduler::WaitForWorkAwaiter::await_resume() noexcept {
    if (!queue_) {
        return;
    }
    if (queue_->waiter_ == this) {
        queue_->waiter_ = nullptr;
    }
    queue_ = nullptr;
    loop_ = nullptr;
    handle_ = {};
    resume_posted_ = false;
}

void Http2OutboundScheduler::WaitForWorkAwaiter::on_notify(WaitForWorkAwaiter *awaiter) {
    if (!awaiter) {
        return;
    }
    awaiter->resume_posted_ = false;
    awaiter->resume();
}

void Http2OutboundScheduler::WaitForWorkAwaiter::resume() noexcept {
    auto handle = handle_;
    handle_ = {};
    if (handle) {
        handle.resume();
    }
}

bool Http2OutboundScheduler::idle() const noexcept {
    return pending_control_bytes_ == 0 && ready_stream_count_ == 0 && waiting_conn_window_stream_count_ == 0 &&
           inflight_stream_ == nullptr;
}

std::size_t Http2OutboundScheduler::active_slab_count() const noexcept {
    std::size_t count = 0;
    for (Slab *slab = head_slab_; slab; slab = slab->next) {
        ++count;
    }
    return count;
}

void Http2OutboundScheduler::bind_owner_loop_if_needed() noexcept {
    fiber::event::EventLoop *current = fiber::event::EventLoop::current_or_null();
    if (!current) {
        return;
    }
    if (!owner_loop_) {
        owner_loop_ = current;
        return;
    }
    FIBER_ASSERT(owner_loop_ == current);
}

common::IoErr Http2OutboundScheduler::reserve_tail(std::size_t bytes, Reservation &reservation) noexcept {
    if (bytes == 0 || bytes > slab_capacity_) {
        return common::IoErr::Invalid;
    }
    if (stop_reason_ != common::IoErr::None) {
        return stop_reason_;
    }
    if (closed_) {
        return common::IoErr::Canceled;
    }

    Slab *slab = tail_slab_;
    if (!slab || slab->writable_bytes() < bytes) {
        slab = acquire_slab();
        if (!slab) {
            return common::IoErr::NoMem;
        }
        append_tail_slab(slab);
    }

    reservation.slab = slab;
    reservation.data = slab->data() + slab->commit_pos;
    reservation.begin = slab->commit_pos;
    reservation.bytes = bytes;
    return common::IoErr::None;
}

void Http2OutboundScheduler::rollback_reservation(const Reservation &) noexcept {
}

void Http2OutboundScheduler::commit_reservation(const Reservation &reservation) noexcept {
    FIBER_ASSERT(reservation.slab != nullptr);
    FIBER_ASSERT(reservation.begin == reservation.slab->commit_pos);
    reservation.slab->commit_pos += reservation.bytes;
    pending_control_bytes_ += reservation.bytes;
}

bool Http2OutboundScheduler::arm_waiter(WaitForWorkAwaiter *awaiter) noexcept {
    if (!awaiter || should_wake_waiter()) {
        return false;
    }

    FIBER_ASSERT(waiter_ == nullptr);
    waiter_ = awaiter;
    return true;
}

void Http2OutboundScheduler::cancel_waiter(WaitForWorkAwaiter *awaiter) noexcept {
    if (waiter_ == awaiter) {
        waiter_ = nullptr;
    }
}

void Http2OutboundScheduler::notify_waiter() noexcept {
    if (!waiter_ || waiter_->resume_posted_ || waiter_->loop_ == nullptr) {
        return;
    }

    waiter_->resume_posted_ = true;
    waiter_->loop_->post<WaitForWorkAwaiter, &WaitForWorkAwaiter::notify_entry_, &WaitForWorkAwaiter::on_notify>(
        *waiter_);
}

bool Http2OutboundScheduler::should_wake_waiter() const noexcept {
    return pending_control_bytes_ != 0 || ready_stream_count_ != 0 || waiting_conn_window_stream_count_ != 0 ||
           inflight_stream_ != nullptr || closed_ || aborting_ || stop_reason_ != common::IoErr::None;
}

Http2OutboundScheduler::Slab *Http2OutboundScheduler::acquire_slab() noexcept {
    if (cached_empty_slab_) {
        Slab *slab = cached_empty_slab_;
        cached_empty_slab_ = nullptr;
        slab->next = nullptr;
        slab->read_pos = 0;
        slab->commit_pos = 0;
        return slab;
    }

    return Slab::allocate(slab_capacity_);
}

void Http2OutboundScheduler::recycle_slab(Slab *slab) noexcept {
    if (!slab) {
        return;
    }

    slab->next = nullptr;
    slab->read_pos = 0;
    slab->commit_pos = 0;
    if (!cached_empty_slab_) {
        cached_empty_slab_ = slab;
        return;
    }

    Slab::destroy(slab);
}

void Http2OutboundScheduler::append_tail_slab(Slab *slab) noexcept {
    FIBER_ASSERT(slab != nullptr);
    slab->next = nullptr;
    if (tail_slab_) {
        tail_slab_->next = slab;
    } else {
        head_slab_ = slab;
    }
    tail_slab_ = slab;
}

void Http2OutboundScheduler::discard_empty_head_slabs() noexcept {
    while (head_slab_ && head_slab_->read_pos == head_slab_->commit_pos) {
        Slab *next = head_slab_->next;
        recycle_slab(head_slab_);
        head_slab_ = next;
        if (!head_slab_) {
            tail_slab_ = nullptr;
        }
        sending_end_ = 0;
    }
}

Http2OutboundScheduler::SendSpan Http2OutboundScheduler::current_send_span() noexcept {
    discard_empty_head_slabs();
    FIBER_ASSERT(head_slab_ != nullptr);
    if (sending_end_ <= head_slab_->read_pos) {
        sending_end_ = head_slab_->commit_pos;
    }
    FIBER_ASSERT(sending_end_ > head_slab_->read_pos);
    return {head_slab_->data() + head_slab_->read_pos, sending_end_ - head_slab_->read_pos};
}

void Http2OutboundScheduler::consume_written_control_bytes(std::size_t bytes) noexcept {
    FIBER_ASSERT(head_slab_ != nullptr);
    FIBER_ASSERT(bytes <= pending_control_bytes_);
    FIBER_ASSERT(head_slab_->read_pos + bytes <= sending_end_);

    head_slab_->read_pos += bytes;
    pending_control_bytes_ -= bytes;
    if (head_slab_->read_pos == sending_end_) {
        sending_end_ = head_slab_->read_pos;
    }
    discard_empty_head_slabs();
}

void Http2OutboundScheduler::fail(common::IoErr reason) noexcept {
    if (stop_reason_ == common::IoErr::None) {
        stop_reason_ = reason;
    }
    aborting_ = true;
    closed_ = true;
    notify_waiter();
}

void Http2OutboundScheduler::clear_ready_streams() noexcept {
    while (!ready_streams_.empty()) {
        Http2Stream *stream = ready_streams_.front();
        ready_streams_.erase(*stream);
        stream->outbound_hook_.queue_state_ = static_cast<std::uint8_t>(QueueState::None);
        stream->outbound_hook_.next_kind_ = Http2OutboundNextKind::None;
        stream->outbound_hook_.encode_ = nullptr;
        stream->outbound_hook_.encode_ctx_ = nullptr;
    }
    ready_stream_count_ = 0;
}

void Http2OutboundScheduler::clear_waiting_conn_window_streams() noexcept {
    while (!waiting_conn_window_streams_.empty()) {
        Http2Stream *stream = waiting_conn_window_streams_.front();
        waiting_conn_window_streams_.erase(*stream);
        stream->outbound_hook_.queue_state_ = static_cast<std::uint8_t>(QueueState::None);
        stream->outbound_hook_.next_kind_ = Http2OutboundNextKind::None;
        stream->outbound_hook_.encode_ = nullptr;
        stream->outbound_hook_.encode_ctx_ = nullptr;
    }
    waiting_conn_window_stream_count_ = 0;
}

void Http2OutboundScheduler::clear_all_stream_state() noexcept {
    clear_ready_streams();
    clear_waiting_conn_window_streams();
    if (inflight_stream_) {
        inflight_stream_->outbound_hook_.queue_state_ = static_cast<std::uint8_t>(QueueState::None);
        inflight_stream_->outbound_hook_.next_kind_ = Http2OutboundNextKind::None;
        inflight_stream_->outbound_hook_.encode_ = nullptr;
        inflight_stream_->outbound_hook_.encode_ctx_ = nullptr;
        inflight_stream_ = nullptr;
    }
    inflight_stream_write_.clear();
}

bool Http2OutboundScheduler::has_ready_work() const noexcept {
    return pending_control_bytes_ != 0 || ready_stream_count_ != 0 ||
           (conn_send_window_ > 0 && waiting_conn_window_stream_count_ != 0) || inflight_stream_ != nullptr;
}

void Http2OutboundScheduler::enqueue_ready(Http2Stream &stream) noexcept {
    FIBER_ASSERT(static_cast<QueueState>(stream.outbound_hook_.queue_state_) == QueueState::None);
    ready_streams_.push_back(stream);
    stream.outbound_hook_.queue_state_ = static_cast<std::uint8_t>(QueueState::Ready);
    ++ready_stream_count_;
}

void Http2OutboundScheduler::enqueue_waiting_conn_window(Http2Stream &stream) noexcept {
    FIBER_ASSERT(static_cast<QueueState>(stream.outbound_hook_.queue_state_) == QueueState::None);
    waiting_conn_window_streams_.push_back(stream);
    stream.outbound_hook_.queue_state_ = static_cast<std::uint8_t>(QueueState::WaitingConnWindow);
    ++waiting_conn_window_stream_count_;
}

void Http2OutboundScheduler::unlink_stream(Http2Stream &stream) noexcept {
    QueueState state = static_cast<QueueState>(stream.outbound_hook_.queue_state_);
    if (state == QueueState::Ready) {
        ready_streams_.erase(stream);
        FIBER_ASSERT(ready_stream_count_ != 0);
        --ready_stream_count_;
    } else if (state == QueueState::WaitingConnWindow) {
        waiting_conn_window_streams_.erase(stream);
        FIBER_ASSERT(waiting_conn_window_stream_count_ != 0);
        --waiting_conn_window_stream_count_;
    }
    stream.outbound_hook_.queue_state_ = static_cast<std::uint8_t>(QueueState::None);
}

void Http2OutboundScheduler::classify_stream(Http2Stream &stream, bool notify) noexcept {
    QueueState state = static_cast<QueueState>(stream.outbound_hook_.queue_state_);
    if (state == QueueState::Inflight) {
        return;
    }

    if (state != QueueState::None) {
        unlink_stream(stream);
    }

    if (stream.outbound_hook_.closed_ || stream.outbound_hook_.encode_ == nullptr ||
        stream.outbound_hook_.next_kind_ == Http2OutboundNextKind::None) {
        stream.outbound_hook_.encode_ = nullptr;
        stream.outbound_hook_.encode_ctx_ = nullptr;
        stream.outbound_hook_.next_kind_ = Http2OutboundNextKind::None;
        return;
    }

    if (stream.outbound_hook_.next_kind_ == Http2OutboundNextKind::Data && conn_send_window_ <= 0) {
        enqueue_waiting_conn_window(stream);
        return;
    }

    enqueue_ready(stream);
    if (notify) {
        notify_waiter();
    }
}

Http2Stream *Http2OutboundScheduler::pop_ready_stream() noexcept {
    Http2Stream *stream = ready_streams_.front();
    if (!stream) {
        return nullptr;
    }

    ready_streams_.erase(*stream);
    FIBER_ASSERT(ready_stream_count_ != 0);
    --ready_stream_count_;
    stream->outbound_hook_.queue_state_ = static_cast<std::uint8_t>(QueueState::None);
    return stream;
}

Http2Stream *Http2OutboundScheduler::pop_waiting_conn_window_stream() noexcept {
    Http2Stream *stream = waiting_conn_window_streams_.front();
    if (!stream) {
        return nullptr;
    }

    waiting_conn_window_streams_.erase(*stream);
    FIBER_ASSERT(waiting_conn_window_stream_count_ != 0);
    --waiting_conn_window_stream_count_;
    stream->outbound_hook_.queue_state_ = static_cast<std::uint8_t>(QueueState::None);
    return stream;
}

common::IoErr Http2OutboundScheduler::request_send(Http2Stream &stream, Http2OutboundNextKind next_kind,
                                                   Http2OutboundEncodeFn encode, void *ctx) noexcept {
    bind_owner_loop_if_needed();

    if (stop_reason_ != common::IoErr::None) {
        return stop_reason_;
    }
    if (closed_) {
        return common::IoErr::Canceled;
    }
    if (stream.outbound_hook_.closed_) {
        return common::IoErr::Canceled;
    }
    if (next_kind != Http2OutboundNextKind::None && encode == nullptr) {
        return common::IoErr::Invalid;
    }

    stream.outbound_hook_.encode_ = encode;
    stream.outbound_hook_.encode_ctx_ = ctx;
    stream.outbound_hook_.next_kind_ = next_kind;
    classify_stream(stream, true);
    return common::IoErr::None;
}

void Http2OutboundScheduler::cancel_stream(Http2Stream &stream) noexcept {
    bind_owner_loop_if_needed();

    stream.outbound_hook_.closed_ = true;
    stream.outbound_hook_.next_kind_ = Http2OutboundNextKind::None;
    stream.outbound_hook_.encode_ = nullptr;
    stream.outbound_hook_.encode_ctx_ = nullptr;

    QueueState state = static_cast<QueueState>(stream.outbound_hook_.queue_state_);
    if (state == QueueState::Ready || state == QueueState::WaitingConnWindow) {
        unlink_stream(stream);
    }
}

void Http2OutboundScheduler::on_connection_window_available() noexcept {
    bind_owner_loop_if_needed();

    if (conn_send_window_ > 0 && waiting_conn_window_stream_count_ != 0) {
        notify_waiter();
    }
}

void Http2OutboundScheduler::finish_inflight_stream_write() noexcept {
    FIBER_ASSERT(inflight_stream_ != nullptr);

    Http2Stream *stream = inflight_stream_;
    inflight_stream_ = nullptr;
    inflight_stream_write_.clear();

    auto &hook = stream->outbound_hook_;
    hook.queue_state_ = static_cast<std::uint8_t>(QueueState::None);
    if (hook.closed_) {
        hook.next_kind_ = Http2OutboundNextKind::None;
        hook.encode_ = nullptr;
        hook.encode_ctx_ = nullptr;
        stream->on_outbound_send_complete();
        return;
    }

    classify_stream(*stream, false);
    stream->on_outbound_send_complete();
}

fiber::async::Task<void> Http2OutboundScheduler::send_loop() noexcept {
    fiber::event::EventLoop *loop = fiber::event::EventLoop::current_or_null();
    FIBER_ASSERT(loop != nullptr);
    if (!owner_loop_) {
        owner_loop_ = loop;
    } else {
        FIBER_ASSERT(owner_loop_ == loop);
    }
    FIBER_ASSERT(!send_loop_running_);

    auto try_begin_stream_write = [&](Http2Stream *stream) noexcept -> bool {
        if (!stream) {
            return false;
        }

        auto &hook = stream->outbound_hook_;
        if (hook.closed_ || hook.next_kind_ == Http2OutboundNextKind::None || hook.encode_ == nullptr) {
            hook.next_kind_ = Http2OutboundNextKind::None;
            hook.encode_ = nullptr;
            hook.encode_ctx_ = nullptr;
            hook.queue_state_ = static_cast<std::uint8_t>(QueueState::None);
            return false;
        }

        Http2OutboundEncodeRequest req;
        req.max_frame_size = peer_max_frame_size_;
        req.conn_window_budget = conn_send_window_ > 0 ? conn_send_window_ : 0;

        inflight_stream_write_.clear();
        Http2OutboundEncodeTarget target;
        target.reset(inflight_stream_write_.slot, sizeof(inflight_stream_write_.slot));
        Http2OutboundEncodeResult result;
        common::IoErr err = hook.encode_(*stream, hook.encode_ctx_, req, target, result);
        if (err != common::IoErr::None) {
            fail(err);
            return false;
        }
        if (target.slot_reserved_ != 0) {
            fail(common::IoErr::Invalid);
            return false;
        }

        switch (result.status) {
            case Http2OutboundEncodeResult::Status::Encoded:
                if (target.empty()) {
                    fail(common::IoErr::Invalid);
                    return false;
                }
                if (result.consumed_conn_window > static_cast<std::uint32_t>(req.conn_window_budget)) {
                    fail(common::IoErr::Invalid);
                    return false;
                }

                inflight_stream_ = stream;
                inflight_stream_write_.stream = stream;
                inflight_stream_write_.slot_size = target.slot_used_;
                inflight_stream_write_.slot_written = 0;
                inflight_stream_write_.tail_chain = target.take_tail_chain();
                hook.queue_state_ = static_cast<std::uint8_t>(QueueState::Inflight);
                hook.next_kind_ = result.next_kind;
                conn_send_window_ -= static_cast<std::int32_t>(result.consumed_conn_window);
                stream->update_send_window(-static_cast<std::int32_t>(result.consumed_conn_window));
                return true;
            case Http2OutboundEncodeResult::Status::BlockedConnWindow:
                if (!target.empty() || result.consumed_conn_window != 0) {
                    fail(common::IoErr::Invalid);
                    return false;
                }
                hook.next_kind_ =
                    result.next_kind != Http2OutboundNextKind::None ? result.next_kind : Http2OutboundNextKind::Data;
                hook.queue_state_ = static_cast<std::uint8_t>(QueueState::None);
                enqueue_waiting_conn_window(*stream);
                return false;
            case Http2OutboundEncodeResult::Status::BlockedStreamWindow:
                if (!target.empty() || result.consumed_conn_window != 0) {
                    fail(common::IoErr::Invalid);
                    return false;
                }
                hook.next_kind_ = result.next_kind;
                hook.queue_state_ = static_cast<std::uint8_t>(QueueState::None);
                return false;
            case Http2OutboundEncodeResult::Status::NoWork:
                if (!target.empty() || result.consumed_conn_window != 0) {
                    fail(common::IoErr::Invalid);
                    return false;
                }
                hook.next_kind_ = result.next_kind;
                hook.queue_state_ = static_cast<std::uint8_t>(QueueState::None);
                if (hook.next_kind_ == Http2OutboundNextKind::None) {
                    hook.encode_ = nullptr;
                    hook.encode_ctx_ = nullptr;
                }
                return false;
            case Http2OutboundEncodeResult::Status::Closed:
                if (!target.empty() || result.consumed_conn_window != 0) {
                    fail(common::IoErr::Invalid);
                    return false;
                }
                hook.closed_ = true;
                hook.next_kind_ = Http2OutboundNextKind::None;
                hook.encode_ = nullptr;
                hook.encode_ctx_ = nullptr;
                hook.queue_state_ = static_cast<std::uint8_t>(QueueState::None);
                return false;
        }

        fail(common::IoErr::Invalid);
        return false;
    };

    send_loop_running_ = true;
    for (;;) {
        discard_empty_head_slabs();

        if (aborting_) {
            break;
        }

        if (inflight_stream_) {
            if (inflight_stream_write_.empty()) {
                finish_inflight_stream_write();
                continue;
            }
            if (!transport_ || !transport_->valid()) {
                fail(common::IoErr::Invalid);
                break;
            }
            common::IoResult<size_t> result;
            if (!inflight_stream_write_.slot_done()) {
                const std::size_t remaining =
                    inflight_stream_write_.slot_size - inflight_stream_write_.slot_written;
                result = co_await transport_->write(inflight_stream_write_.slot + inflight_stream_write_.slot_written,
                                                    remaining, write_timeout_);
                if (result) {
                    inflight_stream_write_.slot_written += *result;
                }
            } else {
                result = co_await transport_->writev(inflight_stream_write_.tail_chain, write_timeout_);
            }
            if (!result) {
                fail(result.error());
                break;
            }
            if (*result == 0) {
                fail(common::IoErr::ConnReset);
                break;
            }
            if (inflight_stream_write_.empty()) {
                finish_inflight_stream_write();
            }
            continue;
        }

        if (pending_control_bytes_ != 0) {
            if (!transport_ || !transport_->valid()) {
                fail(common::IoErr::Invalid);
                break;
            }
            SendSpan span = current_send_span();
            common::IoResult<size_t> result = co_await transport_->write(span.data, span.length, write_timeout_);
            if (!result) {
                fail(result.error());
                break;
            }
            if (*result == 0) {
                fail(common::IoErr::ConnReset);
                break;
            }
            consume_written_control_bytes(*result);
            continue;
        }

        if (ready_stream_count_ != 0) {
            if (try_begin_stream_write(pop_ready_stream())) {
                continue;
            }
            if (aborting_ || stop_reason_ != common::IoErr::None) {
                break;
            }
            continue;
        }

        if (conn_send_window_ > 0 && waiting_conn_window_stream_count_ != 0) {
            if (try_begin_stream_write(pop_waiting_conn_window_stream())) {
                continue;
            }
            if (aborting_ || stop_reason_ != common::IoErr::None) {
                break;
            }
            continue;
        }

        if (closed_ && !has_ready_work()) {
            break;
        }

        co_await wait_for_work();
    }

    send_loop_running_ = false;
    if (aborting_) {
        clear_all_stream_state();
    } else if (closed_ && !has_ready_work()) {
        clear_all_stream_state();
    }
    co_return;
}

void Http2OutboundScheduler::close() noexcept {
    closed_ = true;
    notify_waiter();
}

void Http2OutboundScheduler::abort(common::IoErr reason) noexcept {
    fail(reason);
}

} // namespace fiber::http
