#include "QuicStream.h"

namespace fiber::quic {

namespace {

constexpr std::uint64_t kStreamTypeMask = 0x02;

} // namespace

QuicStream::QuicStream(std::uint64_t stream_id) noexcept : stream_id_(stream_id) {}

std::uint64_t QuicStream::sequence() const noexcept { return stream_sequence(stream_id_); }

QuicStreamType QuicStream::type() const noexcept {
    return bidirectional() ? QuicStreamType::Bidirectional : QuicStreamType::Unidirectional;
}

bool QuicStream::bidirectional() const noexcept { return is_bidirectional_stream_id(stream_id_); }

bool QuicStream::unidirectional() const noexcept { return is_unidirectional_stream_id(stream_id_); }

std::uint64_t QuicStream::stream_sequence(std::uint64_t stream_id) noexcept { return stream_id >> 2U; }

bool QuicStream::is_bidirectional_stream_id(std::uint64_t stream_id) noexcept {
    return (stream_id & kStreamTypeMask) == 0;
}

bool QuicStream::is_unidirectional_stream_id(std::uint64_t stream_id) noexcept {
    return !is_bidirectional_stream_id(stream_id);
}

} // namespace fiber::quic
