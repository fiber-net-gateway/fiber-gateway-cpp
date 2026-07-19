#ifndef FIBER_QUIC_QUIC_DATA_REASSEMBLER_H
#define FIBER_QUIC_QUIC_DATA_REASSEMBLER_H

#include <cstddef>
#include <cstdint>
#include <limits>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/IoBufChain.h"

namespace fiber::quic {

inline constexpr std::size_t kQuicMaxCryptoBuffered = 64 * 1024;
inline constexpr std::size_t kQuicCryptoRecvMaxActiveExtents = 1024;

class QuicDataReassembler : public common::NonCopyable, public common::NonMovable {
public:
    enum class BufferAccounting : std::uint8_t {
        AllRetained,
        OutOfOrderOnly,
    };

    struct Options {
        std::size_t buffer_limit = kQuicMaxCryptoBuffered;
        std::size_t max_active_extents = kQuicCryptoRecvMaxActiveExtents;
        BufferAccounting buffer_accounting = BufferAccounting::OutOfOrderOnly;
    };

    QuicDataReassembler() noexcept = default;
    explicit QuicDataReassembler(mem::IoBufNodePool &pool) noexcept;
    QuicDataReassembler(mem::IoBufNodePool &pool, Options options) noexcept;
    ~QuicDataReassembler();

    void init(mem::IoBufNodePool &pool) noexcept;
    void init(mem::IoBufNodePool &pool, Options options) noexcept;
    void discard_buffered() noexcept;
    void clear() noexcept;

    [[nodiscard]] common::IoResult<std::size_t> insert(std::uint64_t offset, mem::IoBuf data) noexcept;
    [[nodiscard]] common::IoResult<std::size_t>
    take_contiguous(mem::IoBufChain &out, std::size_t max_bytes = std::numeric_limits<std::size_t>::max()) noexcept;

    [[nodiscard]] std::uint64_t next_offset() const noexcept { return next_offset_; }
    [[nodiscard]] std::size_t buffered_bytes() const noexcept { return buffered_bytes_; }
    [[nodiscard]] std::size_t buffer_limit() const noexcept { return buffer_limit_; }
    [[nodiscard]] std::size_t active_extent_count() const noexcept { return active_extent_count_; }
    [[nodiscard]] bool has_contiguous_data() const noexcept {
        return head_ != nullptr && head_->offset == next_offset_ && head_->buf.readable() != 0;
    }
    [[nodiscard]] mem::IoBufNodePool &node_pool() noexcept { return *pool_; }
    [[nodiscard]] bool initialized() const noexcept { return pool_ != nullptr; }

private:
    struct InsertCost {
        std::size_t bytes = 0;
        std::size_t extents = 0;
    };

    [[nodiscard]] common::IoResult<InsertCost> insert_cost(std::uint64_t offset, std::size_t len) const noexcept;
    [[nodiscard]] std::uint64_t contiguous_end_after_insert(std::uint64_t offset, std::size_t len) const noexcept;
    [[nodiscard]] mem::IoBufNode *reserve_nodes(std::size_t count) noexcept;
    void release_nodes(mem::IoBufNode *nodes) noexcept;
    void insert_after(mem::IoBufNode *prev, mem::IoBufNode &extent) noexcept;
    [[nodiscard]] mem::IoBufNode *try_merge_with_next(mem::IoBufNode *extent) noexcept;
    void unlink_after(mem::IoBufNode *prev, mem::IoBufNode &extent) noexcept;

    mem::IoBufNodePool *pool_ = nullptr;
    mem::IoBufNode *head_ = nullptr;
    mem::IoBufNode *tail_ = nullptr;
    mem::IoBufNode *last_insert_ = nullptr;
    std::size_t buffer_limit_ = kQuicMaxCryptoBuffered;
    std::size_t max_active_extents_ = kQuicCryptoRecvMaxActiveExtents;
    std::size_t buffered_bytes_ = 0;
    std::size_t active_extent_count_ = 0;
    std::uint64_t next_offset_ = 0;
    BufferAccounting buffer_accounting_ = BufferAccounting::OutOfOrderOnly;
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_DATA_REASSEMBLER_H
