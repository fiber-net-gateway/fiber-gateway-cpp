#include "Http2Stream.h"

#include "../common/Assert.h"
#include "Http2Connection.h"

namespace fiber::http {

Http2Stream::Http2Stream(void *owner, const Ops &ops) noexcept : owner_(owner), ops_(&ops) {
    FIBER_ASSERT(owner_ != nullptr);
    FIBER_ASSERT(ops_ != nullptr);
    FIBER_ASSERT(ops_->on_destroy != nullptr);
    FIBER_ASSERT(ops_->on_header_block_start != nullptr);
    FIBER_ASSERT(ops_->on_header_block_complete != nullptr);
    FIBER_ASSERT(ops_->on_body != nullptr);
}

common::IoErr Http2Stream::on_headers_payload_recv(const mem::IoBuf &payload, bool block_start, bool end_headers,
                                                   bool end_stream) noexcept {
    if (remote_rst_ || local_rst_ || remote_end_stream_) {
        return common::IoErr::Invalid;
    }
    if (!conn_) {
        return common::IoErr::Invalid;
    }
    if (block_start) {
        Http2HpackDecoder::Sink sink;
        common::IoErr err = ops_->on_header_block_start(owner_, sink);
        if (err != common::IoErr::None) {
            return err;
        }
        if (!sink.ops) {
            return common::IoErr::Invalid;
        }
        conn_->inbound_hpack_decoder().begin_block(sink.ctx, sink.ops);
    }

    if (payload.readable() != 0) {
        common::IoErr err =
                conn_->inbound_hpack_decoder().decode(payload.readable_data(), payload.readable(), end_headers);
        if (err != common::IoErr::None) {
            return err;
        }
    } else if (end_headers) {
        common::IoErr err = conn_->inbound_hpack_decoder().decode(nullptr, 0, true);
        if (err != common::IoErr::None) {
            return err;
        }
    }
    if (end_headers) {
        common::IoErr err = ops_->on_header_block_complete(owner_, end_stream);
        if (err != common::IoErr::None) {
            return err;
        }
    }

    if (end_stream) {
        remote_end_stream_ = true;
    }
    return common::IoErr::None;
}

common::IoErr Http2Stream::on_data_payload_recv(mem::IoBuf payload, std::size_t offset, std::size_t length,
                                                bool end_stream) noexcept {
    if (remote_rst_ || local_rst_ || remote_end_stream_) {
        return common::IoErr::Invalid;
    }
    if (offset > length) {
        return common::IoErr::Invalid;
    }
    if (payload.readable() > length - offset) {
        return common::IoErr::Invalid;
    }

    common::IoErr err = ops_->on_body(owner_, std::move(payload), end_stream);
    if (err != common::IoErr::None) {
        return err;
    }

    if (end_stream) {
        remote_end_stream_ = true;
    }
    return common::IoErr::None;
}

void Http2Stream::on_rst_recv(Http2ErrorCode, common::IoErr result) noexcept {
    remote_rst_ = true;
    close(result);
}

common::IoErr Http2Stream::close_rst(Http2ErrorCode code, common::IoErr result) noexcept {
    if (!conn_) {
        return close_reason_ != common::IoErr::None ? close_reason_ : common::IoErr::Canceled;
    }

    common::IoErr err = conn_->send_rst_stream(stream_id_, code);
    if (err != common::IoErr::None) {
        return err;
    }

    local_rst_ = true;
    close(result);
    conn_->try_release_stream(*this);
    return common::IoErr::None;
}

void Http2Stream::update_send_window(std::int32_t delta) noexcept {
    send_window_ += delta;
    if (delta > 0 && conn_ && outbound_wait_state_ == OutboundWaitState::StreamWindow) {
        conn_->on_stream_send_window_update(*this);
    }
}

void Http2Stream::consume_recv_window(std::uint32_t bytes) noexcept {
    FIBER_ASSERT(recv_window_remaining_ >= 0);
    FIBER_ASSERT(bytes <= static_cast<std::uint32_t>(recv_window_remaining_));
    recv_window_remaining_ -= static_cast<std::int32_t>(bytes);
}

common::IoErr Http2Stream::maybe_replenish_recv_window(std::size_t buffered_bytes) noexcept {
    if (!conn_) {
        return common::IoErr::None;
    }

    if (buffered_bytes > recv_window_target_) {
        return common::IoErr::Invalid;
    }

    if (recv_window_remaining_ > static_cast<std::int32_t>(recv_window_low_watermark_)) {
        return common::IoErr::None;
    }

    if (buffered_bytes > recv_window_low_watermark_) {
        return common::IoErr::None;
    }

    FIBER_ASSERT(recv_window_remaining_ >= 0);
    std::uint32_t current_window = static_cast<std::uint32_t>(recv_window_remaining_);
    std::uint32_t target_window = recv_window_target_ - static_cast<std::uint32_t>(buffered_bytes);
    if (target_window <= current_window) {
        return common::IoErr::None;
    }

    std::uint32_t increment = target_window - current_window;
    common::IoErr err = conn_->send_window_update(stream_id_, increment);
    if (err != common::IoErr::None) {
        return err;
    }

    recv_window_remaining_ += static_cast<std::int32_t>(increment);
    return common::IoErr::None;
}

void Http2Stream::close(common::IoErr result) noexcept {
    const bool first_abort = close_reason_ == common::IoErr::None;
    if (first_abort) {
        close_reason_ = result;
    }
    active_ = false;
    if (first_abort && conn_) {
        conn_->cancel_stream_send(*this, close_reason_);
    }
    if (first_abort && ops_ && ops_->on_abort) {
        ops_->on_abort(owner_, close_reason_);
    }
    if (first_abort && !conn_ && outbound_operation_) {
        outbound_operation_->on_outbound_abort(close_reason_);
    }
}

bool Http2Stream::try_arm_outbound(Http2OutboundOperation &operation) noexcept {
    if (outbound_operation_) {
        return false;
    }
    outbound_operation_ = &operation;
    return true;
}

void Http2Stream::disarm_outbound(Http2OutboundOperation &operation) noexcept {
    if (outbound_operation_ != &operation) {
        return;
    }
    FIBER_ASSERT(outbound_hook_.state_ == Http2OutboundHook::State::Idle);
    FIBER_ASSERT(outbound_wait_state_ == OutboundWaitState::None);
    FIBER_ASSERT(outbound_kind_ == Http2OutboundKind::None);
    outbound_operation_ = nullptr;
}

std::size_t Http2Stream::pending_flow_controlled_bytes() const noexcept {
    FIBER_ASSERT(outbound_operation_ != nullptr);
    return outbound_operation_->pending_flow_controlled_bytes();
}

common::IoErr Http2Stream::encode_outbound_batch(const Http2OutboundEncodeRequest &req,
                                                 Http2OutboundEncodeTarget &target,
                                                 Http2OutboundEncodeResult &result) noexcept {
    FIBER_ASSERT(outbound_operation_ != nullptr);
    return outbound_operation_->encode_outbound_batch(*this, req, target, result);
}

void Http2Stream::on_outbound_hook_send_done(Http2OutboundHook &hook, common::IoErr result) noexcept {
    auto *stream = static_cast<Http2Stream *>(hook.ctx_);
    FIBER_ASSERT(stream != nullptr);
    FIBER_ASSERT(&stream->outbound_hook_ == &hook);
    FIBER_ASSERT(hook.state_ == Http2OutboundHook::State::Idle);

    const std::uint32_t flow_controlled_bytes = hook.window_consumed_;
    const bool operation_final_batch = hook.operation_final_batch_;
    hook.window_consumed_ = 0;
    hook.operation_final_batch_ = false;
    if (operation_final_batch) {
        stream->outbound_kind_ = Http2OutboundKind::None;
    }

    if (stream->close_reason_ != common::IoErr::None) {
        if (stream->outbound_operation_) {
            stream->outbound_operation_->on_outbound_abort(result != common::IoErr::None ? result
                                                                                         : stream->close_reason_);
        }
        if (stream->conn_) {
            stream->conn_->on_stream_outbound_idle(*stream);
        }
        return;
    }

    FIBER_ASSERT(stream->outbound_operation_ != nullptr);
    if (result == common::IoErr::None) {
        stream->outbound_operation_->on_outbound_batch_sent(flow_controlled_bytes, operation_final_batch);
    } else {
        stream->outbound_operation_->on_outbound_abort(result);
    }

    if (stream->conn_) {
        stream->conn_->on_stream_outbound_idle(*stream);
    }
}

bool Http2Stream::ready_for_connection_release() const noexcept {
    return close_reason_ != common::IoErr::None || remote_rst_ || local_rst_ ||
           (remote_end_stream_ && local_end_stream_);
}

bool Http2Stream::ready_for_destruction() const noexcept {
    return !attached_to_connection_ && ready_for_connection_release() && ref_count_ == 0;
}

void Http2Stream::attach_to_connection(Http2Connection &conn, std::uint32_t stream_id) noexcept {
    FIBER_ASSERT(!attached_to_connection_);
    FIBER_ASSERT(conn_ == nullptr);
    FIBER_ASSERT(stream_id_ == 0);
    FIBER_ASSERT(stream_id != 0);
    stream_id_ = stream_id;
    conn_ = &conn;
    attached_to_connection_ = true;
}

void Http2Stream::retain() noexcept { ++ref_count_; }

void Http2Stream::release() noexcept {
    FIBER_ASSERT(ref_count_ != 0);
    --ref_count_;
    if (ready_for_destruction()) {
        void *owner = owner_;
        const Ops *ops = ops_;
        owner_ = nullptr;
        ops_ = nullptr;
        FIBER_ASSERT(owner != nullptr);
        FIBER_ASSERT(ops != nullptr);
        ops->on_destroy(owner);
    }
}

} // namespace fiber::http
