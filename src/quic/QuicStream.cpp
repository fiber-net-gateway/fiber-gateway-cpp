#include "QuicStream.h"

#include <algorithm>
#include <expected>
#include <limits>
#include <utility>

#include "QuicConnection.h"

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
    stream_id_(stream_id), recv_queue_(recv_extent_pool), send_queue_(recv_extent_pool) {}

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
    if (*taken != 0 && conn_ != nullptr) {
        conn_->on_stream_data_consumed(*taken);
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
    if (*taken != 0 && conn_ != nullptr) {
        conn_->on_stream_data_consumed(*taken);
    }
    sync_recv_state_from_queue();
    maybe_extend_recv_flow_control();
    co_return *taken;
}

common::IoResult<std::size_t> QuicStream::try_write(const mem::IoBuf &buf, bool fin) noexcept {
    auto appended = send_queue_.try_append(buf, fin);
    if (appended && conn_ != nullptr && has_send_work()) {
        (void) conn_->queue_stream_frame(*this);
    }
    return appended;
}

common::IoResult<std::size_t> QuicStream::try_write(mem::IoBufChain &chain) noexcept {
    auto appended = send_queue_.try_append_chain(chain);
    if (appended && conn_ != nullptr && has_send_work()) {
        (void) conn_->queue_stream_frame(*this);
    }
    return appended;
}

async::Task<common::IoResult<std::size_t>> QuicStream::write(mem::IoBuf buf, bool fin,
                                                             std::chrono::milliseconds timeout) noexcept {
    auto written = co_await send_queue_.append(std::move(buf), fin, timeout);
    if (!written) {
        co_return std::unexpected(written.error());
    }
    if (conn_ != nullptr && has_send_work()) {
        (void) conn_->queue_stream_frame(*this);
    }
    co_return *written;
}

async::Task<common::IoResult<std::size_t>> QuicStream::write(mem::IoBufChain &chain,
                                                             std::chrono::milliseconds timeout) noexcept {
    auto written = co_await send_queue_.append_chain(chain, timeout);
    if (!written) {
        co_return std::unexpected(written.error());
    }
    if (conn_ != nullptr && has_send_work()) {
        (void) conn_->queue_stream_frame(*this);
    }
    co_return *written;
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
    auto final_size = send_queue_.mark_reset();
    if (!final_size) {
        return std::unexpected(final_size.error());
    }
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
    return final_size - old_end;
}

common::IoResult<void> QuicStream::on_remote_stop_sending(std::uint64_t error_code) noexcept {
    return reset(error_code);
}

void QuicStream::on_max_stream_data(std::uint64_t limit) noexcept {
    send_queue_.update_max_stream_data(limit);
    if (conn_ != nullptr && has_send_work()) {
        (void) conn_->queue_stream_frame(*this);
    }
}

bool QuicStream::has_send_work() const noexcept { return send_queue_.has_send_work(); }

common::IoResult<QuicStreamSendBuffer::PreparedFrameResult>
QuicStream::prepare_stream_frame(std::size_t capacity) noexcept {
    return send_queue_.prepare_stream_frame(stream_id_, capacity);
}

common::IoResult<QuicStreamSendBuffer::EncodedFrameResult>
QuicStream::encode_stream_frame(std::uint8_t *dst, std::size_t capacity) noexcept {
    return send_queue_.encode_stream_frame(stream_id_, dst, capacity);
}

common::IoResult<void> QuicStream::mark_send_acked(std::size_t offset, std::size_t length, bool fin) noexcept {
    return send_queue_.mark_acked(offset, length, fin);
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
    return app_released_ && (recv_queue_.finished() || recv_queue_.reset_received()) && send_queue_.buffer().empty() &&
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

} // namespace fiber::quic
