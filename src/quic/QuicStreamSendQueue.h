#ifndef FIBER_QUIC_QUIC_STREAM_SEND_QUEUE_H
#define FIBER_QUIC_QUIC_STREAM_SEND_QUEUE_H

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/IoBufChain.h"

namespace fiber::quic {

class QuicStream;
struct QuicStreamSendQueueFrameTestAccess;
struct QuicStreamSendQueueTestAccess;

inline constexpr std::size_t kQuicStreamSendDefaultBufferLimit = 64 * 1024;

enum class QuicSendExtentState : std::uint8_t {
    Ready = 0,
    Inflight = 1,
};

class QuicStreamSendQueue : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        std::size_t buffer_limit = kQuicStreamSendDefaultBufferLimit;
        std::uint64_t max_stream_data = 0;
    };

    struct EncodedFrameResult {
        std::size_t offset = 0;
        std::size_t data_len = 0;
        std::size_t encoded_len = 0;
        bool fin = false;
        bool encoded = false;
    };

    struct PreparedFrameResult {
        const std::uint8_t *data = nullptr;
        std::size_t offset = 0;
        std::size_t data_len = 0;
        std::size_t encoded_len = 0;
        bool has_length = false;
        bool fin = false;
        bool encoded = false;
    };

    explicit QuicStreamSendQueue(mem::IoBufNodePool &pool) noexcept;
    QuicStreamSendQueue(mem::IoBufNodePool &pool, Options options) noexcept;
    ~QuicStreamSendQueue();

    [[nodiscard]] common::IoResult<std::size_t> try_append(const mem::IoBuf &buf, bool fin = false) noexcept;
    [[nodiscard]] common::IoResult<std::size_t> try_append_chain(mem::IoBufChain &chain) noexcept;
    [[nodiscard]] async::Task<common::IoResult<std::size_t>>
    append(mem::IoBuf buf, bool fin = false,
           std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] async::Task<common::IoResult<std::size_t>>
    append_chain(mem::IoBufChain &chain, std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;

    [[nodiscard]] common::IoResult<std::uint64_t> reset(std::uint64_t error_code = 0) noexcept;

    [[nodiscard]] std::size_t buffer_limit() const noexcept { return buffer_limit_; }
    [[nodiscard]] std::uint64_t max_stream_data() const noexcept { return max_stream_data_; }
    [[nodiscard]] std::size_t buffer_available() const noexcept;
    [[nodiscard]] std::uint64_t stream_data_available() const noexcept;
    [[nodiscard]] std::size_t ready_bytes() const noexcept { return ready_bytes_; }
    [[nodiscard]] std::size_t inflight_bytes() const noexcept { return inflight_bytes_; }
    [[nodiscard]] std::size_t buffered_bytes() const noexcept { return ready_bytes_ + inflight_bytes_; }
    [[nodiscard]] std::size_t active_extent_count() const noexcept { return active_extent_count_; }
    [[nodiscard]] std::uint64_t total_appended_bytes() const noexcept { return total_appended_bytes_; }
    [[nodiscard]] std::uint64_t final_size() const noexcept { return final_size_; }
    [[nodiscard]] std::uint64_t reset_error_code() const noexcept { return reset_error_code_; }
    [[nodiscard]] bool empty() const noexcept {
        return buffered_bytes() == 0 && (reset_sent_ || !fin_appended_ || fin_acked_);
    }
    [[nodiscard]] bool has_final_size() const noexcept { return fin_appended_ || reset_sent_; }
    [[nodiscard]] bool reset_sent() const noexcept { return reset_sent_; }
    [[nodiscard]] bool send_closed() const noexcept { return reset_sent_ || fin_appended_; }
    [[nodiscard]] bool can_append() const noexcept { return !reset_sent_ && !fin_appended_ && !fin_acked_; }
    [[nodiscard]] bool has_append_waiter() const noexcept { return append_waiter_ != nullptr; }

private:
    class AppendAwaiter;

    [[nodiscard]] common::IoResult<PreparedFrameResult> prepare_stream_frame(std::uint64_t stream_id,
                                                                             std::size_t capacity) noexcept;
    [[nodiscard]] common::IoResult<EncodedFrameResult> encode_stream_frame(std::uint64_t stream_id, std::uint8_t *dst,
                                                                           std::size_t capacity) noexcept;
    [[nodiscard]] common::IoResult<void> mark_acked(std::size_t offset, std::size_t length, bool encoded_fin) noexcept;
    [[nodiscard]] common::IoResult<void> mark_failed(std::size_t offset, std::size_t length, bool encoded_fin) noexcept;
    void update_max_stream_data(std::uint64_t limit) noexcept;
    [[nodiscard]] bool has_pending_fin() const noexcept { return fin_appended_ && !fin_inflight_ && !fin_acked_; }
    [[nodiscard]] bool is_last_ready_extent(const mem::IoBufNode *extent) const noexcept;
    [[nodiscard]] bool can_append_now(std::size_t bytes) const noexcept;
    [[nodiscard]] bool has_send_work() const noexcept {
        return !reset_sent_ && (ready_bytes_ != 0 || has_pending_fin());
    }
    [[nodiscard]] common::IoErr terminal_append_error() const noexcept;
    [[nodiscard]] common::IoResult<void> check_append_preconditions(std::size_t bytes) const noexcept;
    void notify_append_waiter(common::IoErr result = common::IoErr::None) noexcept;
    void cancel_append_waiter(AppendAwaiter *awaiter) noexcept;
    void clear_extents() noexcept;
    void try_merge_with_next(mem::IoBufNode *extent) noexcept;

    mem::IoBufNodePool *pool_ = nullptr;
    mem::IoBufNode *head_ = nullptr;
    mem::IoBufNode *tail_ = nullptr;
    mem::IoBufNode *ready_head_ = nullptr;
    std::size_t buffer_limit_ = 0;
    std::size_t ready_bytes_ = 0;
    std::size_t inflight_bytes_ = 0;
    std::size_t active_extent_count_ = 0;
    std::uint64_t max_stream_data_ = 0;
    std::uint64_t total_appended_bytes_ = 0;
    std::uint64_t final_size_ = 0;
    std::uint64_t reset_error_code_ = 0;
    AppendAwaiter *append_waiter_ = nullptr;
    bool fin_appended_ = false;
    bool fin_inflight_ = false;
    bool fin_acked_ = false;
    bool reset_sent_ = false;

    friend class QuicStream;
    friend struct QuicStreamSendQueueFrameTestAccess;
    friend struct QuicStreamSendQueueTestAccess;
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_STREAM_SEND_QUEUE_H
