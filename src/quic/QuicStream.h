#ifndef FIBER_QUIC_QUIC_STREAM_H
#define FIBER_QUIC_QUIC_STREAM_H

#include <chrono>
#include <cstdint>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/IoBufChain.h"
#include "QuicFrame.h"
#include "QuicStreamRecvQueue.h"
#include "QuicStreamSendQueue.h"

namespace fiber::quic {

class QuicConnection;

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

class QuicStream : public common::NonCopyable, public common::NonMovable {
public:
    struct Ops {
        void (*on_destroy)(void *owner) noexcept = nullptr;
        common::IoErr (*on_data)(void *owner, QuicStream &stream, mem::IoBufChain &&data, bool fin) noexcept = nullptr;
        void (*on_reset)(void *owner, QuicStream &stream, std::uint64_t error_code,
                         std::uint64_t final_size) noexcept = nullptr;
        void (*on_abort)(void *owner, common::IoErr reason) noexcept = nullptr;
    };

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

    QuicStream(std::uint64_t stream_id, mem::IoBufNodePool &recv_extent_pool) noexcept;
    QuicStream(std::uint64_t stream_id, mem::IoBufNodePool &recv_extent_pool, void *owner, const Ops &ops) noexcept;
    ~QuicStream();

    [[nodiscard]] std::uint64_t stream_id() const noexcept { return stream_id_; }
    [[nodiscard]] void *owner() noexcept { return owner_; }
    [[nodiscard]] const void *owner() const noexcept { return owner_; }
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
    [[nodiscard]] bool app_released() const noexcept { return app_released_; }
    [[nodiscard]] std::uint32_t ref_count() const noexcept { return ref_count_; }
    [[nodiscard]] Lease lease() noexcept { return Lease(this); }

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

    void mark_app_released() noexcept;
    void abort(common::IoErr reason) noexcept;

    [[nodiscard]] bool ready_for_connection_release() const noexcept;
    [[nodiscard]] bool ready_for_destruction() const noexcept;

    [[nodiscard]] static std::uint64_t stream_sequence(std::uint64_t stream_id) noexcept;
    [[nodiscard]] static bool is_bidirectional_stream_id(std::uint64_t stream_id) noexcept;
    [[nodiscard]] static bool is_unidirectional_stream_id(std::uint64_t stream_id) noexcept;

private:
    void attach_to_connection(QuicConnection &conn) noexcept;
    void detach_from_connection() noexcept;
    [[nodiscard]] common::IoResult<std::uint64_t> on_stream_data_recv(const std::uint8_t *src, std::size_t length,
                                                                      std::uint64_t offset, bool fin) noexcept;
    [[nodiscard]] common::IoResult<std::uint64_t> on_remote_reset(std::uint64_t error_code,
                                                                  std::uint64_t final_size) noexcept;
    [[nodiscard]] common::IoResult<void> on_remote_stop_sending(std::uint64_t error_code) noexcept;
    void on_max_stream_data(std::uint64_t limit) noexcept;
    void maybe_extend_recv_flow_control() noexcept;
    void retain() noexcept;
    void release() noexcept;
    [[nodiscard]] common::IoResult<void> set_final_size(std::uint64_t final_size) noexcept;
    void sync_recv_state_from_queue() noexcept;

    std::uint64_t stream_id_ = 0;
    QuicConnection *conn_ = nullptr;
    void *owner_ = nullptr;
    const Ops *ops_ = nullptr;
    QuicStreamRecvQueue recv_queue_;
    QuicStreamSendQueue send_queue_;
    QuicStreamRecvState recv_state_ = QuicStreamRecvState::Open;
    std::uint64_t final_size_ = 0;
    std::uint64_t reset_error_code_ = 0;
    std::uint32_t ref_count_ = 1;
    bool has_final_size_ = false;
    bool attached_to_connection_ = false;
    bool app_released_ = true;

    friend class QuicConnection;
    friend class QuicStreamTable;
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_STREAM_H
