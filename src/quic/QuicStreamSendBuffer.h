#ifndef FIBER_QUIC_QUIC_STREAM_SEND_BUFFER_H
#define FIBER_QUIC_QUIC_STREAM_SEND_BUFFER_H

#include <cstddef>
#include <cstdint>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/IoBuf.h"
#include "QuicStreamDataExtent.h"

namespace fiber::quic {

enum class QuicStreamSendExtentState : std::uint8_t {
    Ready = 0,
    Inflight = 1,
};

enum class QuicStreamSendFinState : std::uint8_t {
    None = 0,
    Ready = 1,
    Inflight = 2,
};

struct QuicStreamSendEncodeResult {
    std::uint64_t offset = 0;
    std::size_t data_len = 0;
    std::size_t encoded_len = 0;
    bool fin = false;
    bool encoded = false;
};

class QuicStreamSendBuffer : public common::NonCopyable, public common::NonMovable {
public:
    explicit QuicStreamSendBuffer(QuicStreamDataExtentPool &pool) noexcept;
    ~QuicStreamSendBuffer();

    [[nodiscard]] common::IoResult<std::size_t> append(mem::IoBuf data, bool fin = false) noexcept;
    [[nodiscard]] common::IoResult<QuicStreamSendEncodeResult>
    encode_stream_frame(std::uint64_t stream_id, std::uint8_t *dst, std::size_t capacity) noexcept;
    [[nodiscard]] common::IoResult<void> mark_acked(std::uint64_t offset, std::size_t len, bool fin = false) noexcept;
    [[nodiscard]] common::IoResult<void> mark_failed(std::uint64_t offset, std::size_t len, bool fin = false) noexcept;
    void clear() noexcept;

    [[nodiscard]] std::uint64_t next_append_offset() const noexcept { return next_append_offset_; }
    [[nodiscard]] bool has_final_size() const noexcept { return has_final_size_; }
    [[nodiscard]] std::uint64_t final_size() const noexcept { return final_size_; }
    [[nodiscard]] bool empty() const noexcept {
        return buffered_bytes_ == 0 && fin_state_ != QuicStreamSendFinState::Ready &&
               fin_state_ != QuicStreamSendFinState::Inflight;
    }
    [[nodiscard]] std::size_t buffered_bytes() const noexcept { return buffered_bytes_; }
    [[nodiscard]] std::size_t ready_bytes() const noexcept { return ready_bytes_; }
    [[nodiscard]] std::size_t inflight_bytes() const noexcept { return inflight_bytes_; }
    [[nodiscard]] std::size_t active_extent_count() const noexcept { return active_extent_count_; }
    [[nodiscard]] std::size_t active_block_count() const noexcept { return active_block_count_; }

private:
    [[nodiscard]] static std::uint64_t block_index(std::uint64_t offset) noexcept;
    [[nodiscard]] static std::uint64_t block_end(std::uint64_t offset) noexcept;
    [[nodiscard]] static QuicStreamSendExtentState extent_state(const QuicStreamDataExtent &extent) noexcept;
    static void set_extent_state(QuicStreamDataExtent &extent, QuicStreamSendExtentState state) noexcept;

    [[nodiscard]] QuicStreamDataExtent *find_prev(std::uint64_t offset) noexcept;
    [[nodiscard]] QuicStreamDataExtent *first_ready() noexcept;
    [[nodiscard]] common::IoResult<void> split_at(std::uint64_t offset) noexcept;
    [[nodiscard]] common::IoResult<QuicStreamDataExtent *> create_extent(std::uint64_t start, std::uint64_t end,
                                                                         mem::IoBuf &&view) noexcept;
    void push_back(QuicStreamDataExtent &extent) noexcept;
    void insert_after(QuicStreamDataExtent *prev, QuicStreamDataExtent &extent) noexcept;
    [[nodiscard]] QuicStreamDataExtent *try_merge_with_next(QuicStreamDataExtent *extent) noexcept;
    void unlink_after(QuicStreamDataExtent *prev, QuicStreamDataExtent &extent) noexcept;
    [[nodiscard]] bool has_same_block_neighbor(const QuicStreamDataExtent *prev, const QuicStreamDataExtent *next,
                                               std::uint64_t block) const noexcept;
    void merge_around(std::uint64_t offset) noexcept;
    [[nodiscard]] common::IoResult<void> set_range_state(std::uint64_t offset, std::size_t len,
                                                         QuicStreamSendExtentState from,
                                                         QuicStreamSendExtentState to) noexcept;
    [[nodiscard]] common::IoResult<void> remove_range(std::uint64_t offset, std::size_t len,
                                                      QuicStreamSendExtentState state) noexcept;

    QuicStreamDataExtentPool *pool_ = nullptr;
    QuicStreamDataExtent *head_ = nullptr;
    QuicStreamDataExtent *tail_ = nullptr;
    std::uint64_t next_append_offset_ = 0;
    std::uint64_t final_size_ = 0;
    std::size_t buffered_bytes_ = 0;
    std::size_t ready_bytes_ = 0;
    std::size_t inflight_bytes_ = 0;
    std::size_t active_extent_count_ = 0;
    std::size_t active_block_count_ = 0;
    bool has_final_size_ = false;
    QuicStreamSendFinState fin_state_ = QuicStreamSendFinState::None;
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_STREAM_SEND_BUFFER_H
