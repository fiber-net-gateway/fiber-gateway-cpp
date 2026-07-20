#ifndef FIBER_QUIC_QUIC_STREAM_H
#define FIBER_QUIC_QUIC_STREAM_H

#include <chrono>
#include <cstdint>

#include "../async/Task.h"
#include "../common/IntrusiveList.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/IoBufChain.h"
#include "QuicFrame.h"
#include "QuicStreamRecvQueue.h"
#include "QuicStreamSendQueue.h"

namespace fiber::quic {

enum class QuicStreamEarlyDataMode : std::uint8_t {
    OneRttOnly,
    ReplaySafe,
};

class QuicConnection;
class QuicUdpEndpoint;

enum class QuicStreamType : std::uint8_t {
    Bidirectional,
    Unidirectional,
};

enum class QuicStreamRecvState : std::uint8_t {
    Open,
    SizeKnown,
    ResetRecvd,
    Stopped,
    Closed,
};

enum class QuicStreamFrameEncodeStatus : std::uint8_t {
    Encoded,
    Skipped,
    Blocked,
};

inline constexpr std::uint64_t kQuicUnassignedStreamId = UINT64_MAX;

class QuicStream : public common::NonCopyable, public common::NonMovable {
public:
    using DestroyCallback = void (*)(void *owner, QuicStream &stream) noexcept;

    class Lease {
    public:
        Lease() noexcept = default;
        explicit Lease(QuicStream *stream) noexcept : stream_(stream) {
            if (stream_) {
                stream_->retain();
            }
        }

        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;

        Lease(Lease &&other) noexcept : stream_(other.stream_) { other.stream_ = nullptr; }

        Lease &operator=(Lease &&other) noexcept {
            if (this == &other) {
                return *this;
            }
            reset();
            stream_ = other.stream_;
            other.stream_ = nullptr;
            return *this;
        }

        ~Lease() { reset(); }

        void reset() noexcept;
        [[nodiscard]] QuicStream *release_raw() noexcept {
            QuicStream *stream = stream_;
            stream_ = nullptr;
            return stream;
        }
        [[nodiscard]] QuicStream *get() const noexcept { return stream_; }
        [[nodiscard]] QuicStream &operator*() const noexcept { return *stream_; }
        [[nodiscard]] QuicStream *operator->() const noexcept { return stream_; }
        [[nodiscard]] explicit operator bool() const noexcept { return stream_ != nullptr; }

        [[nodiscard]] static Lease adopt(QuicStream *stream) noexcept {
            Lease lease;
            lease.stream_ = stream;
            return lease;
        }

    private:
        QuicStream *stream_ = nullptr;
    };

    QuicStream(void *destroy_owner, DestroyCallback on_destroy) noexcept;
    ~QuicStream();

    [[nodiscard]] std::uint64_t stream_id() const noexcept { return stream_id_; }
    [[nodiscard]] bool stream_id_assigned() const noexcept { return stream_id_ != kQuicUnassignedStreamId; }
    [[nodiscard]] std::uint64_t sequence() const noexcept;
    [[nodiscard]] QuicStreamType type() const noexcept;
    [[nodiscard]] bool bidirectional() const noexcept;
    [[nodiscard]] bool unidirectional() const noexcept;
    [[nodiscard]] QuicStreamRecvState recv_state() const noexcept {
        if (recv_queue_.reset_received()) {
            return QuicStreamRecvState::ResetRecvd;
        }
        if (recv_queue_.stop_sending()) {
            return QuicStreamRecvState::Stopped;
        }
        if (recv_queue_.finished()) {
            return QuicStreamRecvState::Closed;
        }
        return recv_queue_.has_final_size() ? QuicStreamRecvState::SizeKnown : QuicStreamRecvState::Open;
    }
    [[nodiscard]] bool has_final_size() const noexcept { return recv_queue_.has_final_size(); }
    [[nodiscard]] std::uint64_t final_size() const noexcept { return recv_queue_.final_size(); }
    [[nodiscard]] std::uint64_t reset_error_code() const noexcept { return recv_queue_.reset_error_code(); }
    [[nodiscard]] std::uint64_t stop_error_code() const noexcept { return recv_queue_.stop_error_code(); }
    [[nodiscard]] bool reset_received() const noexcept { return recv_queue_.reset_received(); }
    [[nodiscard]] bool stop_sending() const noexcept { return recv_queue_.stop_sending(); }
    [[nodiscard]] bool recv_closed() const noexcept { return recv_queue_.finished(); }
    [[nodiscard]] bool attached_to_connection() const noexcept { return attached_to_connection_; }
    [[nodiscard]] std::uint32_t ref_count() const noexcept { return ref_count_; }
    [[nodiscard]] void *owner() noexcept { return destroy_owner_; }
    [[nodiscard]] const void *owner() const noexcept { return destroy_owner_; }
    [[nodiscard]] DestroyCallback destroy_callback() const noexcept { return on_destroy_; }
    [[nodiscard]] Lease lease() noexcept { return Lease(this); }
    [[nodiscard]] QuicStreamEarlyDataMode early_data_mode() const noexcept { return early_data_mode_; }
    [[nodiscard]] bool created_during_early_data() const noexcept { return created_during_early_data_; }

    [[nodiscard]] common::IoResult<std::size_t> try_read(std::size_t max_bytes, mem::IoBufChain &out) noexcept;
    [[nodiscard]] async::Task<common::IoResult<std::size_t>>
    read(std::size_t max_bytes, mem::IoBufChain &out,
         std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] common::IoResult<std::size_t> try_write(const mem::IoBuf &buf, bool fin = false) noexcept;
    [[nodiscard]] common::IoResult<std::size_t> try_write(mem::IoBufChain &chain) noexcept;
    [[nodiscard]] async::Task<common::IoResult<std::size_t>>
    write(mem::IoBuf buf, bool fin = false,
          std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] async::Task<common::IoResult<std::size_t>>
    write(mem::IoBufChain &chain, std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] common::IoResult<void> stop_read(std::uint64_t error_code = 0) noexcept;
    [[nodiscard]] common::IoResult<void> reset(std::uint64_t error_code = 0) noexcept;
    void close(std::uint64_t error_code = 0) noexcept;

    // Compatibility shim for callers that still use the old connection-close
    // notification name.
    void notify_connection_closing() noexcept { close(); }

    [[nodiscard]] bool ready_for_connection_release() const noexcept;
    [[nodiscard]] bool ready_for_destruction() const noexcept;

    [[nodiscard]] static std::uint64_t stream_sequence(std::uint64_t stream_id) noexcept;
    [[nodiscard]] static bool is_bidirectional_stream_id(std::uint64_t stream_id) noexcept;
    [[nodiscard]] static bool is_unidirectional_stream_id(std::uint64_t stream_id) noexcept;

private:
    class WriteAwaiter;

    void assign_conn_ctx(QuicConnection &conn, std::uint64_t stream_id, QuicStreamRecvQueue::Options recv_options,
                         bool local_initiated,
                         QuicStreamEarlyDataMode early_data_mode = QuicStreamEarlyDataMode::OneRttOnly) noexcept;
    void detach_from_connection() noexcept;
    [[nodiscard]] common::IoResult<std::uint64_t> on_stream_data_recv(mem::IoBuf data, std::uint64_t offset,
                                                                      bool fin) noexcept;
    [[nodiscard]] common::IoResult<std::uint64_t> on_remote_reset(std::uint64_t error_code,
                                                                  std::uint64_t final_size) noexcept;
    [[nodiscard]] common::IoResult<void> on_remote_stop_sending(std::uint64_t error_code) noexcept;
    void on_max_stream_data(std::uint64_t limit) noexcept;
    [[nodiscard]] bool has_send_work() const noexcept;
    [[nodiscard]] common::IoResult<QuicStreamFrameEncodeStatus>
    encode_stream_frame(QuicOutputFrame &frame, std::uint8_t *dst, std::size_t capacity) noexcept;
    [[nodiscard]] common::IoResult<void> mark_send_acked(std::size_t offset, std::size_t length, bool fin) noexcept;
    [[nodiscard]] common::IoResult<void> mark_send_failed(std::size_t offset, std::size_t length, bool fin) noexcept;
    void maybe_extend_recv_flow_control() noexcept;
    void retain() noexcept;
    void release() noexcept;
    void sync_recv_state_from_queue() noexcept;
    [[nodiscard]] std::uint64_t stream_data_available() const noexcept;
    [[nodiscard]] std::size_t write_available() const noexcept;
    [[nodiscard]] bool blocked_by_connection_window() const noexcept;
    void maybe_report_write_flow_blocked() noexcept;
    [[nodiscard]] bool should_retransmit_stream_data_blocked(std::uint64_t limit) const noexcept;
    [[nodiscard]] common::IoErr terminal_write_error() const noexcept;
    void notify_write_waiter(common::IoErr result = common::IoErr::None) noexcept;
    void cancel_write_waiter(WriteAwaiter *awaiter) noexcept;

    std::uint64_t stream_id_ = kQuicUnassignedStreamId;
    QuicConnection *conn_ = nullptr;
    QuicStreamRecvQueue recv_queue_;
    QuicStreamSendQueue send_queue_;
    QuicStreamRecvState recv_state_ = QuicStreamRecvState::Open;
    std::uint64_t max_stream_data_ = 0;
    WriteAwaiter *write_waiter_ = nullptr;
    std::uint64_t last_stream_data_blocked_limit_ = 0;
    std::uint32_t ref_count_ = 1;
    common::IoErr terminal_error_ = common::IoErr::None;
    bool attached_to_connection_ = false;
    bool local_initiated_ = false;
    QuicStreamEarlyDataMode early_data_mode_ = QuicStreamEarlyDataMode::OneRttOnly;
    bool created_during_early_data_ = false;
    bool closed_ = false;
    bool stream_send_pending_ = false;
    bool stream_data_blocked_reported_ = false;
    void *destroy_owner_ = nullptr;
    DestroyCallback on_destroy_ = nullptr;

    friend class QuicConnection;
    friend class QuicUdpEndpoint;
    friend class QuicStreamTable;
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_STREAM_H
