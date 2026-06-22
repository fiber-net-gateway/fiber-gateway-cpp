#ifndef FIBER_QUIC_QUIC_PATH_MANAGER_H
#define FIBER_QUIC_QUIC_PATH_MANAGER_H

#include <array>
#include <cstddef>
#include <cstdint>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "../net/SocketAddress.h"
#include "QuicCongestion.h"
#include "QuicConnectionId.h"
#include "QuicFrame.h"
#include "QuicPath.h"

namespace fiber::quic {

class QuicConnection;
struct QuicPathChallengeFrame;

class QuicPathManager : public common::NonCopyable, public common::NonMovable {
public:
    explicit QuicPathManager(QuicConnection &connection) noexcept;
    ~QuicPathManager();

    [[nodiscard]] std::array<QuicPath, kQuicMaxPaths> &paths() noexcept { return paths_; }
    [[nodiscard]] const std::array<QuicPath, kQuicMaxPaths> &paths() const noexcept { return paths_; }
    [[nodiscard]] QuicPath *active() noexcept { return active_; }
    [[nodiscard]] const QuicPath *active() const noexcept { return active_; }
    [[nodiscard]] std::size_t count() const noexcept;

    [[nodiscard]] QuicPath *find(const net::SocketAddress &remote, const net::SocketAddress &local) noexcept;
    [[nodiscard]] const QuicPath *find(const net::SocketAddress &remote,
                                       const net::SocketAddress &local) const noexcept;
    [[nodiscard]] QuicPath *find(QuicPathTag tag) noexcept;
    [[nodiscard]] const QuicPath *find(QuicPathTag tag) const noexcept;

    [[nodiscard]] QuicPath *create(const net::SocketAddress &remote, const net::SocketAddress &local,
                                   const QuicConnectionId &remote_connection_id, QuicPathTag tag) noexcept;
    void free(QuicPath &path) noexcept;
    [[nodiscard]] bool set_active(QuicPath &path) noexcept;

    void record_received(QuicPath &path, std::size_t len) noexcept;
    void record_sent(QuicPath &path, std::size_t len) noexcept;
    [[nodiscard]] bool has_send_work() const noexcept;
    [[nodiscard]] static std::size_t send_limit(const QuicPath &path, std::size_t size) noexcept;

    void clear_frames(QuicPath &path) noexcept;
    void clear_frames(QuicPath &path, QuicFrameType type) noexcept;

    [[nodiscard]] common::IoResult<void> queue_path_challenge_frame(QuicPath &path,
                                                                    const std::uint8_t data[8]) noexcept;
    [[nodiscard]] common::IoResult<void> queue_path_response_frame(QuicPath &path, const std::uint8_t data[8]) noexcept;

    [[nodiscard]] common::IoResult<void> recv_path_challenge_frame(QuicPath &path,
                                                                   const QuicPathChallengeFrame &frame) noexcept;
    [[nodiscard]] common::IoResult<bool> recv_path_response_frame(const QuicPathChallengeFrame &frame,
                                                                  QuicTime now) noexcept;
    [[nodiscard]] common::IoResult<void> handle_migration(QuicPath &path, bool rebound, QuicTime now) noexcept;

    [[nodiscard]] common::IoResult<void> start_validation(QuicPath &path, QuicTime now) noexcept;
    [[nodiscard]] common::IoResult<bool> expire_validation(QuicPath &path, QuicTime now) noexcept;

    void arm_validation_timer(event::EventLoop &loop) noexcept;
    void cancel_validation_timer(event::EventLoop &loop) noexcept;
    [[nodiscard]] bool validation_timer_armed() const noexcept { return validation_timer_entry_.is_in_heap(); }

    [[nodiscard]] QuicTime validation_delay() const noexcept;
    [[nodiscard]] QuicTime validation_delay(std::uint32_t tries) const noexcept;

private:
    static void on_validation_timer(QuicPathManager *manager) noexcept;

    QuicConnection &connection_;
    std::array<QuicPath, kQuicMaxPaths> paths_{};
    QuicPath *active_ = nullptr;
    std::uint64_t next_seqnum_ = 0;
    event::EventLoop::TimerEntry validation_timer_entry_{};
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_PATH_MANAGER_H
