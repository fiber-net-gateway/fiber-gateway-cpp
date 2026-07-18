#include "Http2OutboundScheduler.h"

#include <algorithm>

#include "../common/Assert.h"
#include "HttpTransport.h"

namespace fiber::http {

Http2OutboundScheduler::Http2OutboundScheduler(std::uint32_t peer_max_frame_size) noexcept :
    peer_max_frame_size_(peer_max_frame_size) {}

Http2OutboundScheduler::~Http2OutboundScheduler() {
    clear_all_stream_state();
    clear_control_state();
}

bool Http2OutboundScheduler::idle() const noexcept {
    return control_chain_.readable_bytes() == 0 && ready_stream_count_ == 0 && waiting_conn_window_stream_count_ == 0 &&
           inflight_stream_ == nullptr;
}

void Http2OutboundScheduler::bind_owner_loop(event::EventLoop &loop) noexcept {
    FIBER_ASSERT(loop.in_loop());
    if (!owner_loop_) {
        owner_loop_ = &loop;
    } else {
        FIBER_ASSERT(owner_loop_ == &loop);
    }
    if (!control_chain_.bound()) {
        control_chain_.bind_node_pool(loop.io_buf_node_pool());
    } else {
        FIBER_ASSERT(&control_chain_.node_pool() == &loop.io_buf_node_pool());
    }
}

bool Http2OutboundScheduler::bind_owner_loop_if_needed() noexcept {
    fiber::event::EventLoop *current = fiber::event::EventLoop::current_or_null();
    if (!current) {
        return false;
    }
    bind_owner_loop(*current);
    return true;
}

void Http2OutboundScheduler::notify_work() noexcept {
    if (wake_callback_) {
        wake_callback_(wake_ctx_);
    }
}

void Http2OutboundScheduler::clear_control_state() noexcept { control_chain_.clear(); }

void Http2OutboundScheduler::fail(common::IoErr reason) noexcept {
    if (stop_reason_ == common::IoErr::None) {
        stop_reason_ = reason;
    }
    aborting_ = true;
    closed_ = true;
}

void Http2OutboundScheduler::clear_ready_streams() noexcept {
    while (!ready_streams_.empty()) {
        Http2Stream *stream = ready_streams_.front();
        ready_streams_.erase(*stream);
        stream->outbound_hook_.queue_state_ = static_cast<std::uint8_t>(QueueState::None);
        stream->outbound_hook_.next_kind_ = Http2OutboundNextKind::None;
    }
    ready_stream_count_ = 0;
}

void Http2OutboundScheduler::clear_waiting_conn_window_streams() noexcept {
    while (!waiting_conn_window_streams_.empty()) {
        Http2Stream *stream = waiting_conn_window_streams_.front();
        waiting_conn_window_streams_.erase(*stream);
        stream->outbound_hook_.queue_state_ = static_cast<std::uint8_t>(QueueState::None);
        stream->outbound_hook_.next_kind_ = Http2OutboundNextKind::None;
    }
    waiting_conn_window_stream_count_ = 0;
}

void Http2OutboundScheduler::clear_all_stream_state() noexcept {
    clear_ready_streams();
    clear_waiting_conn_window_streams();
    if (inflight_stream_) {
        inflight_stream_->outbound_hook_.queue_state_ = static_cast<std::uint8_t>(QueueState::None);
        inflight_stream_->outbound_hook_.next_kind_ = Http2OutboundNextKind::None;
        inflight_stream_ = nullptr;
    }
    inflight_stream_write_.clear();
}

bool Http2OutboundScheduler::has_ready_work() const noexcept {
    return control_chain_.readable_bytes() != 0 || ready_stream_count_ != 0 ||
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

    if (stream.outbound_hook_.closed_ || stream.outbound_hook_.next_kind_ == Http2OutboundNextKind::None) {
        stream.outbound_hook_.next_kind_ = Http2OutboundNextKind::None;
        return;
    }

    if (stream.outbound_hook_.next_kind_ == Http2OutboundNextKind::Data && conn_send_window_ <= 0) {
        enqueue_waiting_conn_window(stream);
        return;
    }

    enqueue_ready(stream);
    if (notify) {
        notify_work();
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

common::IoErr Http2OutboundScheduler::request_send(Http2Stream &stream, Http2OutboundNextKind next_kind) noexcept {
    (void) bind_owner_loop_if_needed();

    if (stop_reason_ != common::IoErr::None) {
        return stop_reason_;
    }
    if (closed_) {
        return common::IoErr::Canceled;
    }
    if (stream.outbound_hook_.closed_) {
        return common::IoErr::Canceled;
    }
    if (next_kind == Http2OutboundNextKind::None || stream.outbound_operation_ == nullptr) {
        return common::IoErr::Invalid;
    }

    QueueState state = static_cast<QueueState>(stream.outbound_hook_.queue_state_);
    if (state == QueueState::Ready || state == QueueState::WaitingConnWindow ||
        (state == QueueState::Inflight && stream.outbound_hook_.next_kind_ != Http2OutboundNextKind::None)) {
        return common::IoErr::Already;
    }

    stream.outbound_hook_.next_kind_ = next_kind;
    classify_stream(stream, true);
    return common::IoErr::None;
}

bool Http2OutboundScheduler::cancel_queued_send(Http2Stream &stream) noexcept {
    (void) bind_owner_loop_if_needed();

    QueueState state = static_cast<QueueState>(stream.outbound_hook_.queue_state_);
    if (state != QueueState::Ready && state != QueueState::WaitingConnWindow) {
        return false;
    }

    unlink_stream(stream);
    stream.outbound_hook_.next_kind_ = Http2OutboundNextKind::None;
    return true;
}

void Http2OutboundScheduler::cancel_stream(Http2Stream &stream) noexcept {
    (void) bind_owner_loop_if_needed();

    stream.outbound_hook_.closed_ = true;
    stream.outbound_hook_.next_kind_ = Http2OutboundNextKind::None;

    QueueState state = static_cast<QueueState>(stream.outbound_hook_.queue_state_);
    if (state == QueueState::Ready || state == QueueState::WaitingConnWindow) {
        unlink_stream(stream);
    }
}

void Http2OutboundScheduler::on_connection_window_available() noexcept {
    (void) bind_owner_loop_if_needed();

    if (conn_send_window_ > 0 && waiting_conn_window_stream_count_ != 0) {
        notify_work();
    }
}

void Http2OutboundScheduler::finish_inflight_stream_write() noexcept {
    FIBER_ASSERT(inflight_stream_ != nullptr);

    Http2Stream *stream = inflight_stream_;
    inflight_stream_ = nullptr;

    auto &hook = stream->outbound_hook_;
    hook.queue_state_ = static_cast<std::uint8_t>(QueueState::None);
    if (hook.closed_) {
        hook.next_kind_ = Http2OutboundNextKind::None;
        inflight_stream_write_.clear();
        stream->on_outbound_send_complete();
        return;
    }

    classify_stream(*stream, false);
    inflight_stream_write_.clear();
    stream->on_outbound_send_complete();
}

common::IoResult<Http2OutboundPumpResult> Http2OutboundScheduler::pump_write(HttpTransport &transport,
                                                                             std::size_t operation_budget,
                                                                             std::size_t byte_budget) noexcept {
    event::EventLoop &loop = transport.loop();
    FIBER_ASSERT(loop.in_loop());
    bind_owner_loop(loop);
    operation_budget = std::max<std::size_t>(operation_budget, 1);
    byte_budget = std::max<std::size_t>(byte_budget, 1);
    Http2OutboundPumpResult pump_result;
    std::size_t operations = 0;

    auto fail_pump = [&](common::IoErr reason) noexcept -> common::IoResult<Http2OutboundPumpResult> {
        fail(reason);
        clear_all_stream_state();
        clear_control_state();
        stopped_ = true;
        return std::unexpected(stop_reason_);
    };

    auto try_begin_stream_write = [&](Http2Stream *stream) noexcept -> bool {
        if (!stream) {
            return false;
        }

        auto &hook = stream->outbound_hook_;
        if (hook.closed_ || hook.next_kind_ == Http2OutboundNextKind::None) {
            hook.next_kind_ = Http2OutboundNextKind::None;
            hook.queue_state_ = static_cast<std::uint8_t>(QueueState::None);
            return false;
        }
        if (stream->outbound_operation_ == nullptr) {
            fail(common::IoErr::Invalid);
            return false;
        }

        Http2OutboundEncodeRequest req;
        req.max_frame_size = peer_max_frame_size_;
        req.conn_window_budget =
                conn_send_window_ > 0 ? static_cast<std::uint32_t>(conn_send_window_) : std::uint32_t{0};
        req.stream_window_budget =
                stream->send_window() > 0 ? static_cast<std::uint32_t>(stream->send_window()) : std::uint32_t{0};

        inflight_stream_write_.clear();
        Http2OutboundEncodeTarget target;
        target.reset(loop.io_buf_node_pool());
        Http2OutboundEncodeResult result;
        common::IoErr err = stream->encode_outbound_batch(req, target, result);
        if (err != common::IoErr::None) {
            fail(err);
            return false;
        }

        switch (result.status) {
            case Http2OutboundEncodeResult::Status::Encoded:
                if (target.empty()) {
                    fail(common::IoErr::Invalid);
                    return false;
                }
                if (result.flow_controlled_bytes > req.conn_window_budget ||
                    result.flow_controlled_bytes > req.stream_window_budget) {
                    fail(common::IoErr::Invalid);
                    return false;
                }

                inflight_stream_ = stream;
                inflight_stream_write_.chain = target.take_chain();
                hook.queue_state_ = static_cast<std::uint8_t>(QueueState::Inflight);
                hook.next_kind_ = result.next_kind;
                conn_send_window_ -= static_cast<std::int32_t>(result.flow_controlled_bytes);
                stream->update_send_window(-static_cast<std::int32_t>(result.flow_controlled_bytes));
                return true;
            case Http2OutboundEncodeResult::Status::BlockedConnWindow:
                if (!target.empty() || result.flow_controlled_bytes != 0) {
                    fail(common::IoErr::Invalid);
                    return false;
                }
                hook.next_kind_ = result.next_kind != Http2OutboundNextKind::None ? result.next_kind
                                                                                  : Http2OutboundNextKind::Data;
                hook.queue_state_ = static_cast<std::uint8_t>(QueueState::None);
                enqueue_waiting_conn_window(*stream);
                return false;
            case Http2OutboundEncodeResult::Status::NoWork:
                if (!target.empty() || result.flow_controlled_bytes != 0) {
                    fail(common::IoErr::Invalid);
                    return false;
                }
                hook.next_kind_ = result.next_kind;
                hook.queue_state_ = static_cast<std::uint8_t>(QueueState::None);
                return false;
            case Http2OutboundEncodeResult::Status::Closed:
                if (!target.empty() || result.flow_controlled_bytes != 0) {
                    fail(common::IoErr::Invalid);
                    return false;
                }
                hook.closed_ = true;
                hook.next_kind_ = Http2OutboundNextKind::None;
                hook.queue_state_ = static_cast<std::uint8_t>(QueueState::None);
                return false;
        }

        fail(common::IoErr::Invalid);
        return false;
    };

    for (;;) {
        if (aborting_) {
            return fail_pump(stop_reason_ != common::IoErr::None ? stop_reason_ : common::IoErr::Canceled);
        }
        if (closed_ && !has_ready_work()) {
            clear_all_stream_state();
            stopped_ = true;
            pump_result.stopped = true;
            return pump_result;
        }
        if (operations >= operation_budget || pump_result.bytes_written >= byte_budget) {
            pump_result.needs_reschedule = has_ready_work();
            return pump_result;
        }

        if (inflight_stream_) {
            if (inflight_stream_write_.empty()) {
                finish_inflight_stream_write();
                ++operations;
                continue;
            }
            if (!transport.valid()) {
                return fail_pump(common::IoErr::Invalid);
            }
            std::size_t written = 0;
            event::IoEvent wait_event = event::IoEvent::None;
            common::IoErr err = transport.poll_writev(inflight_stream_write_.chain, written, wait_event);
            ++operations;
            if (err == common::IoErr::WouldBlock) {
                if (wait_event != event::IoEvent::Read && wait_event != event::IoEvent::Write) {
                    return fail_pump(common::IoErr::Invalid);
                }
                pump_result.wait_event = wait_event;
                return pump_result;
            }
            if (err != common::IoErr::None) {
                return fail_pump(err);
            }
            if (written == 0) {
                return fail_pump(common::IoErr::ConnReset);
            }
            pump_result.bytes_written += written;
            if (inflight_stream_write_.empty()) {
                finish_inflight_stream_write();
            }
            continue;
        }

        if (control_chain_.readable_bytes() != 0) {
            if (!transport.valid()) {
                return fail_pump(common::IoErr::Invalid);
            }
            std::size_t written = 0;
            event::IoEvent wait_event = event::IoEvent::None;
            common::IoErr err = transport.poll_writev(control_chain_, written, wait_event);
            ++operations;
            if (err == common::IoErr::WouldBlock) {
                if (wait_event != event::IoEvent::Read && wait_event != event::IoEvent::Write) {
                    return fail_pump(common::IoErr::Invalid);
                }
                pump_result.wait_event = wait_event;
                return pump_result;
            }
            if (err != common::IoErr::None) {
                return fail_pump(err);
            }
            if (written == 0) {
                return fail_pump(common::IoErr::ConnReset);
            }
            pump_result.bytes_written += written;
            continue;
        }

        if (ready_stream_count_ != 0) {
            ++operations;
            if (try_begin_stream_write(pop_ready_stream())) {
                continue;
            }
            if (aborting_ || stop_reason_ != common::IoErr::None) {
                return fail_pump(stop_reason_ != common::IoErr::None ? stop_reason_ : common::IoErr::Invalid);
            }
            continue;
        }

        if (conn_send_window_ > 0 && waiting_conn_window_stream_count_ != 0) {
            ++operations;
            if (try_begin_stream_write(pop_waiting_conn_window_stream())) {
                continue;
            }
            if (aborting_ || stop_reason_ != common::IoErr::None) {
                return fail_pump(stop_reason_ != common::IoErr::None ? stop_reason_ : common::IoErr::Invalid);
            }
            continue;
        }

        return pump_result;
    }
}

void Http2OutboundScheduler::close() noexcept {
    if (closed_) {
        return;
    }
    closed_ = true;
    notify_work();
}

void Http2OutboundScheduler::abort(common::IoErr reason) noexcept {
    if (stopped_) {
        return;
    }
    fail(reason);
    clear_all_stream_state();
    clear_control_state();
    stopped_ = true;
    notify_work();
}

} // namespace fiber::http
