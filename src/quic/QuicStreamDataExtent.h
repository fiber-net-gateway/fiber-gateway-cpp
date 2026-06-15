#ifndef FIBER_QUIC_QUIC_STREAM_DATA_EXTENT_H
#define FIBER_QUIC_QUIC_STREAM_DATA_EXTENT_H

#include <cstddef>
#include <cstdint>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/IoBuf.h"

namespace fiber::quic {

inline constexpr std::size_t kQuicStreamDataBlockSize = 64 * 1024;
inline constexpr std::size_t kQuicStreamDataExtentPoolMaxCached = 1000;
inline constexpr std::size_t kQuicStreamDataMaxActiveExtents = 4096;
inline constexpr std::size_t kQuicStreamDataMaxActiveBlocks = 1024;

struct QuicStreamDataExtent {
    std::uint64_t offset = 0; // stream data offset.
    std::uint8_t state = 0; // state represent ready|inflight (in sending buffer) or hole (in reassemble)
    mem::IoBuf view{}; // readable data represent stream data;
    QuicStreamDataExtent *next = nullptr;
};

class QuicStreamDataExtentPool : public common::NonCopyable, public common::NonMovable {
public:
    QuicStreamDataExtentPool() noexcept = default;
    ~QuicStreamDataExtentPool();

    [[nodiscard]] QuicStreamDataExtent *alloc() noexcept;
    void release(QuicStreamDataExtent *extent) noexcept;
    void clear() noexcept;

    [[nodiscard]] std::size_t cached_count() const noexcept { return cached_count_; }

private:
    QuicStreamDataExtent *free_head_ = nullptr;
    std::size_t cached_count_ = 0;
};

using QuicRecvExtent = QuicStreamDataExtent;
using QuicRecvExtentPool = QuicStreamDataExtentPool;

inline constexpr std::size_t kQuicStreamRecvBlockSize = kQuicStreamDataBlockSize;
inline constexpr std::size_t kQuicRecvExtentPoolMaxCached = kQuicStreamDataExtentPoolMaxCached;
inline constexpr std::size_t kQuicStreamRecvMaxActiveExtents = kQuicStreamDataMaxActiveExtents;
inline constexpr std::size_t kQuicStreamRecvMaxActiveBlocks = kQuicStreamDataMaxActiveBlocks;

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_STREAM_DATA_EXTENT_H
