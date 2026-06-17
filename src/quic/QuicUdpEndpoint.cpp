#include "QuicUdpEndpoint.h"

#include <algorithm>
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
inline constexpr QuicEncryptionLevel kSendLevels[] = {QuicEncryptionLevel::Initial, QuicEncryptionLevel::Handshake,
                                                      QuicEncryptionLevel::Application};

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

[[nodiscard]] bool generated_stream_frame(const QuicOutputFrame &frame) noexcept {
    return (frame.flags & QuicOutputFrameGeneratedStream) != 0;
}

void restore_sending_frames(QuicConnection &connection, QuicPacketNumberSpace &space) noexcept {
    QuicOutputFrameQueue restored{};
    while (QuicOutputFrame *frame = space.sending_frames.pop_front()) {
        if (generated_stream_frame(*frame)) {
            (void) connection.on_stream_send_failed(frame->u.stream.stream_id,
                                                    static_cast<std::size_t>(frame->u.stream.offset), frame->data.len,
                                                    frame->u.stream.fin);
            space.release_frame(*frame);
            continue;
        }
        restored.push_back(*frame);
    }
    space.pending_frames.prepend_all(restored);
}

void discard_pending_frames(QuicPacketNumberSpace &space) noexcept {
    while (QuicOutputFrame *frame = space.pending_frames.pop_front()) {
        if (frame == &space.ack_frame) {
            space.send_ack = false;
            space.send_ack_count = 0;
        }
        space.release_frame(*frame);
    }
}

enum class QuicSplitFrameResult : std::uint8_t {
    Ok,
    Declined,
};

[[nodiscard]] common::IoResult<QuicSplitFrameResult>
split_ordered_frame(QuicPacketNumberSpace &space, QuicOutputFrame &frame, std::size_t available) noexcept {
    if (frame.type != QuicFrameType::Crypto && frame.type != QuicFrameType::Stream) {
        return QuicSplitFrameResult::Declined;
    }

    auto current_len = quic_output_frame_encoded_len(frame);
    if (!current_len) {
        return std::unexpected(current_len.error());
    }
    if (*current_len <= available) {
        return QuicSplitFrameResult::Ok;
    }

    const std::size_t shrink = *current_len - available;
    std::uint64_t payload_len = 0;
    if (frame.type == QuicFrameType::Crypto) {
        payload_len = frame.data.len;
    } else {
        payload_len = frame.data.len;
    }
    if (payload_len <= shrink) {
        return QuicSplitFrameResult::Declined;
    }
    if (frame.data.data == nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }

    const std::uint64_t first_payload_len = payload_len - shrink;
    QuicOutputFrame *remainder = space.alloc_frame();
    if (remainder == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }

    *remainder = frame;
    quic_output_frame_retain_data(*remainder);
    remainder->encoded_len = 0;
    remainder->next = nullptr;
    remainder->queued = false;
    remainder->data.data = frame.data.data + first_payload_len;
    remainder->data.len = static_cast<std::size_t>(shrink);

    frame.encoded_len = 0;
    frame.data.len = static_cast<std::size_t>(first_payload_len);

    if (frame.type == QuicFrameType::Crypto) {
        remainder->u.crypto.offset += first_payload_len;
    } else {
        const bool original_fin = frame.u.stream.fin;
        frame.u.stream.has_length = true;
        frame.u.stream.fin = false;
        remainder->u.stream.offset += first_payload_len;
        remainder->u.stream.has_length = true;
        remainder->u.stream.fin = original_fin;
    }

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

common::IoResult<bool> QuicUdpEndpoint::append_stream_frame(QuicConnection &connection, QuicPacketNumberSpace &space,
                                                            QuicSendPacketRecord &packet,
                                                            std::size_t available) noexcept {
    while (connection.has_stream_send_work()) {
        QuicStream *stream = connection.pop_stream_send();
        if (stream == nullptr) {
            continue;
        }

        auto prepared = stream->prepare_stream_frame(available);
        if (!prepared) {
            connection.requeue_stream_send_if_needed(*stream);
            return std::unexpected(prepared.error());
        }
        if (!prepared->encoded) {
            connection.requeue_stream_send_if_needed(*stream);
            return false;
        }

        QuicOutputFrame *frame = space.alloc_frame();
        if (frame == nullptr) {
            (void) stream->mark_send_failed(prepared->offset, prepared->data_len, prepared->fin);
            connection.requeue_stream_send_if_needed(*stream);
            return std::unexpected(common::IoErr::NoMem);
        }

        frame->type = QuicFrameType::Stream;
        frame->encoded_len = prepared->encoded_len;
        frame->flags = QuicOutputFrameGeneratedStream;
        frame->data = {prepared->data, prepared->data_len};
        frame->u.stream.stream_id = stream->stream_id();
        frame->u.stream.offset = prepared->offset;
        frame->u.stream.has_length = prepared->has_length;
        frame->u.stream.fin = prepared->fin;
        space.sending_frames.push_back(*frame);
        ++packet.frame_count;
        packet.ack_eliciting = true;

        connection.requeue_stream_send_if_needed(*stream);
        return true;
    }

    return false;
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
    if (initialized_ || options.max_connections == 0 || options.send.send_buffer_size == 0) {
        return std::unexpected(common::IoErr::Invalid);
    }

    options_ = options;
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

common::IoResult<QuicConnection *> QuicUdpEndpoint::create_connection(const QuicPacketHeader &packet,
                                                                      const QuicReceivedDatagram &datagram) noexcept {
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
    conn_options.original_destination_connection_id = packet.dcid;
    conn_options.local_connection_id = local_connection_id;
    conn_options.remote_connection_id = packet.scid;
    conn_options.transport = options_.transport;
    conn_options.transport.max_ack_delay = options_.max_ack_delay;
    conn_options.transport.ack_delay_exponent = options_.ack_delay_exponent;
    conn_options.max_peer_bidirectional_streams = options_.transport.initial_max_streams_bidi;
    conn_options.max_peer_unidirectional_streams = options_.transport.initial_max_streams_uni;
    conn_options.output_frame_pool = &output_frame_pool_;
    conn_options.schedule_send_owner = this;
    conn_options.schedule_send = [](void *owner, QuicConnection &connection) noexcept {
        static_cast<QuicUdpEndpoint *>(owner)->schedule_send(connection);
    };

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

        auto created_connection = create_connection(*packet, datagram);
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
    schedule_after_receive(*connection, *result);
    return out;
}

void QuicUdpEndpoint::schedule_after_receive(QuicConnection &connection,
                                             const QuicPacketProcessResult &result) noexcept {
    if (result.send_output) {
        schedule_send(connection);
        return;
    }

    if (!result.send_ack) {
        return;
    }

    QuicPacketNumberSpace &space = connection.packet_number_space(result.level);
    const QuicTime now = loop_ != nullptr ? quic_time_ms(loop_->now()) : QuicTime{0};
    if (should_delay_ack(space, now)) {
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

common::IoResult<QuicBuildSendResult> QuicUdpEndpoint::build_send_datagram(QuicConnection &connection,
                                                                           QuicSendDatagram &datagram) noexcept {
    if (!valid() || datagram.data == nullptr || datagram.capacity == 0 || connection.closing()) {
        return QuicBuildSendResult{QuicBuildSendStatus::Closed};
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
        const bool level_can_send_stream =
                level == QuicEncryptionLevel::Application && connection.has_stream_send_work();
        if (!has_packet_space_work(space) && !level_can_send_stream) {
            continue;
        }

        if (should_delay_ack(space, now) && !level_can_send_stream) {
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
            discard_pending_frames(space);
            continue;
        }

        QuicPacketHeader header{};
        quic_init_packet_header(header, space);
        header.version = kQuicVersion1;
        header.dcid = path->remote_connection_id;
        header.scid = connection.local_connection_id();

        const std::size_t min_payload = std::max(quic_packet_payload_capacity(header, min_packet_len),
                                                 static_cast<std::size_t>(4U - header.pn_len));
        const std::size_t max_payload = quic_packet_payload_capacity(header, max_packet_len);
        if (min_payload > max_payload) {
            continue;
        }

        const bool ack_only = connection.congestion().in_flight >= connection.congestion().window;
        std::size_t payload_len = 0;
        std::size_t control_frame_count = 0;
        QuicOutputFrame *source = space.pending_frames.front();
        while (source != nullptr) {
            if (ack_only && !ack_frame_type(source->type)) {
                break;
            }

            auto frame_len = quic_output_frame_encoded_len(*source);
            if (!frame_len) {
                packet = QuicSendPacketRecord{};
                rollback_encoded();
                return std::unexpected(frame_len.error());
            }

            if (payload_len >= max_payload) {
                break;
            }
            if (payload_len + *frame_len > max_payload) {
                auto split = split_ordered_frame(space, *source, max_payload - payload_len);
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

            payload_len += *frame_len;
            ++control_frame_count;
            source = space.pending_frames.next_of(*source);
        }

        for (std::size_t i = 0; i < control_frame_count; ++i) {
            QuicOutputFrame *frame = space.pending_frames.pop_front();
            if (frame == nullptr) {
                packet = QuicSendPacketRecord{};
                rollback_encoded();
                return std::unexpected(common::IoErr::Invalid);
            }
            space.sending_frames.push_back(*frame);
            packet.sends_ack = packet.sends_ack || frame == &space.ack_frame;
        }
        packet.frame_count = control_frame_count;

        if (!ack_only && level == QuicEncryptionLevel::Application && payload_len < max_payload) {
            auto stream_appended = append_stream_frame(connection, space, packet, max_payload - payload_len);
            if (!stream_appended) {
                packet = QuicSendPacketRecord{};
                rollback_encoded();
                return std::unexpected(stream_appended.error());
            }
            if (*stream_appended) {
                payload_len = 0;
                for (QuicOutputFrame *frame = space.sending_frames.front(); frame != nullptr;
                     frame = space.sending_frames.next_of(*frame)) {
                    auto frame_len = quic_output_frame_encoded_len(*frame);
                    if (!frame_len) {
                        packet = QuicSendPacketRecord{};
                        rollback_encoded();
                        return std::unexpected(frame_len.error());
                    }
                    payload_len += *frame_len;
                }
            }
        }

        if (packet.frame_count == 0) {
            packet = QuicSendPacketRecord{};
            if (!space.pending_frames.empty() || level_can_send_stream) {
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
        spec.frame_queue = &space.sending_frames;
        spec.min_packet_len = min_packet_len;
        spec.max_packet_len = max_packet_len;

        auto encoded = quic_encode_packet(connection, spec, datagram.data + datagram.length,
                                          datagram.capacity - datagram.length);
        if (!encoded) {
            restore_sending_frames(connection, space);
            quic_restore_packet_number(space, packet.packet_number_snapshot);
            packet = QuicSendPacketRecord{};
            if (encoded.error() == common::IoErr::NotFound) {
                discard_pending_frames(space);
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
            if (encoded->ack_eliciting) {
                frame->flags |= QuicOutputFramePacketAckEliciting;
            }
            frame->packet_len = 0;
            if (encoded->ack_eliciting && !accounted_packet) {
                frame->flags |= QuicOutputFramePacketAnchor;
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

            if ((frame->flags & QuicOutputFramePacketAckEliciting) != 0 && !connection.closing()) {
                if (frame->packet_len != 0) {
                    quic_congestion_on_packet_sent(connection.congestion(), frame->packet_len, true, false);
                }
                space.sent_frames.push_back(*frame);
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
    return has_packet_space_work(connection.packet_number_space(QuicEncryptionLevel::Initial)) ||
           has_packet_space_work(connection.packet_number_space(QuicEncryptionLevel::Handshake)) ||
           has_packet_space_work(connection.packet_number_space(QuicEncryptionLevel::Application)) ||
           connection.has_stream_send_work();
}

} // namespace fiber::quic
