#include "QuicConnection.h"

#include <algorithm>
#include <cstring>
#include <new>

#include "QuicCrypto.h"
#include "QuicProtocol.h"

namespace fiber::quic {

namespace {

constexpr std::uint64_t kStreamTypeMask = 0x02;
constexpr std::uint64_t kStreamInitiatorMask = 0x01;
constexpr std::uint64_t kStreamIncrement = 4;

[[nodiscard]] std::uint64_t stream_sequence(std::uint64_t stream_id) noexcept { return stream_id >> 2; }

[[nodiscard]] std::uint64_t initial_stream_id(QuicConnectionRole role, QuicStreamType type) noexcept {
    std::uint64_t id = role == QuicConnectionRole::Server ? 1 : 0;
    if (type == QuicStreamType::Unidirectional) {
        id |= kStreamTypeMask;
    }
    return id;
}

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

} // namespace

common::IoResult<QuicConnectionId> QuicConnectionId::from_bytes(const std::uint8_t *data, std::size_t len) noexcept {
    if (len > kMaxConnectionIdLength || (len > 0 && data == nullptr)) {
        return std::unexpected(common::IoErr::Invalid);
    }

    QuicConnectionId out{};
    out.length = static_cast<std::uint8_t>(len);
    if (len > 0) {
        std::memcpy(out.bytes.data(), data, len);
    }
    return out;
}

QuicPacketProtectionKeys::QuicPacketProtectionKeys() noexcept { EVP_AEAD_CTX_zero(&aead); }

QuicPacketProtectionKeys::~QuicPacketProtectionKeys() { reset(); }

void QuicPacketProtectionKeys::reset() noexcept {
    if (aead_initialized) {
        EVP_AEAD_CTX_cleanup(&aead);
        aead_initialized = false;
    }
    EVP_AEAD_CTX_zero(&aead);
    key = {};
    iv = {};
    hp = {};
    secret = {};
    secret_len = 0;
    key_len = 0;
    iv_len = 0;
    hp_len = 0;
    hp_key = {};
    hp_chacha20 = false;
    ready = false;
}

void QuicCryptoState::reset() noexcept {
    initial_read.reset();
    initial_write.reset();
    early_read.reset();
    early_write.reset();
    handshake_read.reset();
    handshake_write.reset();
    application_read.reset();
    application_write.reset();
    initial_ready = false;
}

QuicPacketNumberSpace::QuicPacketNumberSpace() noexcept { reset(QuicEncryptionLevel::Initial); }

QuicPacketNumberSpace::~QuicPacketNumberSpace() {
    auto delete_owned = [](QuicFrameQueue &queue) noexcept {
        QuicFrame *frame = queue.front();
        while (frame != nullptr) {
            QuicFrame *next = queue.next_of(*frame);
            queue.erase(*frame);
            if (frame->connection_owned) {
                delete frame;
            }
            frame = next;
        }
    };

    delete_owned(pending_frames);
    delete_owned(sending_frames);
    delete_owned(sent_frames);
    delete_owned(free_frames);
}

void QuicPacketNumberSpace::reset(QuicEncryptionLevel space_level) noexcept {
    level = space_level;
    crypto_sent = 0;
    next_packet_number = 0;
    largest_acked_packet_number = kUnsetPacketNumber;
    largest_received_packet_number = kUnsetPacketNumber;
    ack_frame = QuicFrame{};
    pending_ack = kUnsetPacketNumber;
    largest_range = kUnsetPacketNumber;
    first_range = kUnsetPacketNumber;
    largest_received_time = std::chrono::milliseconds{0};
    ack_delay_start = std::chrono::milliseconds{0};
    ack_range_count = 0;
    ack_ranges = {};
    send_ack_count = 0;
    send_ack = false;
}

QuicFrame *QuicPacketNumberSpace::alloc_frame() noexcept {
    QuicFrame *frame = free_frames.front();
    if (frame != nullptr) {
        free_frames.erase(*frame);
        *frame = QuicFrame{};
        frame->connection_owned = true;
        return frame;
    }

    frame = new (std::nothrow) QuicFrame{};
    if (frame != nullptr) {
        frame->connection_owned = true;
    }
    return frame;
}

void QuicPacketNumberSpace::release_frame(QuicFrame &frame) noexcept {
    if (&frame == &ack_frame || !frame.connection_owned) {
        return;
    }
    if (frame.queue_hook.linked()) {
        return;
    }

    frame = QuicFrame{};
    frame.connection_owned = true;
    free_frames.push_front(frame);
}

void QuicPacketNumberSpace::record_received_packet_number(std::uint64_t packet_number) noexcept {
    if (largest_received_packet_number == kUnsetPacketNumber || packet_number > largest_received_packet_number) {
        largest_received_packet_number = packet_number;
    }
}

void QuicPacketNumberSpace::record_acked_packet_number(std::uint64_t packet_number) noexcept {
    if (largest_acked_packet_number == kUnsetPacketNumber || packet_number > largest_acked_packet_number) {
        largest_acked_packet_number = packet_number;
    }
}

QuicConnection::QuicConnection(const Options &options) noexcept :
    options_(options), next_local_bidi_stream_id_(initial_stream_id(options.role, QuicStreamType::Bidirectional)),
    next_local_uni_stream_id_(initial_stream_id(options.role, QuicStreamType::Unidirectional)) {
    packet_number_spaces_[0].reset(QuicEncryptionLevel::Initial);
    packet_number_spaces_[1].reset(QuicEncryptionLevel::Handshake);
    packet_number_spaces_[2].reset(QuicEncryptionLevel::Application);
    quic_congestion_init(congestion_, QuicTime{0});
    quic_rtt_init(rtt_);

    active_path_ =
            create_path(options_.remote_addr, options_.local_addr, options_.remote_connection_id, QuicPathTag::Active);
}

common::IoResult<void> QuicConnection::start_handshake() noexcept {
    if (state_ != QuicConnectionState::Init) {
        return std::unexpected(common::IoErr::Already);
    }
    state_ = QuicConnectionState::Handshaking;
    return {};
}

common::IoResult<void> QuicConnection::mark_established() noexcept {
    if (closing()) {
        return std::unexpected(common::IoErr::Canceled);
    }
    if (state_ != QuicConnectionState::Init && state_ != QuicConnectionState::Handshaking) {
        return std::unexpected(common::IoErr::Already);
    }
    state_ = QuicConnectionState::Established;
    return {};
}

void QuicConnection::begin_draining(QuicErrorCode error) noexcept {
    if (state_ == QuicConnectionState::Closed) {
        return;
    }
    close_error_ = error;
    state_ = QuicConnectionState::Draining;
}

void QuicConnection::close(QuicErrorCode error) noexcept {
    if (state_ == QuicConnectionState::Closed) {
        return;
    }
    close_error_ = error;
    state_ = QuicConnectionState::Closing;
}

void QuicConnection::mark_closed() noexcept { state_ = QuicConnectionState::Closed; }

common::IoResult<std::uint64_t> QuicConnection::next_local_stream_id(QuicStreamType type) noexcept {
    if (closing()) {
        return std::unexpected(common::IoErr::Canceled);
    }

    std::uint64_t &next =
            type == QuicStreamType::Bidirectional ? next_local_bidi_stream_id_ : next_local_uni_stream_id_;
    if (stream_sequence(next) >= local_stream_limit(type)) {
        return std::unexpected(common::IoErr::Busy);
    }

    const std::uint64_t id = next;
    next += kStreamIncrement;
    return id;
}

bool QuicConnection::can_accept_peer_stream(std::uint64_t stream_id) const noexcept {
    if (!is_peer_stream(stream_id)) {
        return false;
    }
    const QuicStreamType type = stream_type(stream_id);
    return stream_sequence(stream_id) < peer_stream_limit(type);
}

common::IoResult<void> QuicConnection::record_peer_stream_id(std::uint64_t stream_id) noexcept {
    if (!can_accept_peer_stream(stream_id)) {
        return std::unexpected(common::IoErr::Busy);
    }

    std::uint64_t &largest =
            is_bidirectional_stream(stream_id) ? largest_peer_bidi_sequence_ : largest_peer_uni_sequence_;
    largest = std::max(largest, stream_sequence(stream_id));
    return {};
}

bool QuicConnection::is_local_stream(std::uint64_t stream_id) const noexcept {
    return (stream_id & kStreamInitiatorMask) == local_initiator_bit();
}

bool QuicConnection::is_bidirectional_stream(std::uint64_t stream_id) noexcept {
    return (stream_id & kStreamTypeMask) == 0;
}

bool QuicConnection::is_unidirectional_stream(std::uint64_t stream_id) noexcept {
    return !is_bidirectional_stream(stream_id);
}

QuicStreamType QuicConnection::stream_type(std::uint64_t stream_id) noexcept {
    return is_bidirectional_stream(stream_id) ? QuicStreamType::Bidirectional : QuicStreamType::Unidirectional;
}

QuicPacketNumberSpace &QuicConnection::packet_number_space(QuicEncryptionLevel level) noexcept {
    return packet_number_spaces_[packet_number_space_index(level)];
}

const QuicPacketNumberSpace &QuicConnection::packet_number_space(QuicEncryptionLevel level) const noexcept {
    return packet_number_spaces_[packet_number_space_index(level)];
}

common::IoResult<void> QuicConnection::init_initial_crypto(const QuicConnectionId &original_dcid) noexcept {
    return quic_init_initial_crypto(crypto_, options_.role, original_dcid);
}

void QuicConnection::reset_congestion_for_path(QuicTime now) noexcept {
    auto &space = packet_number_space(QuicEncryptionLevel::Application);
    reset_packet_number_ = space.next_packet_number;
    quic_congestion_reset_for_path(congestion_, rtt_, now);
}

std::size_t QuicConnection::path_count() const noexcept {
    std::size_t count = 0;
    for (const QuicPath &path: paths_) {
        if (path.allocated) {
            ++count;
        }
    }
    return count;
}

QuicPath *QuicConnection::find_path(const net::SocketAddress &remote, const net::SocketAddress &local) noexcept {
    for (QuicPath &path: paths_) {
        if (path.allocated && socket_address_equal(path.remote, remote) && socket_address_equal(path.local, local)) {
            return &path;
        }
    }
    return nullptr;
}

const QuicPath *QuicConnection::find_path(const net::SocketAddress &remote,
                                          const net::SocketAddress &local) const noexcept {
    for (const QuicPath &path: paths_) {
        if (path.allocated && socket_address_equal(path.remote, remote) && socket_address_equal(path.local, local)) {
            return &path;
        }
    }
    return nullptr;
}

QuicPath *QuicConnection::find_path(QuicPathTag tag) noexcept {
    for (QuicPath &path: paths_) {
        if (path.allocated && path.tag == tag) {
            return &path;
        }
    }
    return nullptr;
}

const QuicPath *QuicConnection::find_path(QuicPathTag tag) const noexcept {
    for (const QuicPath &path: paths_) {
        if (path.allocated && path.tag == tag) {
            return &path;
        }
    }
    return nullptr;
}

QuicPath *QuicConnection::create_path(const net::SocketAddress &remote, const net::SocketAddress &local,
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
    slot->tag = tag;
    slot->seqnum = next_path_seqnum_++;
    slot->mtu = kQuicCongestionMinInitialSize;
    for (std::uint64_t &packet_number: slot->mtu_packet_numbers) {
        packet_number = kUnsetPacketNumber;
    }
    return slot;
}

void QuicConnection::free_path(QuicPath &path) noexcept {
    if (&path == active_path_) {
        active_path_ = nullptr;
    }
    path = QuicPath{};
}

bool QuicConnection::set_active_path(QuicPath &path) noexcept {
    if (!path.allocated) {
        return false;
    }
    if (active_path_ != nullptr && active_path_ != &path && active_path_->allocated) {
        active_path_->tag = QuicPathTag::Backup;
    }
    path.tag = QuicPathTag::Active;
    active_path_ = &path;
    options_.remote_addr = path.remote;
    options_.local_addr = path.local;
    options_.remote_connection_id = path.remote_connection_id;
    return true;
}

void QuicConnection::record_path_received(QuicPath &path, std::size_t len) noexcept {
    if (!path.allocated) {
        return;
    }
    path.used = true;
    path.received += len;
}

void QuicConnection::record_path_sent(QuicPath &path, std::size_t len) noexcept {
    if (!path.allocated) {
        return;
    }
    path.sent += len;
}

std::size_t QuicConnection::path_send_limit(const QuicPath &path, std::size_t size) noexcept {
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

std::size_t QuicConnection::packet_number_space_index(QuicEncryptionLevel level) noexcept {
    switch (level) {
        case QuicEncryptionLevel::Initial:
            return 0;
        case QuicEncryptionLevel::Handshake:
            return 1;
        case QuicEncryptionLevel::EarlyData:
        case QuicEncryptionLevel::Application:
            return 2;
    }
    return 2;
}

std::uint8_t QuicConnection::local_initiator_bit() const noexcept {
    return options_.role == QuicConnectionRole::Server ? 1 : 0;
}

std::uint64_t QuicConnection::local_stream_limit(QuicStreamType type) const noexcept {
    return type == QuicStreamType::Bidirectional ? options_.max_local_bidirectional_streams
                                                 : options_.max_local_unidirectional_streams;
}

std::uint64_t QuicConnection::peer_stream_limit(QuicStreamType type) const noexcept {
    return type == QuicStreamType::Bidirectional ? options_.max_peer_bidirectional_streams
                                                 : options_.max_peer_unidirectional_streams;
}

} // namespace fiber::quic
