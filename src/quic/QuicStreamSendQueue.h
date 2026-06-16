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
#include "../event/EventLoop.h"
#include "QuicStreamSendBuffer.h"

namespace fiber::quic {

inline constexpr std::size_t kQuicStreamSendDefaultBufferLimit = 64 * 1024;

class QuicStreamSendQueue : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        std::size_t buffer_limit = kQuicStreamSendDefaultBufferLimit;
        std::uint64_t max_stream_data = 0;
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

    [[nodiscard]] common::IoResult<QuicStreamSendBuffer::EncodedFrameResult>
    encode_stream_frame(std::uint64_t stream_id, std::uint8_t *dst, std::size_t capacity) noexcept;
    [[nodiscard]] common::IoResult<void> mark_acked(std::size_t offset, std::size_t length, bool encoded_fin) noexcept;
    [[nodiscard]] common::IoResult<void> mark_failed(std::size_t offset, std::size_t length, bool encoded_fin) noexcept;
    [[nodiscard]] common::IoResult<std::uint64_t> mark_reset() noexcept;

    void update_max_stream_data(std::uint64_t limit) noexcept;
    void close(common::IoErr reason = common::IoErr::Canceled) noexcept;

    [[nodiscard]] const QuicStreamSendBuffer &buffer() const noexcept { return buffer_; }
    [[nodiscard]] QuicStreamSendBuffer &buffer() noexcept { return buffer_; }
    [[nodiscard]] std::size_t buffer_limit() const noexcept { return buffer_limit_; }
    [[nodiscard]] std::uint64_t max_stream_data() const noexcept { return max_stream_data_; }
    [[nodiscard]] std::size_t buffer_available() const noexcept;
    [[nodiscard]] std::uint64_t stream_data_available() const noexcept;
    [[nodiscard]] bool closed() const noexcept { return closed_; }
    [[nodiscard]] common::IoErr close_reason() const noexcept { return close_reason_; }
    [[nodiscard]] bool has_append_waiter() const noexcept { return append_waiter_ != nullptr; }

private:
    class AppendAwaiter;

    [[nodiscard]] bool can_append_now(std::size_t bytes) const noexcept;
    [[nodiscard]] common::IoErr terminal_append_error() const noexcept;
    [[nodiscard]] common::IoResult<void> check_append_preconditions(std::size_t bytes) const noexcept;
    void notify_append_waiter(common::IoErr result = common::IoErr::None) noexcept;
    void cancel_append_waiter(AppendAwaiter *awaiter) noexcept;

    QuicStreamSendBuffer buffer_;
    std::size_t buffer_limit_ = 0;
    std::uint64_t max_stream_data_ = 0;
    AppendAwaiter *append_waiter_ = nullptr;
    common::IoErr close_reason_ = common::IoErr::None;
    bool closed_ = false;
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_STREAM_SEND_QUEUE_H
