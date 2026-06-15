#include "QuicStream.h"

#include <algorithm>
#include <expected>
#include <limits>
#include <utility>

namespace fiber::quic {

namespace {

constexpr std::uint64_t kStreamTypeMask = 0x02;

} // namespace

void QuicStream::Lease::reset() noexcept {
    if (!stream_) {
        return;
    }
    QuicStream *stream = stream_;
    stream_ = nullptr;
    stream->release();
}

QuicStream::QuicStream(std::uint64_t stream_id, mem::IoBufNodePool &recv_extent_pool) noexcept :
    stream_id_(stream_id), reassembler_(recv_extent_pool) {}

QuicStream::QuicStream(std::uint64_t stream_id, mem::IoBufNodePool &recv_extent_pool, void *owner,
                       const Ops &ops) noexcept :
    stream_id_(stream_id), owner_(owner), ops_(&ops), reassembler_(recv_extent_pool), app_released_(false) {}

QuicStream::~QuicStream() = default;

std::uint64_t QuicStream::sequence() const noexcept { return stream_sequence(stream_id_); }

QuicStreamType QuicStream::type() const noexcept {
    return bidirectional() ? QuicStreamType::Bidirectional : QuicStreamType::Unidirectional;
}

bool QuicStream::bidirectional() const noexcept { return is_bidirectional_stream_id(stream_id_); }

bool QuicStream::unidirectional() const noexcept { return is_unidirectional_stream_id(stream_id_); }

common::IoResult<std::size_t> QuicStream::recv_stream_data(std::uint64_t offset, QuicSlice data, bool fin) noexcept {
    if (offset > std::numeric_limits<std::uint64_t>::max() - data.len) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const std::uint64_t end = offset + data.len;

    if (recv_state_ == QuicStreamRecvState::ResetRecvd || recv_state_ == QuicStreamRecvState::Closed) {
        if (!has_final_size_ || end > final_size_ || (fin && end != final_size_)) {
            return std::unexpected(common::IoErr::Invalid);
        }
        return 0;
    }
    if (fin && end < recv_highest_offset_) {
        return std::unexpected(common::IoErr::Invalid);
    }

    auto inserted = reassembler_.insert(offset, data, fin);
    if (!inserted) {
        return std::unexpected(inserted.error());
    }
    recv_highest_offset_ = std::max(recv_highest_offset_, end);
    if (reassembler_.has_final_size()) {
        auto fixed = set_final_size(reassembler_.final_size());
        if (!fixed) {
            return std::unexpected(fixed.error());
        }
        if (recv_state_ == QuicStreamRecvState::Open) {
            recv_state_ = QuicStreamRecvState::SizeKnown;
        }
    }
    if (reassembler_.finished()) {
        recv_state_ = QuicStreamRecvState::Closed;
    }
    if (ops_ != nullptr && ops_->on_data != nullptr && (*inserted != 0 || recv_state_ == QuicStreamRecvState::Closed)) {
        mem::IoBufChain out;
        auto taken = take_recv_data(reassembler_.buffered_bytes(), out);
        if (!taken) {
            return std::unexpected(taken.error());
        }
        const bool fin_delivered = recv_state_ == QuicStreamRecvState::Closed;
        if (*taken == 0 && !fin_delivered) {
            return *inserted;
        }
        const common::IoErr err = ops_->on_data(owner_, *this, std::move(out), fin_delivered);
        if (err != common::IoErr::None) {
            return std::unexpected(err);
        }
    }
    return *inserted;
}

common::IoResult<void> QuicStream::recv_reset(std::uint64_t error_code, std::uint64_t final_size) noexcept {
    auto fixed = set_final_size(final_size);
    if (!fixed) {
        return std::unexpected(fixed.error());
    }
    if (recv_highest_offset_ > final_size) {
        return std::unexpected(common::IoErr::Invalid);
    }
    reset_error_code_ = error_code;
    recv_state_ = QuicStreamRecvState::ResetRecvd;
    reassembler_.clear();
    if (ops_ != nullptr && ops_->on_reset != nullptr) {
        ops_->on_reset(owner_, *this, error_code, final_size);
    }
    return {};
}

common::IoResult<std::size_t> QuicStream::take_recv_data(std::size_t max_bytes, mem::IoBufChain &out) noexcept {
    auto taken = reassembler_.take(max_bytes, out);
    if (!taken) {
        return std::unexpected(taken.error());
    }
    if (reassembler_.finished()) {
        recv_state_ = QuicStreamRecvState::Closed;
    }
    return *taken;
}

void QuicStream::mark_app_released() noexcept { app_released_ = true; }

void QuicStream::abort(common::IoErr reason) noexcept {
    if (ops_ != nullptr && ops_->on_abort != nullptr) {
        ops_->on_abort(owner_, reason);
    }
}

bool QuicStream::ready_for_connection_release() const noexcept {
    return app_released_ &&
           (recv_state_ == QuicStreamRecvState::Closed || recv_state_ == QuicStreamRecvState::ResetRecvd);
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
}

void QuicStream::detach_from_connection() noexcept {
    conn_ = nullptr;
    attached_to_connection_ = false;
}

void QuicStream::retain() noexcept { ++ref_count_; }

void QuicStream::release() noexcept {
    --ref_count_;
    if (ready_for_destruction()) {
        void *owner = owner_;
        const Ops *ops = ops_;
        owner_ = nullptr;
        ops_ = nullptr;
        if (ops != nullptr && ops->on_destroy != nullptr) {
            ops->on_destroy(owner);
            return;
        }
        delete this;
    }
}

common::IoResult<void> QuicStream::set_final_size(std::uint64_t final_size) noexcept {
    if (has_final_size_ && final_size_ != final_size) {
        return std::unexpected(common::IoErr::Invalid);
    }
    final_size_ = final_size;
    has_final_size_ = true;
    return {};
}

} // namespace fiber::quic
