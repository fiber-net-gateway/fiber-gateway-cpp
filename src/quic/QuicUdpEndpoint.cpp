#include "QuicUdpEndpoint.h"

#include <algorithm>
#include <cstring>
#include <expected>
#include <new>

#include <openssl/rand.h>

#include "../async/Spawn.h"
#include "QuicCongestion.h"
#include "QuicPacketCodec.h"
#include "QuicTransportCodec.h"

namespace fiber::quic {

namespace {

inline constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
inline constexpr std::size_t kPacketEncodingReserve = 64;
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
    if (!has_packet_space_work(initial)) {
        return kQuicSendMaxPacketsPerDatagram;
    }

    std::size_t index = 0;
    for (; index + 1 < kQuicSendMaxPacketsPerDatagram; ++index) {
        if (connection.packet_number_space(kSendLevels[index + 1]).pending_frames.empty()) {
            break;
        }
    }
    return index;
}

void copy_send_frame(QuicFrame &dst, const QuicFrame &src) noexcept {
    dst = src;
    dst.queue_hook = {};
}

[[nodiscard]] bool fill_ack_frame(const QuicPacketNumberSpace &space, QuicFrame &frame, QuicTime now,
                                  std::uint64_t ack_delay_exponent) noexcept {
    if (!space.send_ack || space.pending_ack == kUnsetPacketNumber) {
        return false;
    }

    const std::uint64_t largest = space.largest_received_packet_number != kUnsetPacketNumber
                                          ? space.largest_received_packet_number
                                          : space.pending_ack;
    std::uint64_t ack_delay_us = 0;
    if (space.level == QuicEncryptionLevel::Application && now > space.largest_received_time) {
        ack_delay_us = static_cast<std::uint64_t>((now - space.largest_received_time).count()) * 1000;
        if (ack_delay_exponent < 63) {
            ack_delay_us >>= ack_delay_exponent;
        } else {
            ack_delay_us = 0;
        }
    }

    frame = QuicFrame{};
    frame.type = QuicFrameType::Ack;
    frame.level = space.level;
    frame.u.ack.largest = largest;
    frame.u.ack.delay = ack_delay_us;
    frame.u.ack.range_count = 0;
    frame.u.ack.first_range = 0;
    return true;
}

} // namespace

common::IoResult<void> QuicUdpEndpoint::init(event::EventLoop &loop, const Options &options) noexcept {
    if (initialized_ || options.max_connections == 0 || options.read_buffer_size < kMinInitialDatagramSize ||
        options.plaintext_buffer_size == 0) {
        return std::unexpected(common::IoErr::Invalid);
    }

    options_ = options;
    loop_ = &loop;
    closing_ = false;
    active_connection_count_ = 0;
    dropped_datagram_count_ = 0;
    rejected_connection_count_ = 0;

    socket_ = std::make_unique<net::UdpSocket>(loop);
    read_buffer_ = std::make_unique<std::uint8_t[]>(options_.read_buffer_size);
    plaintext_buffer_ = std::make_unique<std::uint8_t[]>(options_.plaintext_buffer_size);
    if (!socket_ || !read_buffer_ || !plaintext_buffer_) {
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

void QuicUdpEndpoint::schedule_send_after(QuicConnection &connection, std::chrono::milliseconds delay) noexcept {
    send_scheduler_.submit_after(connection, delay);
}

async::Task<common::IoResult<QuicUdpReceiveResult>> QuicUdpEndpoint::recv_once() noexcept {
    if (!valid() || !read_buffer_) {
        co_return std::unexpected(common::IoErr::BadFd);
    }

    auto recv = co_await socket_->recv_packet(read_buffer_.get(), options_.read_buffer_size);
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

    auto result = quic_process_initial_datagram(*connection, datagram, plaintext_buffer_.get(),
                                                options_.plaintext_buffer_size);
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
        schedule_send_after(connection, ack_delay_remaining(space, now));
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

std::chrono::milliseconds QuicUdpEndpoint::ack_delay_remaining(const QuicPacketNumberSpace &space,
                                                               QuicTime now) const noexcept {
    const QuicTime elapsed = now - space.ack_delay_start;
    if (elapsed >= options_.max_ack_delay) {
        return std::chrono::milliseconds{0};
    }
    return options_.max_ack_delay - elapsed;
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
        for (std::size_t i = 0; i < datagram.packet_count; ++i) {
            QuicSendPacketRecord &packet = datagram.packets[i];
            QuicPacketNumberSpace &space = connection.packet_number_space(packet.level);
            quic_restore_packet_number(space, packet.packet_number_snapshot);
        }
        datagram.length = 0;
        datagram.packet_count = 0;
    };

    for (std::size_t level_index = 0; level_index < kQuicSendMaxPacketsPerDatagram; ++level_index) {
        QuicEncryptionLevel level = kSendLevels[level_index];
        QuicPacketNumberSpace &space = connection.packet_number_space(level);
        if (!has_packet_space_work(space)) {
            continue;
        }

        if (should_delay_ack(space, now)) {
            if (datagram.packet_count != 0) {
                break;
            }
            return QuicBuildSendResult{QuicBuildSendStatus::Delayed, ack_delay_remaining(space, now)};
        }

        QuicSendPacketRecord &packet = datagram.packets[datagram.packet_count];
        packet = QuicSendPacketRecord{};
        packet.level = level;
        packet.packet_number_snapshot = quic_preserve_packet_number(space);

        std::size_t estimated_payload_len = 0;
        if (fill_ack_frame(space, packet.frames[0], now, options_.ack_delay_exponent)) {
            auto frame_len = quic_create_frame(nullptr, packet.frames[0]);
            if (!frame_len) {
                quic_restore_packet_number(space, packet.packet_number_snapshot);
                packet = QuicSendPacketRecord{};
                return std::unexpected(frame_len.error());
            }
            estimated_payload_len += *frame_len;
            packet.frame_count = 1;
            packet.sends_ack = true;
        }

        QuicFrame *source = space.pending_frames.front();
        while (source != nullptr && packet.frame_count < kQuicSendMaxFramesPerDatagram) {
            if (!quic_congestion_can_send(connection.congestion(), source->ignore_congestion)) {
                break;
            }

            QuicFrame candidate{};
            copy_send_frame(candidate, *source);
            auto frame_len = quic_create_frame(nullptr, candidate);
            if (!frame_len) {
                quic_restore_packet_number(space, packet.packet_number_snapshot);
                packet = QuicSendPacketRecord{};
                return std::unexpected(frame_len.error());
            }

            if (packet.frame_count != 0 &&
                estimated_payload_len + *frame_len + kAeadTagLength + kPacketEncodingReserve >
                        datagram.capacity - datagram.length) {
                break;
            }

            copy_send_frame(packet.frames[packet.frame_count], candidate);
            packet.source_frames[packet.frame_count] = source;
            estimated_payload_len += *frame_len;
            ++packet.frame_count;

            source = space.pending_frames.next_of(*source);
        }

        if (packet.frame_count == 0) {
            quic_restore_packet_number(space, packet.packet_number_snapshot);
            packet = QuicSendPacketRecord{};
            if (!space.pending_frames.empty()) {
                if (datagram.packet_count != 0) {
                    break;
                }
                return QuicBuildSendResult{QuicBuildSendStatus::Blocked};
            }
            continue;
        }

        std::size_t min_packet_len = 0;
        if (level_index == pad_level && (level == QuicEncryptionLevel::Initial || datagram.packet_count != 0) &&
            datagram.length < kMinInitialDatagramSize) {
            min_packet_len = kMinInitialDatagramSize - datagram.length;
            if (min_packet_len > datagram.capacity - datagram.length) {
                quic_restore_packet_number(space, packet.packet_number_snapshot);
                packet = QuicSendPacketRecord{};
                rollback_encoded();
                return QuicBuildSendResult{QuicBuildSendStatus::Blocked};
            }
        }

        QuicPacketEncodeSpec spec{};
        spec.level = level;
        spec.dcid = path->remote_connection_id;
        spec.scid = connection.local_connection_id();
        spec.frames = packet.frames;
        spec.frame_count = packet.frame_count;
        spec.min_packet_len = min_packet_len;

        auto encoded = quic_encode_packet(connection, spec, datagram.data + datagram.length,
                                          datagram.capacity - datagram.length);
        if (!encoded) {
            quic_restore_packet_number(space, packet.packet_number_snapshot);
            packet = QuicSendPacketRecord{};
            if (encoded.error() == common::IoErr::NotFound) {
                continue;
            }
            rollback_encoded();
            return std::unexpected(encoded.error());
        }

        packet.length = encoded->packet_len;
        packet.packet_number = encoded->packet_number;
        packet.ack_eliciting = encoded->ack_eliciting;
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

    for (std::size_t packet_index = 0; packet_index < datagram.packet_count; ++packet_index) {
        const QuicSendPacketRecord &packet = datagram.packets[packet_index];
        QuicPacketNumberSpace &space = connection.packet_number_space(packet.level);
        if (packet.sends_ack) {
            space.send_ack = false;
            space.send_ack_count = 0;
        }

        bool accounted_packet = false;
        for (std::size_t i = 0; i < packet.frame_count; ++i) {
            QuicFrame *frame = packet.source_frames[i];
            if (frame == nullptr) {
                continue;
            }
            if (frame->queue_hook.linked()) {
                space.pending_frames.erase(*frame);
            }
            frame->packet_number = packet.packet_number;
            frame->send_time = now;
            frame->packet_ack_eliciting = packet.ack_eliciting;
            frame->packet_len = 0;

            if (packet.ack_eliciting && !connection.closing()) {
                if (!accounted_packet) {
                    frame->packet_len = packet.length;
                    accounted_packet = true;
                }
                space.sent_frames.push_back(*frame);
            }
        }

        quic_congestion_on_packet_sent(connection.congestion(), packet.length, packet.ack_eliciting,
                                       connection.closing());
        has_send_work = has_send_work || has_packet_space_work(space);
    }

    if (datagram.path != nullptr) {
        connection.record_path_sent(*datagram.path, datagram.length);
    }
    has_send_work = has_send_work || connection_has_send_work(connection);
    quic_congestion_on_idle(connection.congestion(), !has_send_work, now);
}

void QuicUdpEndpoint::rollback_send_datagram(QuicConnection &connection, const QuicSendDatagram &datagram) noexcept {
    for (std::size_t i = 0; i < datagram.packet_count; ++i) {
        const QuicSendPacketRecord &packet = datagram.packets[i];
        QuicPacketNumberSpace &space = connection.packet_number_space(packet.level);
        quic_restore_packet_number(space, packet.packet_number_snapshot);
    }
}

bool QuicUdpEndpoint::connection_has_send_work(const QuicConnection &connection) const noexcept {
    return has_packet_space_work(connection.packet_number_space(QuicEncryptionLevel::Initial)) ||
           has_packet_space_work(connection.packet_number_space(QuicEncryptionLevel::Handshake)) ||
           has_packet_space_work(connection.packet_number_space(QuicEncryptionLevel::Application));
}

} // namespace fiber::quic
