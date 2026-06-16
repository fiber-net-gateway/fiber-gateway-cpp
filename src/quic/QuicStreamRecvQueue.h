#ifndef FIBER_QUIC_QUIC_STREAM_RECV_QUEUE_H
#define FIBER_QUIC_QUIC_STREAM_RECV_QUEUE_H

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/IoBufChain.h"
#include "QuicFrame.h"
#include "QuicStreamReassembler.h"

namespace fiber::quic {

class QuicStream;
struct QuicStreamRecvQueueTestAccess;

inline constexpr std::size_t kQuicStreamRecvDefaultBufferLimit = 64 * 1024;
inline constexpr std::size_t kQuicStreamRecvDefaultLowWater = 16 * 1024;

class QuicStreamRecvQueue : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        std::size_t buffer_limit = kQuicStreamRecvDefaultBufferLimit;
        std::size_t low_water = kQuicStreamRecvDefaultLowWater;
        std::uint64_t max_stream_data = kQuicStreamRecvDefaultBufferLimit;
    };

    explicit QuicStreamRecvQueue(mem::IoBufNodePool &pool) noexcept;
    QuicStreamRecvQueue(mem::IoBufNodePool &pool, Options options) noexcept;
    ~QuicStreamRecvQueue();

    void stop_receiving(std::uint64_t error_code = 0, common::IoErr reason = common::IoErr::Canceled) noexcept;
    void close(common::IoErr reason = common::IoErr::Canceled) noexcept;

    [[nodiscard]] common::IoResult<std::size_t> try_take(std::size_t max_bytes, mem::IoBufChain &out) noexcept;
    [[nodiscard]] async::Task<common::IoResult<std::size_t>>
    take(std::size_t max_bytes, mem::IoBufChain &out,
         std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;

    [[nodiscard]] std::uint64_t next_read_offset() const noexcept { return reassembler_.next_read_offset(); }
    [[nodiscard]] std::uint64_t received_end_offset() const noexcept { return received_end_offset_; }
    [[nodiscard]] std::uint64_t max_stream_data() const noexcept { return max_stream_data_; }
    [[nodiscard]] std::uint64_t next_max_stream_data_limit() const noexcept;
    [[nodiscard]] bool should_extend_max_stream_data() const noexcept;
    [[nodiscard]] bool has_final_size() const noexcept { return has_final_size_; }
    [[nodiscard]] std::uint64_t final_size() const noexcept { return final_size_; }
    [[nodiscard]] bool finished() const noexcept { return reassembler_.finished(); }
    [[nodiscard]] bool reset_received() const noexcept { return reset_received_; }
    [[nodiscard]] bool stop_sending() const noexcept { return stop_sending_; }
    [[nodiscard]] std::uint64_t reset_error_code() const noexcept { return reset_error_code_; }
    [[nodiscard]] std::uint64_t stop_error_code() const noexcept { return stop_error_code_; }
    [[nodiscard]] std::size_t buffered_bytes() const noexcept { return reassembler_.buffered_bytes(); }
    [[nodiscard]] std::size_t buffer_limit() const noexcept { return buffer_limit_; }
    [[nodiscard]] std::size_t low_water() const noexcept { return low_water_; }
    [[nodiscard]] std::size_t active_block_count() const noexcept { return reassembler_.active_block_count(); }
    [[nodiscard]] mem::IoBufNodePool &node_pool() noexcept { return reassembler_.node_pool(); }
    [[nodiscard]] bool has_read_waiter() const noexcept { return read_waiter_ != nullptr; }

private:
    class ReadAwaiter;

    [[nodiscard]] common::IoResult<std::size_t> recv_stream_data(std::uint64_t offset, QuicSlice data,
                                                                 bool fin) noexcept;
    [[nodiscard]] common::IoResult<void> recv_reset(std::uint64_t error_code, std::uint64_t final_size) noexcept;
    void update_max_stream_data(std::uint64_t limit) noexcept;
    [[nodiscard]] common::IoResult<void> set_final_size(std::uint64_t final_size) noexcept;
    [[nodiscard]] common::IoResult<void> check_insert_limits(std::uint64_t offset, std::size_t len) const noexcept;
    [[nodiscard]] bool can_take_now() const noexcept;
    [[nodiscard]] common::IoErr terminal_read_error() const noexcept;
    void notify_read_waiter(common::IoErr result = common::IoErr::None) noexcept;
    void cancel_read_waiter(ReadAwaiter *awaiter) noexcept;

    QuicStreamReassembler reassembler_;
    std::size_t buffer_limit_ = 0;
    std::size_t low_water_ = 0;
    std::uint64_t max_stream_data_ = 0;
    std::uint64_t received_end_offset_ = 0;
    std::uint64_t final_size_ = 0;
    std::uint64_t reset_error_code_ = 0;
    std::uint64_t stop_error_code_ = 0;
    ReadAwaiter *read_waiter_ = nullptr;
    common::IoErr stop_reason_ = common::IoErr::Canceled;
    common::IoErr close_reason_ = common::IoErr::Canceled;
    bool has_final_size_ = false;
    bool reset_received_ = false;
    bool stop_sending_ = false;
    bool closed_ = false;

    friend class QuicStream;
    friend struct QuicStreamRecvQueueTestAccess;
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_STREAM_RECV_QUEUE_H
