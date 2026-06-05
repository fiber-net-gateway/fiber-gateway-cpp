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
inline constexpr std::size_t kQuicSendLevelCount = 3;

enum class QuicBuildSendStatus : std::uint8_t {
    Encoded,
    NoWork,
    Blocked,
    Delayed,
    Closed,
};

struct QuicBuildSendResult {
    QuicBuildSendStatus status = QuicBuildSendStatus::NoWork;
    std::chrono::milliseconds delay{0};
};

struct QuicSendPacketRecord {
    QuicEncryptionLevel level = QuicEncryptionLevel::Initial;
    std::size_t length = 0;
    QuicPacketNumberSpaceSnapshot packet_number_snapshot{};
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
    QuicPacketNumberSpaceSnapshot packet_number_snapshots[kQuicSendLevelCount]{};
    bool packet_number_snapshot_valid[kQuicSendLevelCount]{};
    QuicSendPacketRecord packets[kQuicSendLevelCount]{};
    std::size_t packet_count = 0;
};

class QuicSendScheduler : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        std::size_t send_buffer_size = kQuicSendDefaultBufferSize;
        std::size_t max_packets_per_wakeup = 64;
        std::size_t max_packets_per_connection = 64;
    };

    QuicSendScheduler() noexcept;
    ~QuicSendScheduler();

    [[nodiscard]] common::IoResult<void> init(event::EventLoop &loop, net::UdpSocket &socket, QuicUdpEndpoint &endpoint,
                                              const Options &options) noexcept;
    void submit(QuicConnection &connection) noexcept;
    void submit_after(QuicConnection &connection, std::chrono::milliseconds delay) noexcept;
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
    [[nodiscard]] bool has_due_delayed() const noexcept;
    void promote_due_delayed() noexcept;
    void arm_delay_timer() noexcept;
    void enqueue_ready(QuicConnection &connection) noexcept;
    void enqueue_delayed(QuicConnection &connection, std::chrono::steady_clock::time_point ready_at) noexcept;
    [[nodiscard]] QuicConnection *pop_ready() noexcept;
    void clear_ready() noexcept;
    void clear_delayed() noexcept;
    [[nodiscard]] async::Task<common::IoErr> flush_connection(QuicConnection &connection) noexcept;
    static void on_delay_timer(QuicSendScheduler *scheduler) noexcept;

    event::EventLoop *loop_ = nullptr;
    net::UdpSocket *socket_ = nullptr;
    QuicUdpEndpoint *endpoint_ = nullptr;
    Options options_{};
    std::unique_ptr<std::uint8_t[]> send_buffer_{};
    ReadyList ready_{};
    ReadyList delayed_{};
    event::EventLoop::TimerEntry delay_timer_{};
    WaitForWorkAwaiter *waiter_ = nullptr;
    QuicConnection *blocked_connection_ = nullptr;
    common::IoErr stop_reason_ = common::IoErr::None;
    bool initialized_ = false;
    bool closing_ = false;
    bool running_ = false;
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_SEND_SCHEDULER_H
