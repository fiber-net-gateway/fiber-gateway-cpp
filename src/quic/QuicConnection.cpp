#include "QuicConnection.h"

#include <algorithm>
#include <cstring>

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

void QuicPacketNumberSpace::reset(QuicEncryptionLevel space_level) noexcept {
    level = space_level;
    crypto_sent = 0;
    next_packet_number = 0;
    largest_acked_packet_number = kUnsetPacketNumber;
    largest_received_packet_number = kUnsetPacketNumber;
    pending_ack = kUnsetPacketNumber;
    largest_range = kUnsetPacketNumber;
    first_range = kUnsetPacketNumber;
    largest_received_time = std::chrono::milliseconds{0};
    ack_delay_start = std::chrono::milliseconds{0};
    ack_range_count = 0;
    ack_ranges = {};
    send_ack = false;
}

void QuicPacketNumberSpace::record_received_packet_number(std::uint64_t packet_number) noexcept {
    if (largest_received_packet_number == kUnsetPacketNumber || packet_number > largest_received_packet_number) {
        largest_received_packet_number = packet_number;
    }
    pending_ack = packet_number;
    send_ack = true;
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
