#include "QuicPathManager.h"

#include <algorithm>
#include <cstring>
#include <expected>

#include <openssl/rand.h>

#include "QuicConnection.h"
#include "QuicFrame.h"
#include "QuicPacketNumberSpace.h"
#include "QuicProtocol.h"

namespace fiber::quic {

namespace {

[[nodiscard]] bool ip_address_equal(const net::IpAddress &lhs, const net::IpAddress &rhs) noexcept {
    if (lhs.family() != rhs.family()) {
        return false;
    }
    if (lhs.is_v4()) {
        return lhs.v4_bytes() == rhs.v4_bytes();
    }
    return lhs.scope_id() == rhs.scope_id() && lhs.v6_bytes() == rhs.v6_bytes();
}

[[nodiscard]] bool socket_address_equal(const net::SocketAddress &lhs, const net::SocketAddress &rhs) noexcept {
    return lhs.port() == rhs.port() && ip_address_equal(lhs.ip(), rhs.ip());
}

[[nodiscard]] bool same_peer_ip(const net::SocketAddress &lhs, const net::SocketAddress &rhs) noexcept {
    return ip_address_equal(lhs.ip(), rhs.ip());
}

[[nodiscard]] QuicTime max_validation_delay(QuicTime pto) noexcept { return std::max(pto, QuicTime{1000}); }

} // namespace

QuicPathManager::QuicPathManager(QuicConnection &connection) noexcept : connection_(connection) {}

QuicPathManager::~QuicPathManager() {
    FIBER_ASSERT(!validation_timer_entry_.is_in_heap());
    for (QuicPath &path: paths_) {
        clear_frames(path);
    }
}

std::size_t QuicPathManager::count() const noexcept {
    std::size_t cnt = 0;
    for (const QuicPath &path: paths_) {
        if (path.allocated) {
            ++cnt;
        }
    }
    return cnt;
}

QuicPath *QuicPathManager::find(const net::SocketAddress &remote, const net::SocketAddress &local) noexcept {
    for (QuicPath &path: paths_) {
        if (path.allocated && socket_address_equal(path.remote, remote) && socket_address_equal(path.local, local)) {
            return &path;
        }
    }
    return nullptr;
}

const QuicPath *QuicPathManager::find(const net::SocketAddress &remote,
                                      const net::SocketAddress &local) const noexcept {
    for (const QuicPath &path: paths_) {
        if (path.allocated && socket_address_equal(path.remote, remote) && socket_address_equal(path.local, local)) {
            return &path;
        }
    }
    return nullptr;
}

QuicPath *QuicPathManager::find(QuicPathTag tag) noexcept {
    for (QuicPath &path: paths_) {
        if (path.allocated && path.tag == tag) {
            return &path;
        }
    }
    return nullptr;
}

const QuicPath *QuicPathManager::find(QuicPathTag tag) const noexcept {
    for (const QuicPath &path: paths_) {
        if (path.allocated && path.tag == tag) {
            return &path;
        }
    }
    return nullptr;
}

QuicPath *QuicPathManager::create(const net::SocketAddress &remote, const net::SocketAddress &local,
                                  const QuicConnectionId &remote_connection_id, QuicPathTag tag) noexcept {
    QuicPath *slot = nullptr;
    for (QuicPath &path: paths_) {
        if (!path.allocated) {
            slot = &path;
            break;
        }
    }
    if (slot == nullptr) {
        return nullptr;
    }

    *slot = QuicPath{};
    slot->allocated = true;
    slot->remote = remote;
    slot->local = local;
    slot->remote_connection_id = remote_connection_id;
    // Bind to the peer-CID pool slot whose cid bytes match. If the cid is not
    // (yet) in the pool, default to sequence 0 — the slot for the peer's
    // initial Source Connection ID — which always exists once handshake CIDs
    // are seeded. This keeps path-to-slot accounting consistent so that a
    // later retire_remote_connection_id can find and rebind the path.
    slot->remote_connection_id_sequence = 0;
    for (QuicRemoteConnectionIdSlot &cid_slot: connection_.remote_cids_) {
        if (!cid_slot.in_use) {
            continue;
        }
        if (cid_slot.cid.size() == remote_connection_id.size() &&
            (remote_connection_id.empty() ||
             std::memcmp(cid_slot.cid.data(), remote_connection_id.data(), remote_connection_id.size()) == 0)) {
            slot->remote_connection_id_sequence = cid_slot.sequence_number;
            cid_slot.used = true;
            break;
        }
    }
    slot->tag = tag;
    slot->seqnum = next_seqnum_++;
    slot->mtu = kQuicCongestionMinInitialSize;
    for (std::uint64_t &packet_number: slot->mtu_packet_numbers) {
        packet_number = kUnsetPacketNumber;
    }
    return slot;
}

void QuicPathManager::free(QuicPath &path) noexcept {
    clear_frames(path);
    if (&path == active_) {
        active_ = nullptr;
    }
    // Release the bound remote-CID slot so it can be reassigned to another
    // path or retired by the peer. A slot kept after the path is gone would
    // pin the CID against future rebinds. We only touch slots whose
    // sequence_number matches and that hold the same cid bytes — other paths
    // sharing the same sequence (transitional state during a retire) keep
    // their own binding via rebind_paths_to_cid.
    QuicRemoteConnectionIdSlot *bound = nullptr;
    for (QuicRemoteConnectionIdSlot &cid_slot: connection_.remote_cids_) {
        if (cid_slot.in_use && cid_slot.sequence_number == path.remote_connection_id_sequence) {
            bound = &cid_slot;
            break;
        }
    }
    if (bound != nullptr) {
        bool still_used = false;
        for (const QuicPath &other: paths_) {
            if (&other == &path || !other.allocated) {
                continue;
            }
            if (other.remote_connection_id_sequence == bound->sequence_number) {
                still_used = true;
                break;
            }
        }
        if (!still_used) {
            bound->used = false;
        }
    }
    path = QuicPath{};
}

QuicPath *QuicPathManager::find_path_by_remote_cid_sequence(std::uint64_t sequence) noexcept {
    for (QuicPath &path: paths_) {
        if (path.allocated && path.remote_connection_id_sequence == sequence) {
            return &path;
        }
    }
    return nullptr;
}

std::size_t QuicPathManager::rebind_paths_to_cid(std::uint64_t from_sequence,
                                                 const QuicRemoteConnectionIdSlot &to) noexcept {
    std::size_t rebound = 0;
    for (QuicPath &path: paths_) {
        if (!path.allocated || path.remote_connection_id_sequence != from_sequence) {
            continue;
        }
        path.remote_connection_id = to.cid;
        path.remote_connection_id_sequence = to.sequence_number;
        if (&path == active_) {
            connection_.options_.remote_connection_id = to.cid;
        }
        ++rebound;
    }
    return rebound;
}

bool QuicPathManager::set_active(QuicPath &path) noexcept {
    if (!path.allocated) {
        return false;
    }
    if (active_ != nullptr && active_ != &path && active_->allocated) {
        active_->tag = QuicPathTag::Backup;
    }
    path.tag = QuicPathTag::Active;
    active_ = &path;
    connection_.options_.remote_addr = path.remote;
    connection_.options_.local_addr = path.local;
    connection_.options_.remote_connection_id = path.remote_connection_id;
    return true;
}

void QuicPathManager::record_received(QuicPath &path, std::size_t len) noexcept {
    if (!path.allocated) {
        return;
    }
    path.used = true;
    path.received += len;
}

void QuicPathManager::record_sent(QuicPath &path, std::size_t len) noexcept {
    if (!path.allocated) {
        return;
    }
    path.sent += len;
}

bool QuicPathManager::has_send_work() const noexcept {
    for (const QuicPath &path: paths_) {
        if (path.allocated && !path.pending_frames.empty()) {
            return true;
        }
    }
    return false;
}

std::size_t QuicPathManager::send_limit(const QuicPath &path, std::size_t size) noexcept {
    if (path.validated) {
        return size;
    }

    const std::uint64_t max = path.received * 3;
    if (path.sent >= max) {
        return 0;
    }

    const std::uint64_t left = max - path.sent;
    if (static_cast<std::uint64_t>(size) > left) {
        return static_cast<std::size_t>(left);
    }
    return size;
}

void QuicPathManager::clear_frames(QuicPath &path) noexcept {
    auto &space = connection_.packet_number_space(QuicEncryptionLevel::Application);
    while (QuicOutputFrame *frame = path.pending_frames.pop_front()) {
        frame->path = nullptr;
        space.release_frame(*frame);
    }
}

void QuicPathManager::clear_frames(QuicPath &path, QuicFrameType type) noexcept {
    auto &space = connection_.packet_number_space(QuicEncryptionLevel::Application);
    QuicOutputFrame *prev = nullptr;
    QuicOutputFrame *frame = path.pending_frames.front();
    while (frame != nullptr) {
        QuicOutputFrame *next = path.pending_frames.next_of(*frame);
        if (frame->type == type) {
            path.pending_frames.erase_after(prev, *frame);
            frame->path = nullptr;
            space.release_frame(*frame);
        } else {
            prev = frame;
        }
        frame = next;
    }
}

common::IoResult<void> QuicPathManager::queue_path_challenge_frame(QuicPath &path,
                                                                   const std::uint8_t data[8]) noexcept {
    if (!connection_.can_queue_frame() || !path.allocated || data == nullptr || connection_.closing()) {
        return std::unexpected(common::IoErr::Invalid);
    }

    auto &space = connection_.packet_number_space(QuicEncryptionLevel::Application);
    QuicOutputFrame *frame = space.alloc_frame();
    if (frame == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }

    frame->type = QuicFrameType::PathChallenge;
    frame->path = &path;
    frame->min_packet_len = static_cast<std::uint16_t>(kMinInitialDatagramSize);
    std::memcpy(frame->u.path_challenge.data, data, sizeof(frame->u.path_challenge.data));
    path.pending_frames.push_back(*frame);
    connection_.schedule_send();
    return {};
}

common::IoResult<void> QuicPathManager::queue_path_response_frame(QuicPath &path, const std::uint8_t data[8]) noexcept {
    if (!connection_.can_queue_frame() || !path.allocated || data == nullptr || connection_.closing()) {
        return std::unexpected(common::IoErr::Invalid);
    }

    auto &space = connection_.packet_number_space(QuicEncryptionLevel::Application);
    QuicOutputFrame *frame = space.alloc_frame();
    if (frame == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }

    frame->type = QuicFrameType::PathResponse;
    frame->path = &path;
    frame->min_packet_len = static_cast<std::uint16_t>(kMinInitialDatagramSize);
    std::memcpy(frame->u.path_response.data, data, sizeof(frame->u.path_response.data));
    path.pending_frames.push_back(*frame);
    connection_.schedule_send();
    return {};
}

common::IoResult<void> QuicPathManager::recv_path_challenge_frame(QuicPath &path,
                                                                  const QuicPathChallengeFrame &frame) noexcept {
    auto queued = queue_path_response_frame(path, frame.data);
    if (!queued) {
        return std::unexpected(queued.error());
    }

    if (&path == active_) {
        auto ping = connection_.queue_ping_frame();
        if (!ping) {
            return std::unexpected(ping.error());
        }
    }
    return {};
}

common::IoResult<bool> QuicPathManager::recv_path_response_frame(const QuicPathChallengeFrame &frame,
                                                                 QuicTime now) noexcept {
    auto path = recv_path_response_frame_with_path(frame, now);
    if (!path) {
        return std::unexpected(path.error());
    }
    return *path != nullptr;
}

common::IoResult<QuicPath *> QuicPathManager::recv_path_response_frame_with_path(const QuicPathChallengeFrame &frame,
                                                                                 QuicTime now) noexcept {
    for (QuicPath &path: paths_) {
        if (!path.allocated || path.state != QuicPathState::Validating) {
            continue;
        }
        if (std::memcmp(path.challenge[0], frame.data, sizeof(frame.data)) != 0 &&
            std::memcmp(path.challenge[1], frame.data, sizeof(frame.data)) != 0) {
            continue;
        }

        clear_frames(path, QuicFrameType::PathChallenge);

        const bool active = &path == active_;
        if (active) {
            bool reset_congestion = true;
            if (QuicPath *backup = find(QuicPathTag::Backup)) {
                if (same_peer_ip(backup->remote, path.remote)) {
                    reset_congestion = false;
                    path.mtu = backup->mtu;
                    path.max_mtu = backup->max_mtu;
                }
            }
            if (reset_congestion) {
                connection_.reset_congestion_for_path(now);
            }
        }

        path.validated = true;
        path.mtu_unvalidated = false;
        path.state = QuicPathState::Idle;
        path.expires = QuicTime{0};
        path.tries = 0;
        if (active) {
            connection_.options_.remote_addr = path.remote;
            connection_.options_.local_addr = path.local;
            connection_.options_.remote_connection_id = path.remote_connection_id;
            auto discovered = discover_path_mtu(path, now);
            if (!discovered) {
                return std::unexpected(discovered.error());
            }
        }

        arm_validation_timer();
        return &path;
    }

    return nullptr;
}

common::IoResult<void> QuicPathManager::start_validation(QuicPath &path, QuicTime now) noexcept {
    if (!path.allocated || connection_.closing()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (path.state == QuicPathState::Validating) {
        return {};
    }

    path.tries = 0;
    if (RAND_bytes(reinterpret_cast<unsigned char *>(path.challenge), sizeof(path.challenge)) != 1) {
        return std::unexpected(common::IoErr::Invalid);
    }

    clear_frames(path, QuicFrameType::PathChallenge);
    auto first = queue_path_challenge_frame(path, path.challenge[0]);
    if (!first) {
        return std::unexpected(first.error());
    }
    auto second = queue_path_challenge_frame(path, path.challenge[1]);
    if (!second) {
        clear_frames(path, QuicFrameType::PathChallenge);
        return std::unexpected(second.error());
    }

    path.expires = now + validation_delay();
    path.state = QuicPathState::Validating;

    arm_validation_timer();
    return {};
}

common::IoResult<bool> QuicPathManager::expire_validation(QuicPath &path, QuicTime now) noexcept {
    if (!path.allocated || path.state != QuicPathState::Validating) {
        return false;
    }

    if (++path.tries < kQuicPathRetries) {
        clear_frames(path, QuicFrameType::PathChallenge);
        auto first = queue_path_challenge_frame(path, path.challenge[0]);
        if (!first) {
            return std::unexpected(first.error());
        }
        auto second = queue_path_challenge_frame(path, path.challenge[1]);
        if (!second) {
            return std::unexpected(second.error());
        }
        path.expires = now + validation_delay(path.tries);
        return true;
    }

    path.validated = false;
    path.state = QuicPathState::Idle;

    if (&path == active_) {
        QuicPath *backup = find(QuicPathTag::Backup);
        if (backup == nullptr) {
            connection_.close(QuicErrorCode::NoViablePath);
            return false;
        }

        if (!set_active(*backup)) {
            connection_.close(QuicErrorCode::NoViablePath);
            return false;
        }
    }

    free(path);
    return false;
}

common::IoResult<void> QuicPathManager::discover_path_mtu(QuicPath &path, QuicTime now) noexcept {
    if (!path.allocated || connection_.closing()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (!path.validated || !connection_.peer_transport().received) {
        return {};
    }

    const std::size_t peer_max = connection_.peer_transport().params.max_udp_payload_size;
    if (peer_max <= path.mtu) {
        path.mtud = path.mtu;
        path.max_mtu = path.mtu;
        path.state = QuicPathState::Idle;
        path.expires = QuicTime{0};
        return {};
    }

    if (path.max_mtu != 0) {
        if (path.max_mtu <= path.mtu || path.max_mtu - path.mtu <= kQuicPathMtuPrecision) {
            path.state = QuicPathState::Idle;
            path.expires = QuicTime{0};
            return {};
        }

        path.mtud = (path.mtu + path.max_mtu) / 2;
    } else {
        path.mtud = path.mtu * 2;
        if (path.mtud >= peer_max) {
            path.mtud = peer_max;
            path.max_mtu = peer_max;
        }
    }

    path.state = QuicPathState::WaitingMtuProbe;
    path.expires = now + kQuicPathMtuDelay;

    arm_validation_timer();
    return {};
}

common::IoResult<bool> QuicPathManager::queue_mtu_probe_frame(QuicPath &path) noexcept {
    if (!connection_.can_queue_frame() || !path.allocated || !path.validated || path.mtud <= path.mtu ||
        connection_.closing()) {
        return false;
    }

    auto &space = connection_.packet_number_space(QuicEncryptionLevel::Application);
    QuicOutputFrame *frame = space.alloc_frame();
    if (frame == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }

    frame->type = QuicFrameType::Ping;
    frame->path = &path;
    frame->min_packet_len = static_cast<std::uint16_t>(path.mtud);
    frame->ignore_loss = true;
    frame->ignore_congestion = true;
    frame->mtu_probe = true;
    path.pending_frames.push_back(*frame);
    connection_.schedule_send();
    return true;
}

common::IoResult<bool> QuicPathManager::expire_mtu_delay(QuicPath &path, QuicTime now) noexcept {
    if (!path.allocated || path.state != QuicPathState::WaitingMtuProbe) {
        return false;
    }

    path.tries = 0;
    for (std::uint64_t &packet_number: path.mtu_packet_numbers) {
        packet_number = kUnsetPacketNumber;
    }

    for (;;) {
        auto queued = queue_mtu_probe_frame(path);
        if (!queued) {
            return std::unexpected(queued.error());
        }
        if (*queued) {
            path.expires = now + validation_delay();
            path.state = QuicPathState::MtuDiscovery;
            return true;
        }

        path.max_mtu = path.mtud;
        if (path.max_mtu <= path.mtu || path.max_mtu - path.mtu <= kQuicPathMtuPrecision) {
            path.state = QuicPathState::Idle;
            path.expires = QuicTime{0};
            return false;
        }

        path.mtud = (path.mtu + path.max_mtu) / 2;
    }
}

common::IoResult<bool> QuicPathManager::expire_mtu_discovery(QuicPath &path, QuicTime now) noexcept {
    if (!path.allocated || path.state != QuicPathState::MtuDiscovery) {
        return false;
    }

    if (++path.tries < kQuicPathRetries) {
        auto queued = queue_mtu_probe_frame(path);
        if (!queued) {
            return std::unexpected(queued.error());
        }
        if (*queued) {
            path.expires = now + validation_delay(path.tries);
            return true;
        }
    }

    path.max_mtu = path.mtud;
    auto discovered = discover_path_mtu(path, now);
    if (!discovered) {
        return std::unexpected(discovered.error());
    }
    return true;
}

common::IoResult<bool> QuicPathManager::handle_mtu_ack(std::uint64_t min_packet_number, std::uint64_t max_packet_number,
                                                       QuicTime now) noexcept {
    bool handled = false;
    for (QuicPath &path: paths_) {
        if (!path.allocated || path.state != QuicPathState::MtuDiscovery) {
            continue;
        }

        for (std::uint64_t packet_number: path.mtu_packet_numbers) {
            if (packet_number == kUnsetPacketNumber || packet_number < min_packet_number ||
                packet_number > max_packet_number) {
                continue;
            }

            path.mtu = path.mtud;
            if (&path == active_) {
                connection_.congestion().mtu = path.mtu;
            }
            auto discovered = discover_path_mtu(path, now);
            if (!discovered) {
                return std::unexpected(discovered.error());
            }
            handled = true;
            break;
        }
    }
    return handled;
}

common::IoResult<bool> QuicPathManager::handle_mtu_probe_send_failed(QuicPath &path, QuicTime now) noexcept {
    if (!path.allocated || path.state != QuicPathState::MtuDiscovery) {
        return false;
    }

    clear_frames(path, QuicFrameType::Ping);
    path.max_mtu = path.mtud;
    auto discovered = discover_path_mtu(path, now);
    if (!discovered) {
        return std::unexpected(discovered.error());
    }
    return true;
}

common::IoResult<void> QuicPathManager::handle_migration(QuicPath &path, bool rebound, QuicTime now) noexcept {
    if (!path.allocated) {
        return std::unexpected(common::IoErr::Invalid);
    }

    QuicPath *old = active_;
    if (old == &path) {
        return {};
    }

    if (rebound && old != nullptr && old->validated) {
        auto validating_old = start_validation(*old, now);
        if (!validating_old) {
            return std::unexpected(validating_old.error());
        }
    }

    if (old != nullptr) {
        if (old->validated) {
            if (path.tag != QuicPathTag::Backup) {
                QuicPath *backup = find(QuicPathTag::Backup);
                if (backup != nullptr && backup != &path && backup != old) {
                    free(*backup);
                }
            }
            old->tag = QuicPathTag::Backup;
        } else {
            free(*old);
        }
    }

    if (!set_active(path)) {
        return std::unexpected(common::IoErr::Invalid);
    }

    if (!path.validated && path.state != QuicPathState::Validating) {
        auto validating = start_validation(path, now);
        if (!validating) {
            return std::unexpected(validating.error());
        }
    }

    return {};
}

void QuicPathManager::arm_validation_timer() noexcept {
    if (connection_.active_timer_loop() == nullptr) {
        return;
    }
    if (validation_timer_entry_.is_in_heap()) {
        connection_.loop_->cancel<QuicPathManager, &QuicPathManager::validation_timer_entry_>(*this);
    }
    if (connection_.closing()) {
        return;
    }

    const QuicTime now = quic_time_ms(connection_.loop_->now());
    bool found = false;
    QuicTime delay{0};
    for (const QuicPath &path: paths_) {
        if (!path.allocated || path.state == QuicPathState::Idle) {
            continue;
        }

        QuicTime left = path.expires - now;
        if (left < QuicTime{1}) {
            left = QuicTime{1};
        }
        if (!found || left < delay) {
            delay = left;
            found = true;
        }
    }

    if (!found) {
        return;
    }

    connection_.loop_->post_at<QuicPathManager, &QuicPathManager::validation_timer_entry_,
                               &QuicPathManager::on_validation_timer>(connection_.loop_->now() + delay, *this);
}

void QuicPathManager::cancel_validation_timer() noexcept {
    if (connection_.active_timer_loop() == nullptr) {
        return;
    }
    if (validation_timer_entry_.is_in_heap()) {
        connection_.loop_->cancel<QuicPathManager, &QuicPathManager::validation_timer_entry_>(*this);
    }
}

void QuicPathManager::cancel_validation_timer_quiesced() noexcept {
    FIBER_ASSERT(connection_.loop_ != nullptr);
    connection_.loop_->cancel_quiesced<QuicPathManager, &QuicPathManager::validation_timer_entry_>(*this);
}

void QuicPathManager::on_validation_timer(QuicPathManager *manager) noexcept {
    if (manager == nullptr) {
        return;
    }

    FIBER_ASSERT(manager->connection_.loop_ != nullptr && manager->connection_.loop_->in_loop());
    if (manager->connection_.loop_ == nullptr || !manager->connection_.loop_->in_loop() ||
        manager->connection_.closing()) {
        return;
    }

    const QuicTime now = quic_time_ms(manager->connection_.loop_->now());
    bool send_output = false;
    for (QuicPath &path: manager->paths_) {
        if (!path.allocated || path.state == QuicPathState::Idle || path.expires > now) {
            continue;
        }
        common::IoResult<bool> expired = false;
        switch (path.state) {
            case QuicPathState::Validating:
                expired = manager->expire_validation(path, now);
                break;
            case QuicPathState::WaitingMtuProbe:
                expired = manager->expire_mtu_delay(path, now);
                break;
            case QuicPathState::MtuDiscovery:
                expired = manager->expire_mtu_discovery(path, now);
                break;
            case QuicPathState::Idle:
                break;
        }
        if (!expired) {
            manager->connection_.close(QuicErrorCode::InternalError);
            return;
        }
        if (*expired) {
            send_output = true;
        }
    }

    if (send_output) {
        manager->connection_.schedule_send();
    }

    manager->arm_validation_timer();
}

QuicTime QuicPathManager::validation_delay() const noexcept {
    return max_validation_delay(quic_pto(connection_.rtt(), connection_.peer_transport().params.max_ack_delay, true,
                                         connection_.state() == QuicConnectionState::Established));
}

QuicTime QuicPathManager::validation_delay(std::uint32_t tries) const noexcept {
    QuicTime delay = validation_delay();
    while (tries-- != 0) {
        if (delay > QuicTime::max() / 2) {
            return QuicTime::max();
        }
        delay *= 2;
    }
    return delay;
}

} // namespace fiber::quic
