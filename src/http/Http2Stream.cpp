#include "Http2Stream.h"

#include "../common/Assert.h"
#include "Http2Connection.h"

namespace fiber::http {

Http2Stream::Http2Stream(std::uint32_t stream_id, void *owner, const Ops &ops) noexcept :
    stream_id_(stream_id), owner_(owner), ops_(&ops) {
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
        common::IoErr err = conn_->inbound_hpack_decoder().decode(payload.readable_data(), payload.readable(), end_headers);
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

void Http2Stream::update_send_window(std::int32_t delta) noexcept { send_window_ += delta; }

void Http2Stream::update_recv_window(std::int32_t delta) noexcept { recv_window_remaining_ += delta; }

void Http2Stream::close(common::IoErr result) noexcept {
    if (close_reason_ == common::IoErr::None) {
        close_reason_ = result;
    }
    active_ = false;
}

bool Http2Stream::ready_for_connection_release() const noexcept {
    return close_reason_ != common::IoErr::None || remote_rst_ || local_rst_ ||
           (remote_end_stream_ && local_end_stream_);
}

bool Http2Stream::ready_for_destruction() const noexcept {
    return !attached_to_connection_ && ready_for_connection_release() && ref_count_ == 0;
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
