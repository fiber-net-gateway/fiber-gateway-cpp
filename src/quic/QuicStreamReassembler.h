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

class QuicStreamReassembler : public common::NonCopyable, public common::NonMovable {
public:
    explicit QuicStreamReassembler(mem::IoBufNodePool &pool) noexcept;
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
    [[nodiscard]] static std::uint64_t block_of(std::uint64_t offset) noexcept;
    [[nodiscard]] static std::size_t block_offset(std::uint64_t offset) noexcept;
    [[nodiscard]] static std::uint64_t block_end(std::uint64_t offset) noexcept;

    [[nodiscard]] common::IoResult<mem::IoBufNode *> create_extent(std::uint64_t offset, std::size_t len,
                                                                   mem::IoBufNode *prev, mem::IoBufNode *next,
                                                                   const std::uint8_t *src,
                                                                   std::uint64_t block) noexcept;
    void insert_after(mem::IoBufNode *prev, mem::IoBufNode &extent) noexcept;
    [[nodiscard]] mem::IoBufNode *try_merge_with_next(mem::IoBufNode *extent) noexcept;
    void unlink_after(mem::IoBufNode *prev, mem::IoBufNode &extent) noexcept;
    [[nodiscard]] static bool has_same_block_neighbor(const mem::IoBufNode *prev, const mem::IoBufNode *next,
                                                      std::uint64_t block) noexcept;

    mem::IoBufNodePool *pool_ = nullptr;
    mem::IoBufNode *head_ = nullptr;
    mem::IoBufNode *tail_ = nullptr;
    mem::IoBufNode *last_insert_ = nullptr;
    std::uint64_t next_read_offset_ = 0;
    std::uint64_t final_size_ = 0;
    std::size_t buffered_bytes_ = 0;
    std::size_t active_extent_count_ = 0;
    std::size_t active_block_count_ = 0;
    bool has_final_size_ = false;
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_STREAM_REASSEMBLER_H
