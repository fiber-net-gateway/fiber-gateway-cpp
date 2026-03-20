#include "Http2Stream.h"

#include "../common/Assert.h"
#include "Http2Connection.h"

namespace fiber::http {

Http2Stream::Lease Http2Stream::alloc(std::uint32_t stream_id) noexcept {
    Http2Stream *stream = new (std::nothrow) Http2Stream(stream_id);
    return Lease::adopt(stream);
}

common::IoErr Http2Stream::on_headers_payload_recv(const mem::IoBuf &payload, std::size_t offset, std::size_t length,
                                                   bool end_headers, bool end_stream, bool trailer_block) noexcept {
    if (remote_rst_ || local_rst_ || remote_end_stream_ || (remote_trailer_ && !trailer_block)) {
        return common::IoErr::Invalid;
    }
    if (offset > length) {
        return common::IoErr::Invalid;
    }
    if (payload.readable() > length - offset) {
        return common::IoErr::Invalid;
    }

    (void)payload;
    // TODO: decode/process received header block fragments.

    if (trailer_block) {
        remote_trailer_ = true;
    }
    if (end_headers && !remote_end_headers_) {
        remote_end_headers_ = true;
    }
    if (end_stream) {
        remote_end_stream_ = true;
    }
    return common::IoErr::None;
}

common::IoErr Http2Stream::on_data_payload_recv(const mem::IoBuf &payload, std::size_t offset, std::size_t length,
                                                bool end_stream) noexcept {
    if (remote_rst_ || local_rst_ || remote_end_stream_ || remote_trailer_ || !remote_end_headers_) {
        return common::IoErr::Invalid;
    }
    if (offset > length) {
        return common::IoErr::Invalid;
    }
    if (payload.readable() > length - offset) {
        return common::IoErr::Invalid;
    }

    (void)payload;
    // TODO: consume/process received DATA payload.

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
        delete this;
    }
}

} // namespace fiber::http
