#ifndef FIBER_QUIC_QUIC_SEND_SCHEDULER_H
#define FIBER_QUIC_QUIC_SEND_SCHEDULER_H

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "../async/Task.h"
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
inline constexpr std::size_t kQuicSendMaxFramesPerDatagram = 8;

enum class QuicBuildSendStatus : std::uint8_t {
    Encoded,
    NoWork,
    Blocked,
    Closed,
};

struct QuicBuildSendResult {
    QuicBuildSendStatus status = QuicBuildSendStatus::NoWork;
};

struct QuicSendDatagram {
    std::uint8_t *data = nullptr;
    std::size_t capacity = 0;
    std::size_t length = 0;
    QuicPath *path = nullptr;
    net::UdpPacketSendSpec spec{};
    QuicEncryptionLevel level = QuicEncryptionLevel::Initial;
    QuicPacketNumberSpaceSnapshot packet_number_snapshot{};
    std::uint64_t packet_number = 0;
    QuicFrame frames[kQuicSendMaxFramesPerDatagram]{};
    QuicFrame *source_frames[kQuicSendMaxFramesPerDatagram]{};
    std::size_t frame_count = 0;
    bool sends_ack = false;
    bool ack_eliciting = false;
};

class QuicSendScheduler : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        std::size_t send_buffer_size = kQuicSendDefaultBufferSize;
        std::size_t max_packets_per_wakeup = 64;
        std::size_t max_packets_per_connection = 8;
    };

    QuicSendScheduler() noexcept;
    ~QuicSendScheduler();

    [[nodiscard]] common::IoResult<void> init(event::EventLoop &loop, net::UdpSocket &socket, QuicUdpEndpoint &endpoint,
                                              const Options &options) noexcept;
    void submit(QuicConnection &connection) noexcept;
    void remove(QuicConnection &connection) noexcept;
    void close(common::IoErr reason = common::IoErr::Canceled) noexcept;

    [[nodiscard]] async::Task<void> run() noexcept;

    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    [[nodiscard]] bool running() const noexcept { return running_; }
    [[nodiscard]] common::IoErr stop_reason() const noexcept { return stop_reason_; }

private:
    class WaitForWorkAwaiter;
    class DeferAwaiter;

    using ReadyList = common::IntrusiveList<QuicConnection::SendIndex, offsetof(QuicConnection::SendIndex, link)>;

    [[nodiscard]] bool should_wake_waiter() const noexcept;
    [[nodiscard]] bool arm_waiter(WaitForWorkAwaiter *awaiter) noexcept;
    void cancel_waiter(WaitForWorkAwaiter *awaiter) noexcept;
    void notify_waiter() noexcept;
    [[nodiscard]] bool has_work() const noexcept;
    void enqueue_ready(QuicConnection &connection) noexcept;
    [[nodiscard]] QuicConnection *pop_ready() noexcept;
    void clear_ready() noexcept;
    [[nodiscard]] async::Task<common::IoErr> flush_connection(QuicConnection &connection) noexcept;

    event::EventLoop *loop_ = nullptr;
    net::UdpSocket *socket_ = nullptr;
    QuicUdpEndpoint *endpoint_ = nullptr;
    Options options_{};
    std::unique_ptr<std::uint8_t[]> send_buffer_{};
    ReadyList ready_{};
    WaitForWorkAwaiter *waiter_ = nullptr;
    QuicConnection *blocked_connection_ = nullptr;
    common::IoErr stop_reason_ = common::IoErr::None;
    bool initialized_ = false;
    bool closing_ = false;
    bool running_ = false;
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_SEND_SCHEDULER_H
