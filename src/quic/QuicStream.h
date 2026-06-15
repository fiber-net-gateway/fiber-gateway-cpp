#ifndef FIBER_QUIC_QUIC_STREAM_H
#define FIBER_QUIC_QUIC_STREAM_H

#include <cstdint>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/IoBufChain.h"
#include "QuicFrame.h"
#include "QuicStreamReassembler.h"

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
    [[nodiscard]] QuicStreamRecvState recv_state() const noexcept { return recv_state_; }
    [[nodiscard]] bool has_final_size() const noexcept { return has_final_size_; }
    [[nodiscard]] std::uint64_t final_size() const noexcept { return final_size_; }
    [[nodiscard]] std::uint64_t reset_error_code() const noexcept { return reset_error_code_; }
    [[nodiscard]] bool reset_received() const noexcept { return recv_state_ == QuicStreamRecvState::ResetRecvd; }
    [[nodiscard]] bool recv_closed() const noexcept { return recv_state_ == QuicStreamRecvState::Closed; }
    [[nodiscard]] bool attached_to_connection() const noexcept { return attached_to_connection_; }
    [[nodiscard]] bool app_released() const noexcept { return app_released_; }
    [[nodiscard]] std::size_t buffered_recv_bytes() const noexcept { return reassembler_.buffered_bytes(); }
    [[nodiscard]] std::uint32_t ref_count() const noexcept { return ref_count_; }
    [[nodiscard]] Lease lease() noexcept { return Lease(this); }

    [[nodiscard]] common::IoResult<std::size_t> recv_stream_data(std::uint64_t offset, QuicSlice data,
                                                                 bool fin) noexcept;
    [[nodiscard]] common::IoResult<void> recv_reset(std::uint64_t error_code, std::uint64_t final_size) noexcept;
    [[nodiscard]] common::IoResult<std::size_t> take_recv_data(std::size_t max_bytes, mem::IoBufChain &out) noexcept;
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
    void retain() noexcept;
    void release() noexcept;
    [[nodiscard]] common::IoResult<void> set_final_size(std::uint64_t final_size) noexcept;

    std::uint64_t stream_id_ = 0;
    QuicConnection *conn_ = nullptr;
    void *owner_ = nullptr;
    const Ops *ops_ = nullptr;
    QuicStreamReassembler reassembler_;
    QuicStreamRecvState recv_state_ = QuicStreamRecvState::Open;
    std::uint64_t final_size_ = 0;
    std::uint64_t recv_highest_offset_ = 0;
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
