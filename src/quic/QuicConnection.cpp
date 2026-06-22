#include "QuicConnection.h"

#include <algorithm>
#include <cstring>
#include <expected>
#include <limits>
#include <new>

#include "QuicCrypto.h"
#include "QuicProtocol.h"
#include "QuicTransportParamsCodec.h"

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

QuicConnection::QuicConnection(const Options &options) noexcept :
    options_(options), next_local_bidi_stream_id_(initial_stream_id(options.role, QuicStreamType::Bidirectional)),
    next_local_uni_stream_id_(initial_stream_id(options.role, QuicStreamType::Unidirectional)) {
    options_.transport.initial_max_data = options_.recv_flow.conn_recv_limit;
    options_.transport.initial_max_stream_data_bidi_local = options_.recv_flow.stream_buffer_limit;
    options_.transport.initial_max_stream_data_bidi_remote = options_.recv_flow.stream_buffer_limit;
    options_.transport.initial_max_stream_data_uni = options_.recv_flow.stream_buffer_limit;
    recv_data_limit_ = options_.recv_flow.conn_recv_limit;
    QuicOutputFramePool &frame_pool =
            options_.output_frame_pool != nullptr ? *options_.output_frame_pool : output_frame_pool_;
    packet_number_spaces_[0].reset(QuicEncryptionLevel::Initial);
    packet_number_spaces_[0].set_frame_pool(frame_pool);
    packet_number_spaces_[0].crypto_recv.init(recv_extent_pool_);
    packet_number_spaces_[1].reset(QuicEncryptionLevel::Handshake);
    packet_number_spaces_[1].set_frame_pool(frame_pool);
    packet_number_spaces_[1].crypto_recv.init(recv_extent_pool_);
    packet_number_spaces_[2].reset(QuicEncryptionLevel::Application);
    packet_number_spaces_[2].set_frame_pool(frame_pool);
    packet_number_spaces_[2].crypto_recv.init(recv_extent_pool_);
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

    std::uint64_t &next = is_bidirectional_stream(stream_id) ? next_peer_bidi_sequence_ : next_peer_uni_sequence_;
    next = std::max(next, stream_sequence(stream_id) + 1);
    return {};
}

QuicStream *QuicConnection::find_stream(std::uint64_t stream_id) noexcept { return streams_.find(stream_id); }

const QuicStream *QuicConnection::find_stream(std::uint64_t stream_id) const noexcept {
    return streams_.find(stream_id);
}

common::IoResult<QuicStream *> QuicConnection::get_or_create_peer_stream(std::uint64_t stream_id) noexcept {
    if (closing()) {
        return std::unexpected(common::IoErr::Canceled);
    }
    if (QuicStream *stream = streams_.find(stream_id)) {
        return stream;
    }
    auto recorded = record_peer_stream_id(stream_id);
    if (!recorded) {
        return std::unexpected(recorded.error());
    }

    if (options_.ops.on_new_stream == nullptr) {
        return std::unexpected(common::IoErr::NotSupported);
    }

    QuicNewStreamContext ctx{
            .stream_id = stream_id,
            .connection = *this,
            .recv_extent_pool = recv_extent_pool_,
            .recv_options =
                    {
                            .buffer_limit = options_.recv_flow.stream_buffer_limit,
                            .low_water = options_.recv_flow.stream_low_water,
                            .max_stream_data = options_.recv_flow.stream_buffer_limit,
                    },
    };
    QuicStream::Lease lease = options_.ops.on_new_stream(options_.owner, ctx);
    if (!lease || lease->stream_id() != stream_id || lease->attached_to_connection()) {
        return std::unexpected(lease ? common::IoErr::Invalid : common::IoErr::NoMem);
    }

    QuicStream *stream = lease.get();
    stream->attach_to_connection(*this);

    if (!streams_.insert(std::move(lease))) {
        stream->detach_from_connection();
        return std::unexpected(common::IoErr::NoMem);
    }
    return stream;
}

common::IoResult<void> QuicConnection::recv_stream_frame(const QuicStreamFrame &frame, QuicSlice data) noexcept {
    if (find_stream(frame.stream_id) == nullptr && is_gone_peer_stream(frame.stream_id)) {
        return {};
    }

    auto stream = get_or_create_peer_stream(frame.stream_id);
    if (!stream) {
        return std::unexpected(stream.error());
    }

    if (frame.offset > std::numeric_limits<std::uint64_t>::max() - data.len) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const std::uint64_t old_end = (*stream)->recv_queue_.received_end_offset();
    const std::uint64_t frame_end = frame.offset + data.len;
    const std::uint64_t pending_delta = frame_end > old_end ? frame_end - old_end : 0;
    auto allowed = check_recv_data_delta(pending_delta);
    if (!allowed) {
        return std::unexpected(allowed.error());
    }

    auto received = (*stream)->on_stream_data_recv(data.data, data.len, frame.offset, frame.fin);
    if (!received) {
        return std::unexpected(received.error());
    }
    commit_recv_data_delta(*received);
    maybe_extend_recv_data_flow_control();
    try_release_stream(**stream);
    return {};
}

common::IoResult<void> QuicConnection::recv_reset_stream_frame(const QuicResetStreamFrame &frame) noexcept {
    if (find_stream(frame.id) == nullptr && is_gone_peer_stream(frame.id)) {
        return {};
    }

    auto stream = get_or_create_peer_stream(frame.id);
    if (!stream) {
        return std::unexpected(stream.error());
    }

    const std::uint64_t old_end = (*stream)->recv_queue_.received_end_offset();
    if (frame.final_size < old_end) {
        return std::unexpected(common::IoErr::Invalid);
    }
    auto allowed = check_recv_data_delta(frame.final_size - old_end);
    if (!allowed) {
        return std::unexpected(allowed.error());
    }

    auto reset = (*stream)->on_remote_reset(frame.error_code, frame.final_size);
    if (!reset) {
        return std::unexpected(reset.error());
    }
    commit_recv_data_delta(*reset);
    maybe_extend_recv_data_flow_control();
    try_release_stream(**stream);
    return {};
}

common::IoResult<void> QuicConnection::recv_stop_sending_frame(const QuicStopSendingFrame &frame) noexcept {
    QuicStream *stream = find_stream(frame.id);
    if (stream == nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return stream->on_remote_stop_sending(frame.error_code);
}

common::IoResult<void> QuicConnection::recv_max_stream_data_frame(const QuicMaxStreamDataFrame &frame) noexcept {
    QuicStream *stream = find_stream(frame.id);
    if (stream == nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }
    stream->on_max_stream_data(frame.limit);
    return {};
}

common::IoResult<void> QuicConnection::recv_max_data_frame(const QuicMaxDataFrame &frame) noexcept {
    if (frame.max_data > peer_max_data_) {
        peer_max_data_ = frame.max_data;
        notify_stream_write_waiters();
    }
    return {};
}

void QuicConnection::release_stream_app(QuicStream &stream) noexcept {
    stream.mark_app_released();
    try_release_stream(stream);
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

common::IoResult<void> QuicConnection::apply_peer_transport_params(const QuicTransportParams &params) noexcept {
    if (peer_transport_.received) {
        return std::unexpected(common::IoErr::Already);
    }
    if (!params.has_initial_source_connection_id) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (params.initial_source_connection_id.size() != options_.remote_connection_id.size() ||
        (params.initial_source_connection_id.size() != 0 &&
         std::memcmp(params.initial_source_connection_id.data(), options_.remote_connection_id.data(),
                     params.initial_source_connection_id.size()) != 0)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (params.max_udp_payload_size < kMinInitialDatagramSize || params.max_udp_payload_size > kQuicMaxUdpPayloadSize ||
        params.active_connection_id_limit < 2 || params.ack_delay_exponent > 20 || params.max_ack_delay >= 16384) {
        return std::unexpected(common::IoErr::Invalid);
    }

    QuicTransportSettings applied{};
    applied.max_idle_timeout = std::chrono::milliseconds(params.max_idle_timeout);
    applied.max_udp_payload_size = static_cast<std::size_t>(params.max_udp_payload_size);
    applied.initial_max_data = params.initial_max_data;
    applied.initial_max_stream_data_bidi_local = params.initial_max_stream_data_bidi_local;
    applied.initial_max_stream_data_bidi_remote = params.initial_max_stream_data_bidi_remote;
    applied.initial_max_stream_data_uni = params.initial_max_stream_data_uni;
    applied.initial_max_streams_bidi = params.initial_max_streams_bidi;
    applied.initial_max_streams_uni = params.initial_max_streams_uni;
    applied.ack_delay_exponent = params.ack_delay_exponent;
    applied.max_ack_delay = std::chrono::milliseconds(params.max_ack_delay);
    applied.active_connection_id_limit = params.active_connection_id_limit;
    applied.disable_active_migration = params.disable_active_migration;

    peer_transport_.params = applied;
    peer_transport_.received = true;
    peer_max_data_ = params.initial_max_data;
    options_.max_local_bidirectional_streams = params.initial_max_streams_bidi;
    options_.max_local_unidirectional_streams = params.initial_max_streams_uni;
    streams_.for_each([this](QuicStream &stream) noexcept {
        stream.on_max_stream_data(initial_stream_send_limit(stream.stream_id()));
    });
    notify_stream_write_waiters();
    if (params.max_idle_timeout > 0 &&
        std::chrono::milliseconds(params.max_idle_timeout) < options_.transport.max_idle_timeout) {
        options_.transport.max_idle_timeout = std::chrono::milliseconds(params.max_idle_timeout);
    }
    return {};
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

bool QuicConnection::is_gone_peer_stream(std::uint64_t stream_id) const noexcept {
    if (!is_peer_stream(stream_id)) {
        return false;
    }

    const std::uint64_t next = is_bidirectional_stream(stream_id) ? next_peer_bidi_sequence_ : next_peer_uni_sequence_;
    return stream_sequence(stream_id) < next;
}

void QuicConnection::retire_stream(QuicStream &stream) noexcept {
    if (!stream.attached_to_connection()) {
        return;
    }

    QuicStream::Lease lease = streams_.erase(stream.stream_id());
    if (!lease) {
        stream.detach_from_connection();
        return;
    }
    lease->detach_from_connection();
}

void QuicConnection::try_release_stream(QuicStream &stream) noexcept {
    if (stream.ready_for_connection_release()) {
        retire_stream(stream);
    }
}

void QuicConnection::drop_stream_send_ticket(std::uint64_t stream_id) noexcept {
    QuicStream *stream = find_stream(stream_id);
    if (stream == nullptr) {
        return;
    }
    stream->stream_send_pending_ = false;
    try_release_stream(*stream);
}

common::IoResult<void> QuicConnection::queue_stream_frame(QuicStream &stream) noexcept {
    if (closing() || !stream.attached_to_connection() || stream.stream_send_pending_ || !stream.has_send_work()) {
        return {};
    }

    auto &space = packet_number_space(QuicEncryptionLevel::Application);
    QuicOutputFrame *frame = space.alloc_frame();
    if (frame == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }

    frame->type = QuicFrameType::Stream;
    frame->u.stream.stream_id = stream.stream_id();
    stream.stream_send_pending_ = true;
    space.pending_frames.push_back(*frame);
    schedule_send();
    return {};
}

void QuicConnection::schedule_send() noexcept {
    if (options_.schedule_send != nullptr) {
        options_.schedule_send(options_.schedule_send_owner, *this);
    }
}

common::IoResult<void> QuicConnection::on_stream_send_acked(std::uint64_t stream_id, std::size_t offset,
                                                            std::size_t length, bool fin) noexcept {
    QuicStream *stream = find_stream(stream_id);
    if (stream == nullptr) {
        return {};
    }
    auto acked = stream->mark_send_acked(offset, length, fin);
    if (!acked) {
        return std::unexpected(acked.error());
    }
    try_release_stream(*stream);
    return {};
}

common::IoResult<void> QuicConnection::on_stream_send_failed(std::uint64_t stream_id, std::size_t offset,
                                                             std::size_t length, bool fin) noexcept {
    QuicStream *stream = find_stream(stream_id);
    if (stream == nullptr) {
        return {};
    }
    auto failed = stream->mark_send_failed(offset, length, fin);
    if (!failed) {
        return std::unexpected(failed.error());
    }
    return queue_stream_frame(*stream);
}

common::IoResult<void> QuicConnection::queue_reset_stream_frame(std::uint64_t stream_id, std::uint64_t error_code,
                                                                std::uint64_t final_size) noexcept {
    auto &space = packet_number_space(QuicEncryptionLevel::Application);
    QuicOutputFrame *frame = space.alloc_frame();
    if (frame == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    frame->type = QuicFrameType::ResetStream;
    frame->u.reset_stream.id = stream_id;
    frame->u.reset_stream.error_code = error_code;
    frame->u.reset_stream.final_size = final_size;
    space.pending_frames.push_back(*frame);
    schedule_send();
    return {};
}

common::IoResult<void> QuicConnection::queue_stop_sending_frame(std::uint64_t stream_id,
                                                                std::uint64_t error_code) noexcept {
    auto &space = packet_number_space(QuicEncryptionLevel::Application);
    QuicOutputFrame *frame = space.alloc_frame();
    if (frame == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    frame->type = QuicFrameType::StopSending;
    frame->u.stop_sending.id = stream_id;
    frame->u.stop_sending.error_code = error_code;
    space.pending_frames.push_back(*frame);
    schedule_send();
    return {};
}

common::IoResult<void> QuicConnection::queue_max_stream_data_frame(std::uint64_t stream_id,
                                                                   std::uint64_t limit) noexcept {
    auto &space = packet_number_space(QuicEncryptionLevel::Application);
    QuicOutputFrame *frame = space.alloc_frame();
    if (frame == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    frame->type = QuicFrameType::MaxStreamData;
    frame->u.max_stream_data.id = stream_id;
    frame->u.max_stream_data.limit = limit;
    space.pending_frames.push_back(*frame);
    schedule_send();
    return {};
}

common::IoResult<void> QuicConnection::queue_max_data_frame(std::uint64_t limit) noexcept {
    auto &space = packet_number_space(QuicEncryptionLevel::Application);
    QuicOutputFrame *frame = space.alloc_frame();
    if (frame == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    frame->type = QuicFrameType::MaxData;
    frame->u.max_data.max_data = limit;
    space.pending_frames.push_back(*frame);
    schedule_send();
    return {};
}

std::uint64_t QuicConnection::peer_data_available() const noexcept {
    if (peer_data_reserved_ >= peer_max_data_) {
        return 0;
    }
    return peer_max_data_ - peer_data_reserved_;
}

bool QuicConnection::reserve_peer_data(std::uint64_t bytes) noexcept {
    if (bytes == 0) {
        return true;
    }
    if (bytes > peer_data_available()) {
        return false;
    }
    peer_data_reserved_ += bytes;
    return true;
}

std::uint64_t QuicConnection::initial_stream_send_limit(std::uint64_t stream_id) const noexcept {
    if (!peer_transport_.received) {
        return 0;
    }
    const QuicStreamType type = stream_type(stream_id);
    if (type == QuicStreamType::Unidirectional) {
        return is_local_stream(stream_id) ? peer_transport_.params.initial_max_stream_data_uni : 0;
    }
    return is_local_stream(stream_id) ? peer_transport_.params.initial_max_stream_data_bidi_remote
                                      : peer_transport_.params.initial_max_stream_data_bidi_local;
}

void QuicConnection::notify_stream_write_waiters() noexcept {
    streams_.for_each([](QuicStream &stream) noexcept { stream.on_connection_max_data(); });
}

common::IoResult<void> QuicConnection::check_recv_data_delta(std::uint64_t delta) const noexcept {
    if (delta > recv_data_limit_ || recv_data_consumed_ > recv_data_limit_ - delta) {
        return std::unexpected(common::IoErr::MessageTooLarge);
    }
    return {};
}

void QuicConnection::commit_recv_data_delta(std::uint64_t delta) noexcept { recv_data_consumed_ += delta; }

void QuicConnection::maybe_extend_recv_data_flow_control() noexcept {
    const std::uint64_t window = options_.recv_flow.conn_recv_limit;
    if (window == 0) {
        return;
    }

    const std::uint64_t remaining = recv_data_limit_ > recv_data_consumed_ ? recv_data_limit_ - recv_data_consumed_ : 0;
    if (remaining >= options_.recv_flow.conn_recv_low_water) {
        return;
    }

    std::uint64_t limit = kMaxVarint;
    if (recv_data_limit_ <= kMaxVarint && window <= kMaxVarint - recv_data_limit_) {
        limit = recv_data_limit_ + window;
    }
    if (limit <= recv_data_limit_) {
        return;
    }

    auto queued = queue_max_data_frame(limit);
    if (queued) {
        recv_data_limit_ = limit;
    }
}

} // namespace fiber::quic
