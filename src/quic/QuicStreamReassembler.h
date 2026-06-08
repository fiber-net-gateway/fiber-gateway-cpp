#ifndef FIBER_QUIC_QUIC_STREAM_REASSEMBLER_H
#define FIBER_QUIC_QUIC_STREAM_REASSEMBLER_H

#include <cstddef>
#include <cstdint>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/IoBuf.h"
#include "QuicFrame.h"

namespace fiber::quic {

inline constexpr std::size_t kQuicStreamRecvBlockSize = 64 * 1024;
inline constexpr std::size_t kQuicRecvExtentPoolMaxCached = 1000;
inline constexpr std::size_t kQuicStreamRecvMaxActiveExtents = 4096;
inline constexpr std::size_t kQuicStreamRecvMaxActiveBlocks = 1024;

struct QuicRecvExtent {
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    std::uint64_t block_index = 0;
    mem::IoBuf view{};
    QuicRecvExtent *next = nullptr;
};

class QuicRecvExtentPool : public common::NonCopyable, public common::NonMovable {
public:
    QuicRecvExtentPool() noexcept = default;
    ~QuicRecvExtentPool();

    [[nodiscard]] QuicRecvExtent *alloc() noexcept;
    void release(QuicRecvExtent *extent) noexcept;
    void clear() noexcept;

    [[nodiscard]] std::size_t cached_count() const noexcept { return cached_count_; }

private:
    QuicRecvExtent *free_head_ = nullptr;
    std::size_t cached_count_ = 0;
};

class QuicStreamReassembler : public common::NonCopyable, public common::NonMovable {
public:
    explicit QuicStreamReassembler(QuicRecvExtentPool &pool) noexcept;
    ~QuicStreamReassembler();

    [[nodiscard]] common::IoResult<std::size_t> insert(std::uint64_t offset, QuicSlice data, bool fin = false) noexcept;
    [[nodiscard]] common::IoResult<std::size_t> take(std::size_t max_bytes, mem::IoBufChain &out) noexcept;
    void clear() noexcept;

    [[nodiscard]] std::uint64_t next_read_offset() const noexcept { return next_read_offset_; }
    [[nodiscard]] bool has_final_size() const noexcept { return has_final_size_; }
    [[nodiscard]] std::uint64_t final_size() const noexcept { return final_size_; }
    [[nodiscard]] bool finished() const noexcept { return has_final_size_ && next_read_offset_ == final_size_; }
    [[nodiscard]] std::size_t buffered_bytes() const noexcept { return buffered_bytes_; }
    [[nodiscard]] std::size_t active_extent_count() const noexcept { return active_extent_count_; }
    [[nodiscard]] std::size_t active_block_count() const noexcept { return active_block_count_; }

private:
    [[nodiscard]] static std::uint64_t block_index(std::uint64_t offset) noexcept;
    [[nodiscard]] static std::size_t block_offset(std::uint64_t offset) noexcept;
    [[nodiscard]] static std::uint64_t block_end(std::uint64_t offset) noexcept;

    [[nodiscard]] QuicRecvExtent *find_prev(std::uint64_t offset) noexcept;
    [[nodiscard]] common::IoResult<QuicRecvExtent *> create_extent(std::uint64_t start, std::uint64_t end,
                                                                   QuicRecvExtent *prev, QuicRecvExtent *next,
                                                                   const std::uint8_t *src) noexcept;
    void insert_after(QuicRecvExtent *prev, QuicRecvExtent &extent) noexcept;
    [[nodiscard]] QuicRecvExtent *try_merge_with_next(QuicRecvExtent *extent) noexcept;
    void unlink_after(QuicRecvExtent *prev, QuicRecvExtent &extent) noexcept;
    [[nodiscard]] bool has_same_block_neighbor(const QuicRecvExtent *prev, const QuicRecvExtent *next,
                                               std::uint64_t block) const noexcept;

    QuicRecvExtentPool *pool_ = nullptr;
    QuicRecvExtent *head_ = nullptr;
    QuicRecvExtent *tail_ = nullptr;
    QuicRecvExtent *last_insert_ = nullptr;
    std::uint64_t next_read_offset_ = 0;
    std::uint64_t final_size_ = 0;
    std::size_t buffered_bytes_ = 0;
    std::size_t active_extent_count_ = 0;
    std::size_t active_block_count_ = 0;
    bool has_final_size_ = false;
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_STREAM_REASSEMBLER_H
