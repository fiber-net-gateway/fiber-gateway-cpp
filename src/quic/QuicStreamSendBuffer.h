#ifndef FIBER_QUIC_QUIC_STREAM_SEND_BUFFER_H
#define FIBER_QUIC_QUIC_STREAM_SEND_BUFFER_H

#include <cstddef>
#include <cstdint>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/IoBufChain.h"

namespace fiber::quic {

enum class QuicSendExtentState : std::uint8_t {
    Ready = 0,
    Inflight = 1,
};

class QuicStreamSendBuffer : public common::NonCopyable, public common::NonMovable {
public:
    struct EncodedFrameResult {
        std::size_t offset = 0;
        std::size_t data_len = 0;
        std::size_t encoded_len = 0;
        bool fin = false;
        bool encoded = false;
    };

    explicit QuicStreamSendBuffer(mem::IoBufNodePool &pool) noexcept;
    ~QuicStreamSendBuffer();

    [[nodiscard]] common::IoResult<std::size_t> append(const mem::IoBuf &buf, bool fin = false) noexcept;
    [[nodiscard]] common::IoResult<EncodedFrameResult> encode_stream_frame(std::uint64_t stream_id, std::uint8_t *dst,
                                                                           std::size_t capacity) noexcept;
    [[nodiscard]] common::IoResult<void> mark_acked(std::size_t offset, std::size_t length, bool encoded_fin) noexcept;
    [[nodiscard]] common::IoResult<void> mark_failed(std::size_t offset, std::size_t length, bool encoded_fin) noexcept;

    [[nodiscard]] std::size_t ready_bytes() const noexcept { return ready_bytes_; }
    [[nodiscard]] std::size_t inflight_bytes() const noexcept { return inflight_bytes_; }
    [[nodiscard]] std::size_t buffered_bytes() const noexcept { return ready_bytes_ + inflight_bytes_; }
    [[nodiscard]] std::size_t active_extent_count() const noexcept { return active_extent_count_; }
    [[nodiscard]] bool empty() const noexcept { return buffered_bytes() == 0 && (!fin_appended_ || fin_acked_); }
    [[nodiscard]] bool has_final_size() const noexcept { return fin_appended_; }
    [[nodiscard]] std::uint64_t final_size() const noexcept { return total_appended_bytes_; }

private:
    [[nodiscard]] bool has_pending_fin() const noexcept { return fin_appended_ && !fin_inflight_ && !fin_acked_; }
    [[nodiscard]] bool is_last_ready_extent(const mem::IoBufNode *extent) const noexcept;
    void try_merge_with_next(mem::IoBufNode *extent) noexcept;

    mem::IoBufNodePool *pool_ = nullptr;
    mem::IoBufNode *head_ = nullptr;
    mem::IoBufNode *tail_ = nullptr;
    mem::IoBufNode *ready_head_ = nullptr;
    std::size_t ready_bytes_ = 0;
    std::size_t inflight_bytes_ = 0;
    std::size_t active_extent_count_ = 0;
    std::uint64_t total_appended_bytes_ = 0;
    bool fin_appended_ = false;
    bool fin_inflight_ = false;
    bool fin_acked_ = false;
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_STREAM_SEND_BUFFER_H
