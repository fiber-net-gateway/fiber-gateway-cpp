#ifndef FIBER_QUIC_QUIC_SEND_SCHEDULER_H
#define FIBER_QUIC_QUIC_SEND_SCHEDULER_H

#include <cstddef>
#include <cstdint>

#include "../common/IntrusiveList.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "../net/UdpSocket.h"
#include "QuicConnection.h"

namespace fiber::quic {

class QuicUdpEndpoint;

inline constexpr std::size_t kQuicSendDefaultBufferSize = 65536;
inline constexpr std::size_t kQuicSendLevelCount = 4;

enum class QuicBuildMode : std::uint8_t {
    Normal,
    PacingExemptOnly,
};

enum class QuicBuildSendStatus : std::uint8_t {
    Encoded,
    NoWork,
    Blocked,
    Closed,
};

struct QuicBuildSendResult {
    QuicBuildSendStatus status = QuicBuildSendStatus::NoWork;
};

struct QuicSendPacketRecord {
    QuicEncryptionLevel level = QuicEncryptionLevel::Initial;
    std::size_t length = 0;
    std::uint64_t packet_number = 0;
    std::size_t frame_count = 0;
    bool sends_ack = false;
    bool ack_eliciting = false;
};

struct QuicSendDatagram {
    std::uint8_t *data = nullptr;
    std::size_t capacity = 0;
    std::size_t length = 0;
    QuicPath *path = nullptr;
    net::UdpPacketSendSpec spec{};
    QuicSendPacketRecord packets[kQuicSendLevelCount]{};
    std::size_t packet_count = 0;
    bool mtu_probe = false;
    bool pacing_controlled = false;
};

struct QuicSendPathReservation {
    QuicPath *path = nullptr;
    std::uint64_t sent = 0;
    std::uint32_t ecn_validation_sent = 0;
};

struct QuicSendBuildState {
    QuicCongestionState congestion{};
    QuicPacerState pacer{};
    QuicSendPathReservation paths[kQuicMaxPaths]{};
};

class QuicSendScheduler : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        std::size_t send_buffer_size = kQuicSendDefaultBufferSize;
        std::size_t max_datagrams_per_batch = net::kUdpMaxBatchSize;
        std::size_t max_gso_segments = net::kUdpMaxBatchSize;
        std::size_t max_packets_per_wakeup = 64;
        std::size_t max_packets_per_connection = 64;
        QuicPacingOptions pacing{};
        bool enable_gso = true;
    };

    QuicSendScheduler() noexcept;
    ~QuicSendScheduler();

    struct PumpResult {
        std::size_t packets_sent = 0;
        bool write_blocked = false;
        bool needs_reschedule = false;
    };

    [[nodiscard]] common::IoResult<void> init(event::EventLoop &loop, net::UdpSocket &socket, QuicUdpEndpoint &endpoint,
                                              const Options &options) noexcept;
    void submit(QuicConnection &connection) noexcept;
    void remove(QuicConnection &connection) noexcept;
    void close(common::IoErr reason = common::IoErr::Canceled) noexcept;

    [[nodiscard]] PumpResult pump() noexcept;

    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    [[nodiscard]] bool has_work() const noexcept { return !ready_.empty(); }
    [[nodiscard]] common::IoErr stop_reason() const noexcept { return stop_reason_; }

private:
    struct FlushResult {
        common::IoErr error = common::IoErr::None;
        std::size_t packets_sent = 0;
    };

    using ReadyList =
            common::IntrusiveList<QuicConnection::SendQueueEntry, offsetof(QuicConnection::SendQueueEntry, link)>;

    void enqueue_ready(QuicConnection &connection) noexcept;
    void rotate_front_to_back(QuicConnection &connection) noexcept;
    [[nodiscard]] QuicConnection *front_ready() noexcept;
    void clear_ready() noexcept;
    [[nodiscard]] FlushResult flush_connection(QuicConnection &connection) noexcept;

    event::EventLoop *loop_ = nullptr;
    net::UdpSocket *socket_ = nullptr;
    QuicUdpEndpoint *endpoint_ = nullptr;
    Options options_{};
    ReadyList ready_{};
    common::IoErr stop_reason_ = common::IoErr::None;
    bool initialized_ = false;
    bool closing_ = false;
    bool gso_enabled_ = false;
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_SEND_SCHEDULER_H
