#ifndef FIBER_QUIC_QUIC_FRAME_H
#define FIBER_QUIC_QUIC_FRAME_H

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"

namespace fiber::quic {

inline constexpr std::size_t kMaxConnectionIdLength = 20;
inline constexpr std::size_t kStatelessResetTokenLength = 16;

enum class QuicEncryptionLevel : std::uint8_t {
    Initial = 0,
    EarlyData = 1,
    Handshake = 2,
    Application = 3,
};

enum class QuicFrameType : std::uint64_t {
    Padding = 0x00,
    Ping = 0x01,
    Ack = 0x02,
    AckEcn = 0x03,
    ResetStream = 0x04,
    StopSending = 0x05,
    Crypto = 0x06,
    NewToken = 0x07,
    Stream = 0x08,
    Stream1 = 0x09,
    Stream2 = 0x0A,
    Stream3 = 0x0B,
    Stream4 = 0x0C,
    Stream5 = 0x0D,
    Stream6 = 0x0E,
    Stream7 = 0x0F,
    MaxData = 0x10,
    MaxStreamData = 0x11,
    MaxStreamsBidi = 0x12,
    MaxStreamsUni = 0x13,
    DataBlocked = 0x14,
    StreamDataBlocked = 0x15,
    StreamsBlockedBidi = 0x16,
    StreamsBlockedUni = 0x17,
    NewConnectionId = 0x18,
    RetireConnectionId = 0x19,
    PathChallenge = 0x1A,
    PathResponse = 0x1B,
    ConnectionClose = 0x1C,
    ConnectionCloseApp = 0x1D,
    HandshakeDone = 0x1E,
};

inline constexpr std::uint64_t kLastFrameType = static_cast<std::uint64_t>(QuicFrameType::HandshakeDone);

struct QuicSlice {
    const std::uint8_t *data = nullptr;
    std::size_t len = 0;

    [[nodiscard]] bool empty() const noexcept { return len == 0; }
};

struct QuicOutputFrameDataBlock {
    std::uint8_t *data = nullptr;
    std::size_t len = 0;
    std::uint32_t refs = 1;
};

struct QuicPaddingFrame {
    std::uint64_t length = 1;
};

struct QuicAckFrame {
    std::uint64_t largest = 0;
    std::uint64_t delay = 0;
    std::uint64_t range_count = 0;
    std::uint64_t first_range = 0;
    std::uint64_t ect0 = 0;
    std::uint64_t ect1 = 0;
    std::uint64_t ce = 0;
    std::uint64_t ranges_length = 0;
};

struct QuicCryptoFrame {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

struct QuicNewTokenFrame {
    std::uint64_t length = 0;
};

struct QuicStreamFrame {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::uint64_t stream_id = 0;
    bool has_offset = false;
    bool has_length = false;
    bool fin = false;
};

struct QuicCloseFrame {
    std::uint64_t error_code = 0;
    std::uint64_t frame_type = 0;
    QuicSlice reason{};
};

struct QuicOutputAckFrame {
    std::uint64_t largest = 0;
    std::uint64_t delay = 0;
    std::uint64_t range_count = 0;
    std::uint64_t first_range = 0;
    const std::uint8_t *ranges = nullptr;
    std::uint32_t ranges_length = 0;
    QuicOutputFrameDataBlock *owned_ranges = nullptr;
    std::uint64_t ect0 = 0;
    std::uint64_t ect1 = 0;
    std::uint64_t ce = 0;
};

struct QuicOutputCryptoFrame {
    std::uint64_t offset = 0;
    const std::uint8_t *data = nullptr;
    std::uint32_t length = 0;
    QuicOutputFrameDataBlock *owned = nullptr;
};

struct QuicOutputNewTokenFrame {
    const std::uint8_t *data = nullptr;
    std::uint32_t length = 0;
    QuicOutputFrameDataBlock *owned = nullptr;
};

struct QuicOutputStreamFrame {
    std::uint64_t stream_id = 0;
    std::uint64_t offset = 0;
    const std::uint8_t *data = nullptr;
    std::uint32_t length = 0;
    bool has_length = false;
    bool fin = false;
};

struct QuicOutputCloseFrame {
    std::uint64_t error_code = 0;
    std::uint64_t frame_type = 0;
    const std::uint8_t *reason = nullptr;
    std::uint32_t reason_length = 0;
    QuicOutputFrameDataBlock *owned_reason = nullptr;
};

struct QuicOutputMaxStreamsFrame {
    std::uint64_t limit = 0;
};

struct QuicOutputStreamsBlockedFrame {
    std::uint64_t limit = 0;
};

struct QuicResetStreamFrame {
    std::uint64_t id = 0;
    std::uint64_t error_code = 0;
    std::uint64_t final_size = 0;
};

struct QuicStopSendingFrame {
    std::uint64_t id = 0;
    std::uint64_t error_code = 0;
};

struct QuicMaxDataFrame {
    std::uint64_t max_data = 0;
};

struct QuicMaxStreamDataFrame {
    std::uint64_t id = 0;
    std::uint64_t limit = 0;
};

struct QuicMaxStreamsFrame {
    std::uint64_t limit = 0;
    bool bidirectional = true;
};

struct QuicDataBlockedFrame {
    std::uint64_t limit = 0;
};

struct QuicStreamDataBlockedFrame {
    std::uint64_t id = 0;
    std::uint64_t limit = 0;
};

struct QuicStreamsBlockedFrame {
    std::uint64_t limit = 0;
    bool bidirectional = true;
};

struct QuicNewConnectionIdFrame {
    std::uint64_t sequence_number = 0;
    std::uint64_t retire_prior_to = 0;
    std::uint8_t cid_len = 0;
    std::uint8_t cid[kMaxConnectionIdLength]{};
    std::uint8_t stateless_reset_token[kStatelessResetTokenLength]{};
};

struct QuicRetireConnectionIdFrame {
    std::uint64_t sequence_number = 0;
};

struct QuicPathChallengeFrame {
    std::uint8_t data[8]{};
};

struct QuicInputFrame {
    QuicInputFrame() noexcept : u{} {}

    QuicFrameType type = QuicFrameType::Padding;
    QuicEncryptionLevel level = QuicEncryptionLevel::Initial;
    bool ack_eliciting = false;
    QuicSlice data{};

    union Payload {
        QuicPaddingFrame padding;
        QuicAckFrame ack;
        QuicCryptoFrame crypto;
        QuicNewTokenFrame new_token;
        QuicStreamFrame stream;
        QuicCloseFrame close;
        QuicResetStreamFrame reset_stream;
        QuicStopSendingFrame stop_sending;
        QuicMaxDataFrame max_data;
        QuicMaxStreamDataFrame max_stream_data;
        QuicMaxStreamsFrame max_streams;
        QuicDataBlockedFrame data_blocked;
        QuicStreamDataBlockedFrame stream_data_blocked;
        QuicStreamsBlockedFrame streams_blocked;
        QuicNewConnectionIdFrame new_connection_id;
        QuicRetireConnectionIdFrame retire_connection_id;
        QuicPathChallengeFrame path_challenge;
        QuicPathChallengeFrame path_response;
    } u;
};

struct QuicOutputFrame {
    QuicOutputFrame() noexcept : u{} {}

    QuicFrameType type = QuicFrameType::Padding;
    std::uint64_t packet_number = 0;
    std::size_t encoded_len = 0;
    std::uint32_t packet_len = 0;
    std::chrono::milliseconds send_time{0};
    bool packet_ack_eliciting = false;
    bool ignore_loss = false;

    union Payload {
        QuicPaddingFrame padding;
        QuicOutputAckFrame ack;
        QuicOutputCryptoFrame crypto;
        QuicOutputNewTokenFrame new_token;
        QuicOutputStreamFrame stream;
        QuicOutputCloseFrame close;
        QuicResetStreamFrame reset_stream;
        QuicStopSendingFrame stop_sending;
        QuicMaxDataFrame max_data;
        QuicMaxStreamDataFrame max_stream_data;
        QuicOutputMaxStreamsFrame max_streams;
        QuicDataBlockedFrame data_blocked;
        QuicStreamDataBlockedFrame stream_data_blocked;
        QuicOutputStreamsBlockedFrame streams_blocked;
        QuicNewConnectionIdFrame new_connection_id;
        QuicRetireConnectionIdFrame retire_connection_id;
        QuicPathChallengeFrame path_challenge;
        QuicPathChallengeFrame path_response;
    } u;

    QuicOutputFrame *next = nullptr;
    bool queued = false;
};

class QuicOutputFrameQueue {
public:
    [[nodiscard]] bool empty() const noexcept { return head_ == nullptr; }
    [[nodiscard]] QuicOutputFrame *front() noexcept { return head_; }
    [[nodiscard]] const QuicOutputFrame *front() const noexcept { return head_; }
    [[nodiscard]] QuicOutputFrame *back() noexcept { return tail_; }
    [[nodiscard]] const QuicOutputFrame *back() const noexcept { return tail_; }
    [[nodiscard]] QuicOutputFrame *next_of(QuicOutputFrame &frame) noexcept { return frame.next; }
    [[nodiscard]] const QuicOutputFrame *next_of(const QuicOutputFrame &frame) const noexcept { return frame.next; }

    void push_front(QuicOutputFrame &frame) noexcept;
    void push_back(QuicOutputFrame &frame) noexcept;
    void insert_after(QuicOutputFrame &position, QuicOutputFrame &frame) noexcept;
    void erase(QuicOutputFrame &frame) noexcept;
    void erase_after(QuicOutputFrame *prev, QuicOutputFrame &frame) noexcept;
    [[nodiscard]] QuicOutputFrame *pop_front() noexcept;
    void prepend_all(QuicOutputFrameQueue &source) noexcept;

private:
    QuicOutputFrame *head_ = nullptr;
    QuicOutputFrame *tail_ = nullptr;
};

inline constexpr std::size_t kQuicOutputFramePoolMaxCached = 1024;

class QuicOutputFramePool : public common::NonCopyable, public common::NonMovable {
public:
    QuicOutputFramePool() noexcept = default;
    ~QuicOutputFramePool();

    [[nodiscard]] QuicOutputFrame *alloc() noexcept;
    void release(QuicOutputFrame *frame) noexcept;
    void clear() noexcept;

    [[nodiscard]] std::size_t cached_count() const noexcept { return cached_count_; }

private:
    QuicOutputFrame *free_head_ = nullptr;
    std::size_t cached_count_ = 0;
};

struct QuicInputFrameParseResult {
    QuicInputFrame frame{};
    std::size_t consumed = 0;
};

[[nodiscard]] inline bool quic_is_stream_frame_type(std::uint64_t type) noexcept {
    return type >= static_cast<std::uint64_t>(QuicFrameType::Stream) &&
           type <= static_cast<std::uint64_t>(QuicFrameType::Stream7);
}

[[nodiscard]] common::IoResult<void> quic_output_frame_set_owned_data(QuicOutputFrame &frame, const std::uint8_t *data,
                                                                      std::size_t len) noexcept;
void quic_output_frame_retain_data(QuicOutputFrame &frame) noexcept;
void quic_output_frame_release_data(QuicOutputFrame &frame) noexcept;
[[nodiscard]] bool quic_output_frame_ack_eliciting(QuicFrameType type) noexcept;
[[nodiscard]] bool quic_output_frame_retransmittable_on_loss(QuicFrameType type) noexcept;

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_FRAME_H
