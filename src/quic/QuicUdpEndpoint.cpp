#include "QuicUdpEndpoint.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <expected>
#include <new>

#include <openssl/rand.h>

#include "../async/Spawn.h"
#include "QuicCongestion.h"
#include "QuicCrypto.h"
#include "QuicPacketCodec.h"
#include "QuicTransportCodec.h"

namespace fiber::quic {

namespace {

inline constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
inline constexpr std::size_t kQuicStatelessResponseBufferSize = 1500;
inline constexpr const char kInvalidAddressValidationTokenReason[] = "invalid address validation token";
inline constexpr const char kExpiredAddressValidationTokenReason[] = "expired address validation token";
inline constexpr QuicEncryptionLevel kSendLevels[] = {QuicEncryptionLevel::Initial, QuicEncryptionLevel::Handshake,
                                                      QuicEncryptionLevel::Application};

[[nodiscard]] common::IoResult<std::size_t> encode_retry_packet(const QuicPacketHeader &packet,
                                                                const QuicConnectionId &retry_scid,
                                                                const QuicAddressToken &token,
                                                                QuicWriteCursor &out) noexcept {
    QuicRetryPacketSpec spec{};
    spec.version = packet.version;
    spec.original_dcid = packet.dcid;
    spec.dcid = packet.scid;
    spec.scid = retry_scid;
    spec.token = token.slice();
    return quic_create_retry_packet(spec, out);
}

[[nodiscard]] common::IoResult<std::size_t> encode_invalid_token_close_packet(const QuicPacketHeader &packet,
                                                                              const QuicReceivedDatagram &datagram,
                                                                              const char *reason, std::uint8_t *out,
                                                                              std::size_t out_cap) noexcept {
    if (reason == nullptr || out == nullptr || out_cap == 0) {
        return std::unexpected(common::IoErr::Invalid);
    }

    QuicOutputFrame frame{};
    frame.type = QuicFrameType::ConnectionClose;
    frame.u.close.error_code = static_cast<std::uint64_t>(QuicErrorCode::InvalidToken);
    frame.u.close.frame_type = 0;
    frame.u.close.reason = reinterpret_cast<const std::uint8_t *>(reason);
    frame.u.close.reason_length = static_cast<std::uint32_t>(std::strlen(reason));

    std::array<std::uint8_t, 256> payload{};
    QuicWriteCursor payload_writer(payload.data(), payload.size());
    auto frame_len = quic_create_output_frame(&payload_writer, frame);
    if (!frame_len) {
        return std::unexpected(frame_len.error());
    }

    QuicConnection::Options conn_options{};
    conn_options.role = QuicConnectionRole::Server;
    conn_options.local_addr = datagram.local;
    conn_options.remote_addr = datagram.peer;
    conn_options.original_destination_connection_id = packet.dcid;
    conn_options.initial_destination_connection_id = packet.dcid;
    conn_options.local_connection_id = packet.dcid;
    conn_options.remote_connection_id = packet.scid;

    QuicConnection temp(conn_options);
    auto initialized = temp.init_initial_crypto(packet.dcid);
    if (!initialized) {
        return std::unexpected(initialized.error());
    }

    QuicPacketEncodeSpec spec{};
    spec.level = QuicEncryptionLevel::Initial;
    spec.dcid = packet.scid;
    spec.scid = packet.dcid;
    spec.payload = payload.data();
    spec.payload_len = *frame_len;
    spec.payload_frame_count = 1;
    spec.payload_ack_eliciting = false;
    spec.max_packet_len = out_cap;

    auto encoded = quic_encode_packet(temp, spec, out, out_cap);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return encoded->packet_len;
}

} // namespace

QuicUdpEndpoint::QuicUdpEndpoint() noexcept = default;

QuicUdpEndpoint::~QuicUdpEndpoint() { close(); }

namespace {

[[nodiscard]] bool has_packet_space_work(const QuicPacketNumberSpace &space) noexcept {
    return (space.send_ack && space.pending_ack != kUnsetPacketNumber) || !space.pending_frames.empty();
}

[[nodiscard]] std::size_t padding_level_index(const QuicConnection &connection) noexcept {
    const QuicPacketNumberSpace &initial = connection.packet_number_space(QuicEncryptionLevel::Initial);
    bool initial_ack_eliciting = false;
    for (const QuicOutputFrame *frame = initial.pending_frames.front(); frame != nullptr;
         frame = initial.pending_frames.next_of(*frame)) {
        if (quic_output_frame_ack_eliciting(frame->type)) {
            initial_ack_eliciting = true;
            break;
        }
    }
    if (!initial_ack_eliciting) {
        return kQuicSendLevelCount;
    }

    std::size_t index = 0;
    for (; index + 1 < kQuicSendLevelCount; ++index) {
        if (connection.packet_number_space(kSendLevels[index + 1]).pending_frames.empty()) {
            break;
        }
    }
    return index;
}

void restore_sending_frames(QuicConnection &connection, QuicPacketNumberSpace &space) noexcept {
    QuicOutputFrameQueue restored{};
    while (QuicOutputFrame *frame = space.sending_frames.pop_front()) {
        if (frame->type == QuicFrameType::Stream) {
            (void) connection.on_stream_send_failed(frame->u.stream.stream_id,
                                                    static_cast<std::size_t>(frame->u.stream.offset),
                                                    frame->u.stream.length, frame->u.stream.fin);
            space.release_frame(*frame);
            continue;
        }
        if (frame->path != nullptr) {
            if (frame->path->allocated) {
                frame->path->pending_frames.push_front(*frame);
            } else {
                frame->path = nullptr;
                space.release_frame(*frame);
            }
            continue;
        }
        restored.push_back(*frame);
    }
    space.pending_frames.prepend_all(restored);
}

void discard_pending_frames(QuicConnection &connection, QuicPacketNumberSpace &space) noexcept {
    while (QuicOutputFrame *frame = space.pending_frames.pop_front()) {
        if (frame == &space.ack_frame) {
            space.send_ack = false;
            space.send_ack_count = 0;
        } else if (frame->type == QuicFrameType::Stream) {
            connection.drop_stream_send_ticket(frame->u.stream.stream_id);
        }
        space.release_frame(*frame);
    }
}

enum class QuicSplitFrameResult : std::uint8_t {
    Ok,
    Declined,
};

[[nodiscard]] std::size_t crypto_frame_len(std::uint64_t offset, std::size_t payload_len) noexcept {
    return 1 + quic_varint_len(offset) + quic_varint_len(payload_len) + payload_len;
}

[[nodiscard]] std::size_t max_crypto_payload_for_space(std::uint64_t offset, std::size_t payload_len,
                                                       std::size_t available) noexcept {
    if (payload_len == 0 || crypto_frame_len(offset, 1) > available) {
        return 0;
    }

    std::size_t lo = 1;
    std::size_t hi = payload_len;
    std::size_t best = 0;
    while (lo <= hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (crypto_frame_len(offset, mid) <= available) {
            best = mid;
            lo = mid + 1;
            continue;
        }
        hi = mid - 1;
    }
    return best;
}

[[nodiscard]] common::IoResult<QuicSplitFrameResult>
split_crypto_frame(QuicPacketNumberSpace &space, QuicOutputFrame &frame, std::size_t available) noexcept {
    if (frame.type != QuicFrameType::Crypto) {
        return QuicSplitFrameResult::Declined;
    }

    auto current_len = quic_output_frame_encoded_len(frame);
    if (!current_len) {
        return std::unexpected(current_len.error());
    }
    if (*current_len <= available) {
        return QuicSplitFrameResult::Ok;
    }

    if (frame.u.crypto.data == nullptr || !*frame.u.crypto.data) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const std::size_t payload_len = frame.u.crypto.data->readable();
    const std::size_t first_payload_len = max_crypto_payload_for_space(frame.u.crypto.offset, payload_len, available);

    if (first_payload_len == 0) {
        return QuicSplitFrameResult::Declined;
    }
    if (first_payload_len >= payload_len) {
        return QuicSplitFrameResult::Ok;
    }

    QuicOutputFrame *remainder = space.alloc_frame();
    if (remainder == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    auto *remainder_data = new (std::nothrow) mem::IoBuf{};
    if (remainder_data == nullptr) {
        space.release_frame(*remainder);
        return std::unexpected(common::IoErr::NoMem);
    }

    mem::IoBuf source = std::move(*frame.u.crypto.data);
    *frame.u.crypto.data = source.retain_slice(0, first_payload_len);
    *remainder_data = source.retain_slice(first_payload_len, payload_len - first_payload_len);

    remainder->type = QuicFrameType::Crypto;
    remainder->u.crypto.offset = frame.u.crypto.offset + first_payload_len;
    remainder->u.crypto.data = remainder_data;
    remainder->encoded_len = 0;

    frame.encoded_len = 0;

    auto split_len = quic_output_frame_encoded_len(frame);
    if (!split_len) {
        space.release_frame(*remainder);
        return std::unexpected(split_len.error());
    }
    if (*split_len > available) {
        space.release_frame(*remainder);
        return std::unexpected(common::IoErr::MessageTooLarge);
    }

    space.pending_frames.insert_after(frame, *remainder);
    return QuicSplitFrameResult::Ok;
}

[[nodiscard]] bool ack_frame_type(QuicFrameType type) noexcept {
    return type == QuicFrameType::Ack || type == QuicFrameType::AckEcn;
}

} // namespace

common::IoResult<QuicStreamFrameEncodeStatus>
QuicUdpEndpoint::encode_stream_frame_into_payload(QuicConnection &connection, QuicOutputFrame &frame, std::uint8_t *dst,
                                                  std::size_t available) noexcept {
    QuicStream *stream = connection.find_stream(frame.u.stream.stream_id);
    if (stream == nullptr) {
        return QuicStreamFrameEncodeStatus::Skipped;
    }
    return stream->encode_stream_frame(frame, dst, available);
}

namespace {

[[nodiscard]] common::IoResult<void> generate_ack_frame_into_pending(QuicPacketNumberSpace &space, QuicTime now,
                                                                     std::uint64_t ack_delay_exponent) noexcept {
    if (!space.send_ack || space.pending_ack == kUnsetPacketNumber) {
        return {};
    }
    if (space.ack_frame.queued) {
        return {};
    }

    // Use tracked ACK ranges when available, otherwise fall back to simple tracking.
    const std::uint64_t largest = space.largest_range != kUnsetPacketNumber ? space.largest_range : space.pending_ack;
    const std::uint64_t first_range = space.largest_range != kUnsetPacketNumber ? space.first_range : 0;
    const std::uint32_t range_count = space.largest_range != kUnsetPacketNumber ? space.ack_range_count : 0;

    std::uint64_t ack_delay_us = 0;
    if (space.level == QuicEncryptionLevel::Application && now > space.largest_received_time) {
        ack_delay_us = static_cast<std::uint64_t>((now - space.largest_received_time).count()) * 1000;
        if (ack_delay_exponent < 63) {
            ack_delay_us >>= ack_delay_exponent;
        } else {
            ack_delay_us = 0;
        }
    }

    // Serialize ACK range gap/range pairs (each a varint pair).
    // Max: 32 ranges × (8 + 8) bytes = 512 bytes.
    std::uint8_t range_buf[512];
    std::size_t range_buf_len = 0;
    if (range_count > 0) {
        QuicWriteCursor rcur(range_buf, sizeof(range_buf));
        for (std::uint32_t i = 0; i < range_count; ++i) {
            auto wrote = quic_write_varint(rcur, space.ack_ranges[i].gap);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
            wrote = quic_write_varint(rcur, space.ack_ranges[i].range);
            if (!wrote) {
                return std::unexpected(wrote.error());
            }
        }
        range_buf_len = rcur.offset();
    }

    space.ack_frame = QuicOutputFrame{};
    space.ack_frame.type = QuicFrameType::Ack;
    space.ack_frame.u.ack.largest = largest;
    space.ack_frame.u.ack.delay = ack_delay_us;
    space.ack_frame.u.ack.range_count = range_count;
    space.ack_frame.u.ack.first_range = first_range;

    if (range_buf_len > 0) {
        auto set_data = quic_output_frame_set_owned_data(space.ack_frame, range_buf, range_buf_len);
        if (!set_data) {
            return std::unexpected(set_data.error());
        }
    }

    auto frame_len = quic_output_frame_encoded_len(space.ack_frame);
    if (!frame_len) {
        return std::unexpected(frame_len.error());
    }
    space.pending_frames.push_front(space.ack_frame);
    return {};
}

} // namespace

common::IoResult<void> QuicUdpEndpoint::init(event::EventLoop &loop, const Options &options) noexcept {
    if (initialized_ || options.max_connections == 0 || options.send.send_buffer_size == 0 ||
        options.retry_token_lifetime.count() < 0 || options.new_token_lifetime.count() < 0) {
        return std::unexpected(common::IoErr::Invalid);
    }

    options_ = options;
    if (options_.retry) {
        options_.issue_new_token = true;
    }
    if ((options_.retry || options_.issue_new_token) && !options_.address_validation_key_set) {
        if (RAND_bytes(options_.address_validation_key.data(), options_.address_validation_key.size()) != 1) {
            return std::unexpected(common::IoErr::Invalid);
        }
        options_.address_validation_key_set = true;
    }
    loop_ = &loop;
    closing_ = false;
    active_connection_count_ = 0;
    dropped_datagram_count_ = 0;
    rejected_connection_count_ = 0;

    socket_ = std::make_unique<net::UdpSocket>(loop);
    read_buffer_ = std::make_unique<std::uint8_t[]>(kQuicUdpDefaultReadBufferSize);
    plaintext_buffer_ = std::make_unique<std::uint8_t[]>(kQuicUdpDefaultPlaintextBufferSize);
    send_buffer_ = std::make_unique<std::uint8_t[]>(options_.send.send_buffer_size);
    if (!socket_ || !read_buffer_ || !plaintext_buffer_ || !send_buffer_) {
        close();
        return std::unexpected(common::IoErr::NoMem);
    }

    net::UdpBindOptions bind_options = options_.udp;
    bind_options.recv_packet_info = true;
    bind_options.recv_ecn = true;
    auto bound = socket_->bind(options_.bind_addr, bind_options);
    if (!bound) {
        close();
        return std::unexpected(bound.error());
    }

    auto send_initialized = send_scheduler_.init(loop, *socket_, *this, options_.send);
    if (!send_initialized) {
        close();
        return std::unexpected(send_initialized.error());
    }

    initialized_ = true;
    async::spawn(loop, [this]() -> async::DetachedTask { co_await send_scheduler_.run(); });
    return {};
}

void QuicUdpEndpoint::close() noexcept {
    closing_ = true;
    send_scheduler_.close();
    if (socket_ && socket_->valid()) {
        socket_->close();
    }

    while (QuicConnection::EndpointIndex *index = connections_.front()) {
        delete_connection(*index->connection);
    }

    socket_.reset();
    read_buffer_.reset();
    plaintext_buffer_.reset();
    send_buffer_.reset();
    active_connection_count_ = 0;
    loop_ = nullptr;
    initialized_ = false;
}

bool QuicUdpEndpoint::valid() const noexcept { return socket_ && socket_->valid(); }

const net::SocketAddress &QuicUdpEndpoint::local_addr() const noexcept { return socket_->local_addr(); }

QuicConnection *QuicUdpEndpoint::find_connection(const QuicConnectionId &dcid) noexcept {
    return find_connection(dcid, hash_connection_id(dcid));
}

const QuicConnection *QuicUdpEndpoint::find_connection(const QuicConnectionId &dcid) const noexcept {
    return find_connection(dcid, hash_connection_id(dcid));
}

common::IoResult<void> QuicUdpEndpoint::remove_connection(const QuicConnectionId &dcid) noexcept {
    QuicConnection *connection = find_connection(dcid);
    if (connection == nullptr) {
        return std::unexpected(common::IoErr::NotFound);
    }
    delete_connection(*connection);
    return {};
}

void QuicUdpEndpoint::schedule_send(QuicConnection &connection) noexcept { send_scheduler_.submit(connection); }

common::IoResult<void> QuicUdpEndpoint::send_direct_datagram(const std::uint8_t *data, std::size_t len,
                                                             const QuicReceivedDatagram &datagram) noexcept {
    if (!valid() || data == nullptr || len == 0) {
        return std::unexpected(common::IoErr::Invalid);
    }

    net::UdpPacketSendSpec spec{};
    spec.buf = data;
    spec.len = len;
    spec.peer = datagram.peer;
    spec.local = datagram.local;
    spec.has_local = true;
    auto sent = socket_->try_send_packet(spec);
    if (!sent) {
        return std::unexpected(sent.error());
    }
    if (*sent != len) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return {};
}

async::Task<common::IoResult<QuicUdpReceiveResult>> QuicUdpEndpoint::recv_once() noexcept {
    if (!valid() || !read_buffer_) {
        co_return std::unexpected(common::IoErr::BadFd);
    }

    auto recv = co_await socket_->recv_packet(read_buffer_.get(), kQuicUdpDefaultReadBufferSize);
    if (!recv) {
        co_return std::unexpected(recv.error());
    }
    co_return process_datagram(*recv, loop_ != nullptr ? loop_->now() : std::chrono::steady_clock::now());
}

async::Task<void> QuicUdpEndpoint::recv_loop() noexcept {
    while (!closing_ && valid()) {
        auto processed = co_await recv_once();
        if (processed) {
            continue;
        }
        const common::IoErr err = processed.error();
        if (err == common::IoErr::Canceled || err == common::IoErr::BadFd) {
            break;
        }
    }
}

bool QuicUdpEndpoint::QuicConnectionDcidLess::operator()(
        const QuicConnection::ConnectionIdIndex *left, const QuicConnection::ConnectionIdIndex *right) const noexcept {
    return compare_dcid_key(left->cid_hash, left->cid_key, right->cid_hash, right->cid_key) < 0;
}

std::uint64_t QuicUdpEndpoint::hash_connection_id(const QuicConnectionId &id) noexcept {
    std::uint64_t hash = kFnvOffset;
    for (std::size_t i = 0; i < id.size(); ++i) {
        hash ^= id.bytes[i];
        hash *= kFnvPrime;
    }
    hash ^= id.size();
    hash *= kFnvPrime;
    return hash;
}

int QuicUdpEndpoint::compare_connection_id(const QuicConnectionId &left, const QuicConnectionId &right) noexcept {
    const std::size_t common_len = std::min(left.size(), right.size());
    if (common_len != 0) {
        const int cmp = std::memcmp(left.data(), right.data(), common_len);
        if (cmp != 0) {
            return cmp < 0 ? -1 : 1;
        }
    }
    if (left.size() == right.size()) {
        return 0;
    }
    return left.size() < right.size() ? -1 : 1;
}

int QuicUdpEndpoint::compare_dcid_key(std::uint64_t left_hash, const QuicConnectionId &left, std::uint64_t right_hash,
                                      const QuicConnectionId &right) noexcept {
    if (left_hash != right_hash) {
        return left_hash < right_hash ? -1 : 1;
    }
    return compare_connection_id(left, right);
}

QuicConnection::ConnectionIdIndex *QuicUdpEndpoint::index_from_dcid_hook(common::IntrusiveRbTreeHook *hook) noexcept {
    if (hook == nullptr || !hook->linked()) {
        return nullptr;
    }
    return reinterpret_cast<QuicConnection::ConnectionIdIndex *>(reinterpret_cast<std::uint8_t *>(hook) -
                                                                 offsetof(QuicConnection::ConnectionIdIndex, cid_hook));
}

const QuicConnection::ConnectionIdIndex *
QuicUdpEndpoint::index_from_dcid_hook(const common::IntrusiveRbTreeHook *hook) noexcept {
    if (hook == nullptr || !hook->linked()) {
        return nullptr;
    }
    return reinterpret_cast<const QuicConnection::ConnectionIdIndex *>(
            reinterpret_cast<const std::uint8_t *>(hook) - offsetof(QuicConnection::ConnectionIdIndex, cid_hook));
}

QuicConnection *QuicUdpEndpoint::find_connection(const QuicConnectionId &dcid, std::uint64_t hash) noexcept {
    QuicConnection::ConnectionIdIndex *node = dcid_tree_.root();
    while (node != nullptr) {
        const int cmp = compare_dcid_key(hash, dcid, node->cid_hash, node->cid_key);
        if (cmp == 0) {
            return node->connection;
        }
        node = cmp < 0 ? index_from_dcid_hook(node->cid_hook.left) : index_from_dcid_hook(node->cid_hook.right);
    }
    return nullptr;
}

const QuicConnection *QuicUdpEndpoint::find_connection(const QuicConnectionId &dcid,
                                                       std::uint64_t hash) const noexcept {
    const QuicConnection::ConnectionIdIndex *node = dcid_tree_.root();
    while (node != nullptr) {
        const int cmp = compare_dcid_key(hash, dcid, node->cid_hash, node->cid_key);
        if (cmp == 0) {
            return node->connection;
        }
        node = cmp < 0 ? index_from_dcid_hook(node->cid_hook.left) : index_from_dcid_hook(node->cid_hook.right);
    }
    return nullptr;
}

void QuicUdpEndpoint::delete_connection(QuicConnection &connection) noexcept {
    if (loop_ != nullptr) {
        connection.cancel_idle_timer(*loop_);
        connection.cancel_close_timer(*loop_);
        connection.cancel_keepalive_timer(*loop_);
        connection.cancel_loss_detection_timer(*loop_);
        connection.cancel_key_update_discard_timer(*loop_);
        connection.paths().cancel_validation_timer(*loop_);
    }
    send_scheduler_.remove(connection);
    QuicConnection::EndpointIndex &index = connection.endpoint_index;
    if (connection.original_dcid_index.cid_hook.linked()) {
        dcid_tree_.erase(connection.original_dcid_index);
    }
    if (connection.local_cid_index.cid_hook.linked()) {
        dcid_tree_.erase(connection.local_cid_index);
    }
    if (index.link.linked()) {
        connections_.erase(index);
    }
    if (active_connection_count_ != 0) {
        --active_connection_count_;
    }
    delete &connection;
}

void QuicUdpEndpoint::delete_connection_on_timer(void *owner, QuicConnection &connection) noexcept {
    if (owner == nullptr) {
        connection.mark_closed();
        return;
    }
    static_cast<QuicUdpEndpoint *>(owner)->delete_connection(connection);
}

common::IoResult<QuicConnectionId> QuicUdpEndpoint::generate_connection_id() noexcept {
    std::uint8_t bytes[kQuicConnectionIdLength]{};
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return QuicConnectionId::from_bytes(bytes, sizeof(bytes));
}

common::IoResult<void> QuicUdpEndpoint::register_connection_id(QuicConnection &connection,
                                                               QuicConnection::ConnectionIdIndex &index,
                                                               const QuicConnectionId &cid) noexcept {
    const std::uint64_t cid_hash = hash_connection_id(cid);
    if (find_connection(cid, cid_hash) != nullptr) {
        return std::unexpected(common::IoErr::Already);
    }

    index.connection = &connection;
    index.cid_key = cid;
    index.cid_hash = cid_hash;
    dcid_tree_.insert(index);
    return {};
}

common::IoResult<QuicUdpEndpoint::QuicInitialValidation>
QuicUdpEndpoint::validate_initial_address(const QuicPacketHeader &packet,
                                          const QuicReceivedDatagram &datagram) noexcept {
    QuicInitialValidation validation{};
    validation.original_destination_connection_id = packet.dcid;

    if (packet.token.empty()) {
        if (options_.retry) {
            validation.action = QuicInitialValidationAction::SendRetry;
        }
        return validation;
    }

    if (!options_.address_validation_key_set) {
        if (options_.retry) {
            validation.action = QuicInitialValidationAction::SendRetry;
        }
        return validation;
    }

    auto checked = quic_validate_address_token(options_.address_validation_key, datagram.peer, quic_unix_seconds_now(),
                                               packet.token);
    if (!checked) {
        return std::unexpected(checked.error());
    }

    switch (checked->status) {
        case QuicAddressTokenValidationStatus::Valid:
            validation.address_validated = true;
            if (checked->kind == QuicAddressTokenKind::Retry) {
                validation.retried = true;
                validation.original_destination_connection_id = checked->original_destination_connection_id;
                validation.retry_source_connection_id = packet.dcid;
            }
            return validation;

        case QuicAddressTokenValidationStatus::Garbage: {
            validation.action = QuicInitialValidationAction::SendInvalidTokenClose;
            validation.close_reason = kInvalidAddressValidationTokenReason;
            return validation;
        }

        case QuicAddressTokenValidationStatus::Invalid:
        case QuicAddressTokenValidationStatus::Expired:
            if (checked->kind == QuicAddressTokenKind::Retry) {
                validation.action = QuicInitialValidationAction::SendInvalidTokenClose;
                validation.close_reason = checked->status == QuicAddressTokenValidationStatus::Expired
                                                  ? kExpiredAddressValidationTokenReason
                                                  : kInvalidAddressValidationTokenReason;
                return validation;
            }
            if (options_.retry) {
                validation.action = QuicInitialValidationAction::SendRetry;
            }
            return validation;
    }

    return std::unexpected(common::IoErr::Invalid);
}

common::IoResult<void> QuicUdpEndpoint::queue_new_token(QuicConnection &connection, QuicPath &path) noexcept {
    if (!options_.issue_new_token) {
        return {};
    }
    if (!options_.address_validation_key_set) {
        return std::unexpected(common::IoErr::Invalid);
    }

    const std::uint64_t expires =
            quic_unix_seconds_now() + static_cast<std::uint64_t>(options_.new_token_lifetime.count());
    auto token = quic_create_address_token(options_.address_validation_key, path.remote, expires,
                                           QuicAddressTokenKind::NewToken, nullptr);
    if (!token) {
        return std::unexpected(token.error());
    }

    QuicPacketNumberSpace &space = connection.packet_number_space(QuicEncryptionLevel::Application);
    QuicOutputFrame *frame = space.alloc_frame();
    if (frame == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }

    frame->type = QuicFrameType::NewToken;
    auto copied = quic_output_frame_set_owned_data(*frame, token->bytes.data(), token->len);
    if (!copied) {
        space.release_frame(*frame);
        return std::unexpected(copied.error());
    }
    space.pending_frames.push_back(*frame);
    return {};
}

common::IoResult<QuicConnection *>
QuicUdpEndpoint::create_connection(const QuicPacketHeader &packet, const QuicReceivedDatagram &datagram,
                                   const QuicInitialValidation &validation) noexcept {
    const std::uint64_t dcid_hash = hash_connection_id(packet.dcid);
    if (find_connection(packet.dcid, dcid_hash) != nullptr) {
        return std::unexpected(common::IoErr::Already);
    }
    if (active_connection_count_ >= options_.max_connections) {
        ++rejected_connection_count_;
        return std::unexpected(common::IoErr::NoMem);
    }

    QuicConnectionId local_connection_id{};
    bool generated_unique_cid = false;
    for (std::uint8_t attempt = 0; attempt < 8; ++attempt) {
        auto generated = generate_connection_id();
        if (!generated) {
            ++rejected_connection_count_;
            return std::unexpected(generated.error());
        }
        if (compare_connection_id(*generated, packet.dcid) != 0 && find_connection(*generated) == nullptr) {
            local_connection_id = *generated;
            generated_unique_cid = true;
            break;
        }
    }
    if (!generated_unique_cid) {
        ++rejected_connection_count_;
        return std::unexpected(common::IoErr::Already);
    }

    QuicConnection::Options conn_options{};
    conn_options.role = QuicConnectionRole::Server;
    conn_options.local_addr = datagram.local;
    conn_options.remote_addr = datagram.peer;
    conn_options.original_destination_connection_id = validation.original_destination_connection_id;
    conn_options.initial_destination_connection_id = packet.dcid;
    conn_options.local_connection_id = local_connection_id;
    conn_options.remote_connection_id = packet.scid;
    conn_options.retry_source_connection_id = validation.retry_source_connection_id;
    conn_options.transport = options_.transport;
    conn_options.keepalive_interval = options_.keepalive_interval;
    conn_options.transport.max_ack_delay = options_.max_ack_delay;
    conn_options.transport.ack_delay_exponent = options_.ack_delay_exponent;
    conn_options.recv_flow = options_.recv_flow;
    conn_options.max_peer_bidirectional_streams = options_.transport.initial_max_streams_bidi;
    conn_options.max_peer_unidirectional_streams = options_.transport.initial_max_streams_uni;
    conn_options.output_frame_pool = &output_frame_pool_;
    conn_options.schedule_send_owner = this;
    conn_options.schedule_send = [](void *owner, QuicConnection &connection) noexcept {
        static_cast<QuicUdpEndpoint *>(owner)->schedule_send(connection);
    };
    conn_options.lifecycle_owner = this;
    conn_options.on_idle_timeout = &QuicUdpEndpoint::delete_connection_on_timer;
    conn_options.on_close_timeout = &QuicUdpEndpoint::delete_connection_on_timer;
    conn_options.has_retry_source_connection_id = validation.retried;
    conn_options.initial_path_validated = validation.address_validated;

    auto *connection = new (std::nothrow) QuicConnection(conn_options);
    if (connection == nullptr) {
        ++rejected_connection_count_;
        return std::unexpected(common::IoErr::NoMem);
    }

    connection->endpoint_index.connection = connection;
    auto registered = register_connection_id(*connection, connection->original_dcid_index, packet.dcid);
    if (!registered) {
        delete connection;
        return std::unexpected(registered.error());
    }
    registered = register_connection_id(*connection, connection->local_cid_index, local_connection_id);
    if (!registered) {
        dcid_tree_.erase(connection->original_dcid_index);
        delete connection;
        return std::unexpected(registered.error());
    }
    if (options_.tls_context != nullptr) {
        auto tls_initialized = connection->tls().init_server(*options_.tls_context, *connection);
        if (!tls_initialized) {
            dcid_tree_.erase(connection->original_dcid_index);
            dcid_tree_.erase(connection->local_cid_index);
            delete connection;
            return std::unexpected(tls_initialized.error());
        }
    }
    connections_.push_back(connection->endpoint_index);
    ++active_connection_count_;
    return connection;
}

common::IoResult<QuicUdpReceiveResult>
QuicUdpEndpoint::process_datagram(net::UdpPacketRecvResult recv, std::chrono::steady_clock::time_point now) noexcept {
    auto dcid = quic_get_packet_dcid(read_buffer_.get(), recv.size, static_cast<std::uint8_t>(kQuicConnectionIdLength));
    if (!dcid) {
        ++dropped_datagram_count_;
        return std::unexpected(dcid.error());
    }

    QuicReceivedDatagram datagram{};
    datagram.data = read_buffer_.get();
    datagram.len = recv.size;
    datagram.peer = recv.peer;
    datagram.local = recv.local;
    datagram.ecn = recv.ecn;
    datagram.received_at = now;

    const std::uint64_t dcid_hash = hash_connection_id(*dcid);
    QuicConnection *connection = find_connection(*dcid, dcid_hash);
    bool created = false;
    if (connection == nullptr) {
        auto packet = quic_parse_packet_header(read_buffer_.get(), recv.size,
                                               static_cast<std::uint8_t>(kQuicConnectionIdLength));
        if (!packet || !packet->long_header || packet->type != QuicPacketType::Initial ||
            packet->version != kQuicVersion1 || recv.size < kMinInitialDatagramSize) {
            ++dropped_datagram_count_;
            return std::unexpected(packet ? common::IoErr::Invalid : packet.error());
        }

        auto validation = validate_initial_address(*packet, datagram);
        if (!validation) {
            ++dropped_datagram_count_;
            return std::unexpected(validation.error());
        }

        if (validation->action == QuicInitialValidationAction::SendRetry) {
            QuicConnectionId retry_scid{};
            bool generated_unique_cid = false;
            for (std::uint8_t attempt = 0; attempt < 8; ++attempt) {
                auto generated = generate_connection_id();
                if (!generated) {
                    ++dropped_datagram_count_;
                    return std::unexpected(generated.error());
                }
                if (compare_connection_id(*generated, packet->dcid) != 0 &&
                    compare_connection_id(*generated, packet->scid) != 0 && find_connection(*generated) == nullptr) {
                    retry_scid = *generated;
                    generated_unique_cid = true;
                    break;
                }
            }
            if (!generated_unique_cid) {
                ++dropped_datagram_count_;
                return std::unexpected(common::IoErr::Already);
            }

            const std::uint64_t expires =
                    quic_unix_seconds_now() + static_cast<std::uint64_t>(options_.retry_token_lifetime.count());
            auto token = quic_create_address_token(options_.address_validation_key, datagram.peer, expires,
                                                   QuicAddressTokenKind::Retry, &packet->dcid);
            if (!token) {
                ++dropped_datagram_count_;
                return std::unexpected(token.error());
            }

            std::array<std::uint8_t, kQuicStatelessResponseBufferSize> out{};
            QuicWriteCursor writer(out.data(), out.size());
            auto written = encode_retry_packet(*packet, retry_scid, *token, writer);
            if (!written) {
                ++dropped_datagram_count_;
                return std::unexpected(written.error());
            }
            auto sent = send_direct_datagram(out.data(), *written, datagram);
            if (!sent) {
                ++dropped_datagram_count_;
                return std::unexpected(sent.error());
            }
            return std::unexpected(common::IoErr::WouldBlock);
        }

        if (validation->action == QuicInitialValidationAction::SendInvalidTokenClose) {
            std::array<std::uint8_t, kQuicStatelessResponseBufferSize> out{};
            auto written = encode_invalid_token_close_packet(*packet, datagram, validation->close_reason, out.data(),
                                                             out.size());
            if (!written) {
                ++dropped_datagram_count_;
                return std::unexpected(written.error());
            }
            auto sent = send_direct_datagram(out.data(), *written, datagram);
            if (!sent) {
                ++dropped_datagram_count_;
                return std::unexpected(sent.error());
            }
            return std::unexpected(common::IoErr::WouldBlock);
        }

        auto created_connection = create_connection(*packet, datagram, *validation);
        if (!created_connection) {
            ++dropped_datagram_count_;
            return std::unexpected(created_connection.error());
        }
        connection = *created_connection;
        created = true;
    }

    auto result =
            quic_process_datagram(*connection, datagram, plaintext_buffer_.get(), kQuicUdpDefaultPlaintextBufferSize,
                                  static_cast<std::uint8_t>(kQuicConnectionIdLength));
    if (!result) {
        ++dropped_datagram_count_;
        if (created) {
            delete_connection(*connection);
        }
        return std::unexpected(result.error());
    }

    QuicUdpReceiveResult out{};
    out.connection = connection;
    out.packet = *result;
    out.created = created;
    handle_receive_result(*connection, *result);
    return out;
}

void QuicUdpEndpoint::handle_receive_result(QuicConnection &connection,
                                            const QuicPacketProcessResult &result) noexcept {
    const QuicReceiveApplyResult applied = quic_apply_receive_result(connection, result);
    bool should_send = result.send_output;

    auto queue_token = [&](QuicPath *path) noexcept {
        if (path == nullptr) {
            return;
        }
        auto queued = queue_new_token(connection, *path);
        if (queued) {
            should_send = true;
        } else {
            connection.close(QuicErrorCode::InternalError);
            should_send = true;
        }
    };

    queue_token(applied.handshake_validated_path);
    if (result.path_validated && result.validated_path != applied.handshake_validated_path) {
        queue_token(result.validated_path);
    }

    if (result.send_ack && !should_send) {
        QuicPacketNumberSpace &space = connection.packet_number_space(result.level);
        const QuicTime now = loop_ != nullptr ? quic_time_ms(loop_->now()) : QuicTime{0};
        should_send = !should_delay_ack(space, now);
    }

    if (loop_ != nullptr) {
        if (connection.closing()) {
            // arm_close_timer is idempotent; this is a safety net in case close() was
            // called without an EventLoop context (the 3*PTO timer must be armed before
            // we let the connection idle).
            connection.arm_close_timer(*loop_);
        } else {
            connection.on_packet_processed(*loop_);
            connection.arm_loss_detection_timer(*loop_);
            connection.paths().arm_validation_timer(*loop_);
        }
    }

    // In Draining state we never send anything (RFC §10.2.2).
    if (connection.state() == QuicConnectionState::Draining || connection.state() == QuicConnectionState::Closed) {
        return;
    }

    // In Closing we may have CC frames queued by close() or by the packet processor
    // requeueing them in response to this datagram; in either case let the scheduler
    // flush them.
    if (connection.state() == QuicConnectionState::Closing) {
        if (connection_has_send_work(connection)) {
            schedule_send(connection);
        }
        return;
    }

    if (!should_send) {
        return;
    }

    schedule_send(connection);
}

bool QuicUdpEndpoint::should_delay_ack(const QuicPacketNumberSpace &space, QuicTime now) const noexcept {
    if (space.level != QuicEncryptionLevel::Application || !space.send_ack || space.pending_ack == kUnsetPacketNumber ||
        !space.pending_frames.empty() || space.send_ack_count >= kQuicMaxAckGap) {
        return false;
    }

    return now - space.ack_delay_start < options_.max_ack_delay;
}

common::IoResult<QuicBuildSendResult>
QuicUdpEndpoint::build_path_control_datagram(QuicConnection &connection, QuicSendDatagram &datagram) noexcept {
    if (!valid() || datagram.data == nullptr || datagram.capacity == 0 || connection.closing()) {
        return QuicBuildSendResult{QuicBuildSendStatus::Closed};
    }

    QuicPacketNumberSpace &space = connection.packet_number_space(QuicEncryptionLevel::Application);
    auto *keys = quic_packet_keys(connection.crypto(), QuicEncryptionLevel::Application, true);
    if (keys == nullptr || !keys->ready) {
        return QuicBuildSendResult{QuicBuildSendStatus::NoWork};
    }

    std::uint8_t *const data = datagram.data;
    const std::size_t capacity = datagram.capacity;

    for (QuicPath &candidate: connection.paths().paths()) {
        if (!candidate.allocated || candidate.pending_frames.empty()) {
            continue;
        }

        QuicOutputFrame *source = candidate.pending_frames.front();
        if (source == nullptr) {
            continue;
        }
        if (!quic_congestion_can_send(connection.congestion(), source->ignore_congestion)) {
            continue;
        }

        const std::size_t requested = std::min(capacity, candidate.mtu);
        const std::size_t allowed = QuicConnection::path_send_limit(candidate, requested);
        if (allowed == 0) {
            continue;
        }

        QuicPacketHeader header{};
        quic_init_packet_header(header, space, connection.key_phase());
        header.version = kQuicVersion1;
        header.dcid = candidate.remote_connection_id;
        header.scid = connection.local_connection_id();

        std::size_t min_packet_len = 0;
        if (source->min_packet_len != 0 &&
            QuicConnection::path_send_limit(candidate, source->min_packet_len) >= source->min_packet_len) {
            min_packet_len = source->min_packet_len;
        }

        const std::size_t max_payload = quic_packet_payload_capacity(header, allowed);
        auto frame_len = quic_output_frame_encoded_len(*source);
        if (!frame_len) {
            return std::unexpected(frame_len.error());
        }
        if (*frame_len > max_payload) {
            continue;
        }

        QuicOutputFrame *frame = candidate.pending_frames.pop_front();
        if (frame == nullptr) {
            return std::unexpected(common::IoErr::Invalid);
        }

        datagram = QuicSendDatagram{};
        datagram.data = data;
        datagram.capacity = allowed;
        datagram.path = &candidate;
        datagram.packet_number_snapshots[2] = quic_preserve_packet_number(space);
        datagram.packet_number_snapshot_valid[2] = true;

        QuicPacketEncodeSpec spec{};
        spec.level = QuicEncryptionLevel::Application;
        spec.dcid = candidate.remote_connection_id;
        spec.scid = connection.local_connection_id();
        spec.frames = frame;
        spec.frame_count = 1;
        spec.min_packet_len = min_packet_len;
        spec.max_packet_len = allowed;

        auto encoded = quic_encode_packet(connection, spec, datagram.data, datagram.capacity);
        if (!encoded) {
            candidate.pending_frames.push_front(*frame);
            datagram = QuicSendDatagram{};
            datagram.data = data;
            datagram.capacity = capacity;
            if (encoded.error() == common::IoErr::NoMem) {
                continue;
            }
            return std::unexpected(encoded.error());
        }

        const QuicTime now = loop_ != nullptr ? quic_time_ms(loop_->now()) : QuicTime{0};
        frame->packet_number = encoded->packet_number;
        frame->send_time = now;
        frame->packet_ack_eliciting = encoded->ack_eliciting;
        frame->packet_len = encoded->ack_eliciting ? static_cast<std::uint32_t>(encoded->packet_len) : 0;
        space.sending_frames.push_back(*frame);

        QuicSendPacketRecord &packet = datagram.packets[0];
        packet = QuicSendPacketRecord{};
        packet.level = QuicEncryptionLevel::Application;
        packet.length = encoded->packet_len;
        packet.packet_number = encoded->packet_number;
        packet.ack_eliciting = encoded->ack_eliciting;
        packet.frame_count = 1;
        packet.packet_number_snapshot = datagram.packet_number_snapshots[2];

        datagram.length = encoded->packet_len;
        datagram.packet_count = 1;
        datagram.spec.buf = datagram.data;
        datagram.spec.len = datagram.length;
        datagram.spec.peer = candidate.remote;
        datagram.spec.local = candidate.local;
        datagram.spec.has_local = true;
        return QuicBuildSendResult{QuicBuildSendStatus::Encoded};
    }

    return QuicBuildSendResult{QuicBuildSendStatus::NoWork};
}

common::IoResult<QuicBuildSendResult> QuicUdpEndpoint::build_send_datagram(QuicConnection &connection,
                                                                           QuicSendDatagram &datagram) noexcept {
    // In Closing state the connection has CC frames queued; the only way those
    // frames reach the wire is through this function. Draining and Closed never
    // send anything (RFC §10.2.2).
    if (!valid() || datagram.data == nullptr || datagram.capacity == 0 ||
        connection.state() == QuicConnectionState::Closed || connection.state() == QuicConnectionState::Draining) {
        return QuicBuildSendResult{QuicBuildSendStatus::Closed};
    }

    // Path-control frames (PATH_CHALLENGE / PATH_RESPONSE / NEW_CONNECTION_ID) are
    // suppressed once the connection is Closing — only the CC frame is allowed out.
    if (!connection.closing() && connection.has_path_send_work()) {
        auto path_control = build_path_control_datagram(connection, datagram);
        if (!path_control) {
            return std::unexpected(path_control.error());
        }
        if (path_control->status != QuicBuildSendStatus::NoWork) {
            return path_control;
        }
    }

    QuicPath *path = connection.active_path();
    if (path == nullptr) {
        return QuicBuildSendResult{QuicBuildSendStatus::Closed};
    }

    std::uint8_t *data = datagram.data;
    const std::size_t requested = std::min(datagram.capacity, path->mtu);
    datagram = QuicSendDatagram{};
    datagram.data = data;
    datagram.path = path;

    const std::size_t allowed = QuicConnection::path_send_limit(*path, requested);
    if (allowed == 0) {
        return QuicBuildSendResult{QuicBuildSendStatus::Blocked};
    }
    datagram.capacity = allowed;

    const QuicTime now = loop_ != nullptr ? quic_time_ms(loop_->now()) : QuicTime{0};
    const std::size_t pad_level = padding_level_index(connection);

    auto rollback_encoded = [&]() noexcept {
        for (std::size_t i = 0; i < kQuicSendLevelCount; ++i) {
            if (!datagram.packet_number_snapshot_valid[i]) {
                continue;
            }
            QuicPacketNumberSpace &space = connection.packet_number_space(kSendLevels[i]);
            restore_sending_frames(connection, space);
            quic_restore_packet_number(space, datagram.packet_number_snapshots[i]);
        }
        datagram.length = 0;
        datagram.packet_count = 0;
    };

    for (std::size_t level_index = 0; level_index < kQuicSendLevelCount; ++level_index) {
        QuicPacketNumberSpace &space = connection.packet_number_space(kSendLevels[level_index]);
        datagram.packet_number_snapshots[level_index] = quic_preserve_packet_number(space);
        datagram.packet_number_snapshot_valid[level_index] = true;
    }

    for (std::size_t level_index = 0; level_index < kQuicSendLevelCount; ++level_index) {
        QuicEncryptionLevel level = kSendLevels[level_index];
        QuicPacketNumberSpace &space = connection.packet_number_space(level);
        if (!has_packet_space_work(space)) {
            continue;
        }

        if (should_delay_ack(space, now)) {
            if (datagram.packet_count != 0) {
                break;
            }
            continue;
        }

        auto generated_ack = generate_ack_frame_into_pending(space, now, options_.ack_delay_exponent);
        if (!generated_ack) {
            return std::unexpected(generated_ack.error());
        }

        QuicSendPacketRecord &packet = datagram.packets[datagram.packet_count];
        packet = QuicSendPacketRecord{};
        packet.level = level;
        packet.packet_number_snapshot = datagram.packet_number_snapshots[level_index];

        std::size_t min_packet_len = 0;
        if (level_index == pad_level && datagram.length < kMinInitialDatagramSize) {
            min_packet_len = kMinInitialDatagramSize - datagram.length;
            if (min_packet_len > datagram.capacity - datagram.length) {
                rollback_encoded();
                return QuicBuildSendResult{QuicBuildSendStatus::Blocked};
            }
        }

        const std::size_t max_packet_len = datagram.capacity - datagram.length;
        auto *keys = quic_packet_keys(connection.crypto(), level, true);
        if (keys == nullptr || !keys->ready) {
            if (level == QuicEncryptionLevel::Application) {
                continue;
            }
            discard_pending_frames(connection, space);
            continue;
        }

        QuicPacketHeader header{};
        quic_init_packet_header(header, space, connection.key_phase());
        header.version = kQuicVersion1;
        header.dcid = path->remote_connection_id;
        header.scid = connection.local_connection_id();

        const std::size_t min_payload = std::max(quic_packet_payload_capacity(header, min_packet_len),
                                                 static_cast<std::size_t>(4U - header.pn_len));
        const std::size_t max_payload = quic_packet_payload_capacity(header, max_packet_len);
        if (min_payload > max_payload) {
            continue;
        }
        std::array<std::uint8_t, 4096> packet_payload{};
        if (max_payload > packet_payload.size()) {
            rollback_encoded();
            return std::unexpected(common::IoErr::MessageTooLarge);
        }

        const bool ack_only = connection.congestion().in_flight >= connection.congestion().window;
        std::size_t payload_len = 0;
        bool payload_ack_eliciting = false;
        while (QuicOutputFrame *source = space.pending_frames.front()) {
            if (ack_only && !ack_frame_type(source->type) && !source->ignore_congestion) {
                break;
            }

            if (payload_len >= max_payload) {
                break;
            }

            if (source->type == QuicFrameType::Stream) {
                QuicOutputFrame *frame = space.pending_frames.pop_front();
                if (frame == nullptr) {
                    packet = QuicSendPacketRecord{};
                    rollback_encoded();
                    return std::unexpected(common::IoErr::Invalid);
                }

                auto encoded_stream = encode_stream_frame_into_payload(
                        connection, *frame, packet_payload.data() + payload_len, max_payload - payload_len);
                if (!encoded_stream) {
                    space.release_frame(*frame);
                    packet = QuicSendPacketRecord{};
                    rollback_encoded();
                    return std::unexpected(encoded_stream.error());
                }
                if (*encoded_stream == QuicStreamFrameEncodeStatus::Skipped) {
                    const std::uint64_t stream_id = frame->u.stream.stream_id;
                    space.release_frame(*frame);
                    connection.drop_stream_send_ticket(stream_id);
                    continue;
                }
                if (*encoded_stream == QuicStreamFrameEncodeStatus::Blocked) {
                    space.pending_frames.push_front(*frame);
                    break;
                }

                payload_len += frame->encoded_len;
                payload_ack_eliciting = true;
                space.sending_frames.push_back(*frame);
                ++packet.frame_count;
                continue;
            }

            auto frame_len = quic_output_frame_encoded_len(*source);
            if (!frame_len) {
                packet = QuicSendPacketRecord{};
                rollback_encoded();
                return std::unexpected(frame_len.error());
            }

            if (payload_len + *frame_len > max_payload) {
                auto split = split_crypto_frame(space, *source, max_payload - payload_len);
                if (!split) {
                    packet = QuicSendPacketRecord{};
                    rollback_encoded();
                    return std::unexpected(split.error());
                }
                if (*split == QuicSplitFrameResult::Declined) {
                    break;
                }
                frame_len = quic_output_frame_encoded_len(*source);
                if (!frame_len) {
                    packet = QuicSendPacketRecord{};
                    rollback_encoded();
                    return std::unexpected(frame_len.error());
                }
            }

            QuicOutputFrame *frame = space.pending_frames.pop_front();
            if (frame == nullptr) {
                packet = QuicSendPacketRecord{};
                rollback_encoded();
                return std::unexpected(common::IoErr::Invalid);
            }
            space.sending_frames.push_back(*frame);
            packet.sends_ack = packet.sends_ack || frame == &space.ack_frame;
            payload_ack_eliciting = payload_ack_eliciting || quic_output_frame_ack_eliciting(frame->type);

            QuicWriteCursor payload_writer(packet_payload.data() + payload_len, max_payload - payload_len);
            auto written = quic_create_output_frame(&payload_writer, *frame);
            if (!written) {
                packet = QuicSendPacketRecord{};
                rollback_encoded();
                return std::unexpected(written.error());
            }
            payload_len += *written;
            ++packet.frame_count;
        }

        if (packet.frame_count == 0) {
            packet = QuicSendPacketRecord{};
            if (!space.pending_frames.empty()) {
                if (datagram.packet_count != 0) {
                    break;
                }
                return QuicBuildSendResult{QuicBuildSendStatus::Blocked};
            }
            continue;
        }

        QuicPacketEncodeSpec spec{};
        spec.level = level;
        spec.dcid = path->remote_connection_id;
        spec.scid = connection.local_connection_id();
        spec.payload = packet_payload.data();
        spec.payload_len = payload_len;
        spec.payload_frame_count = packet.frame_count;
        spec.payload_ack_eliciting = payload_ack_eliciting;
        spec.min_packet_len = min_packet_len;
        spec.max_packet_len = max_packet_len;

        auto encoded = quic_encode_packet(connection, spec, datagram.data + datagram.length,
                                          datagram.capacity - datagram.length);
        if (!encoded) {
            restore_sending_frames(connection, space);
            quic_restore_packet_number(space, packet.packet_number_snapshot);
            packet = QuicSendPacketRecord{};
            if (encoded.error() == common::IoErr::NotFound) {
                discard_pending_frames(connection, space);
                continue;
            }
            rollback_encoded();
            return std::unexpected(encoded.error());
        }

        packet.length = encoded->packet_len;
        packet.packet_number = encoded->packet_number;
        packet.ack_eliciting = encoded->ack_eliciting;

        bool accounted_packet = false;
        for (QuicOutputFrame *frame = space.sending_frames.front(); frame != nullptr;
             frame = space.sending_frames.next_of(*frame)) {
            frame->packet_number = encoded->packet_number;
            frame->send_time = now;
            frame->packet_ack_eliciting = encoded->ack_eliciting;
            frame->packet_len = 0;
            if (encoded->ack_eliciting && !accounted_packet) {
                frame->packet_len = static_cast<std::uint32_t>(encoded->packet_len);
                accounted_packet = true;
            }
        }

        datagram.length += encoded->packet_len;
        ++datagram.packet_count;
    }

    if (datagram.packet_count == 0) {
        return QuicBuildSendResult{QuicBuildSendStatus::NoWork};
    }

    datagram.spec.buf = datagram.data;
    datagram.spec.len = datagram.length;
    datagram.spec.peer = path->remote;
    datagram.spec.local = path->local;
    datagram.spec.has_local = true;
    return QuicBuildSendResult{QuicBuildSendStatus::Encoded};
}

void QuicUdpEndpoint::commit_send_datagram(QuicConnection &connection, const QuicSendDatagram &datagram) noexcept {
    const QuicTime now = loop_ != nullptr ? quic_time_ms(loop_->now()) : QuicTime{0};
    bool has_send_work = false;
    bool sent_ack_eliciting = false;

    for (std::size_t level_index = 0; level_index < kQuicSendLevelCount; ++level_index) {
        QuicPacketNumberSpace &space = connection.packet_number_space(kSendLevels[level_index]);
        if (!space.pending_frames.empty()) {
            has_send_work = true;
        }

        while (QuicOutputFrame *frame = space.sending_frames.pop_front()) {
            if (frame == &space.ack_frame) {
                space.send_ack = false;
                space.send_ack_count = 0;
            }

            if (frame->packet_ack_eliciting && !connection.closing()) {
                if (frame->packet_len != 0) {
                    quic_congestion_on_packet_sent(connection.congestion(), frame->packet_len, true, false);
                }
                space.sent_frames.push_back(*frame);
                sent_ack_eliciting = true;
            } else {
                space.release_frame(*frame);
            }
        }
    }

    if (datagram.path != nullptr) {
        connection.record_path_sent(*datagram.path, datagram.length);
    }
    has_send_work = has_send_work || connection_has_send_work(connection);
    quic_congestion_on_idle(connection.congestion(), !has_send_work, now);
    if (sent_ack_eliciting && loop_ != nullptr) {
        connection.on_ack_eliciting_packet_sent(*loop_);
        connection.arm_loss_detection_timer(*loop_);
    }
}

void QuicUdpEndpoint::rollback_send_datagram(QuicConnection &connection, const QuicSendDatagram &datagram) noexcept {
    for (std::size_t i = 0; i < kQuicSendLevelCount; ++i) {
        if (!datagram.packet_number_snapshot_valid[i]) {
            continue;
        }
        QuicPacketNumberSpace &space = connection.packet_number_space(kSendLevels[i]);
        restore_sending_frames(connection, space);
        quic_restore_packet_number(space, datagram.packet_number_snapshots[i]);
    }
    const QuicTime now = loop_ != nullptr ? quic_time_ms(loop_->now()) : QuicTime{0};
    quic_congestion_on_idle(connection.congestion(), true, now);
}

bool QuicUdpEndpoint::connection_has_send_work(const QuicConnection &connection) const noexcept {
    return connection.has_path_send_work() ||
           has_packet_space_work(connection.packet_number_space(QuicEncryptionLevel::Initial)) ||
           has_packet_space_work(connection.packet_number_space(QuicEncryptionLevel::Handshake)) ||
           has_packet_space_work(connection.packet_number_space(QuicEncryptionLevel::Application));
}

} // namespace fiber::quic
