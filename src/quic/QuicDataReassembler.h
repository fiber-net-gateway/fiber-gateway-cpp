#ifndef FIBER_QUIC_QUIC_DATA_REASSEMBLER_H
#define FIBER_QUIC_QUIC_DATA_REASSEMBLER_H

#include <cstddef>
#include <cstdint>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/IoBufChain.h"
#include "QuicFrame.h"

namespace fiber::quic {

inline constexpr std::size_t kQuicMaxCryptoBuffered = 64 * 1024;

class QuicDataReassembler : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        std::size_t buffer_limit = kQuicMaxCryptoBuffered;
    };

    QuicDataReassembler() noexcept = default;
    ~QuicDataReassembler();

    void init(mem::IoBufNodePool &pool) noexcept;
    void init(mem::IoBufNodePool &pool, Options options) noexcept;
    void clear() noexcept;

    [[nodiscard]] common::IoResult<std::size_t> insert(std::uint64_t offset, QuicSlice data) noexcept;
    [[nodiscard]] common::IoResult<std::size_t> take_contiguous(mem::IoBufChain &out) noexcept;

    [[nodiscard]] std::uint64_t next_offset() const noexcept { return next_offset_; }
    [[nodiscard]] std::size_t buffered_bytes() const noexcept { return buffered_bytes_; }
    [[nodiscard]] std::size_t buffer_limit() const noexcept { return buffer_limit_; }
    [[nodiscard]] std::size_t active_extent_count() const noexcept { return active_extent_count_; }
    [[nodiscard]] std::size_t active_block_count() const noexcept { return active_block_count_; }

private:
    struct InsertCost {
        std::size_t bytes = 0;
        std::size_t blocks = 0;
        std::size_t extents = 0;
    };

    static constexpr std::size_t kRecvBlockSize = 4 * 1024;

    [[nodiscard]] common::IoResult<InsertCost> insert_cost(std::uint64_t offset, std::size_t len) const noexcept;
    [[nodiscard]] std::uint64_t contiguous_end_after_insert(std::uint64_t offset, std::size_t len) const noexcept;
    [[nodiscard]] common::IoResult<mem::IoBufNode *> create_extent(std::uint64_t offset, std::size_t len,
                                                                   mem::IoBufNode *prev, mem::IoBufNode *next,
                                                                   const std::uint8_t *src,
                                                                   std::uint64_t block) noexcept;
    void insert_after(mem::IoBufNode *prev, mem::IoBufNode &extent) noexcept;
    [[nodiscard]] mem::IoBufNode *try_merge_with_next(mem::IoBufNode *extent) noexcept;
    void unlink_after(mem::IoBufNode *prev, mem::IoBufNode &extent) noexcept;
    [[nodiscard]] static bool has_same_block_neighbor(const mem::IoBufNode *prev, const mem::IoBufNode *next,
                                                      std::uint64_t block) noexcept;

    [[nodiscard]] static std::uint64_t block_of(std::uint64_t offset) noexcept;
    [[nodiscard]] static std::size_t block_offset(std::uint64_t offset) noexcept;
    [[nodiscard]] static std::uint64_t block_end(std::uint64_t offset) noexcept;

    mem::IoBufNodePool *pool_ = nullptr;
    mem::IoBufNode *head_ = nullptr;
    mem::IoBufNode *tail_ = nullptr;
    mem::IoBufNode *last_insert_ = nullptr;
    std::size_t buffer_limit_ = kQuicMaxCryptoBuffered;
    std::size_t buffered_bytes_ = 0;
    std::size_t active_extent_count_ = 0;
    std::size_t active_block_count_ = 0;
    std::uint64_t next_offset_ = 0;
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_DATA_REASSEMBLER_H
