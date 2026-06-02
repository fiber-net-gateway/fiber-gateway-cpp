#include "QuicUdpEndpoint.h"

#include <algorithm>
#include <cstring>
#include <expected>
#include <new>

#include "QuicPacketCodec.h"
#include "QuicTransportCodec.h"

namespace fiber::quic {

namespace {

inline constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

} // namespace

QuicUdpEndpoint::QuicUdpEndpoint() noexcept = default;

QuicUdpEndpoint::~QuicUdpEndpoint() { close(); }

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

    initialized_ = true;
    return {};
}

void QuicUdpEndpoint::close() noexcept {
    closing_ = true;
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

async::Task<common::IoResult<QuicUdpReceiveResult>> QuicUdpEndpoint::recv_once() noexcept {
    if (!valid() || !read_buffer_) {
        co_return std::unexpected(common::IoErr::BadFd);
    }

    auto recv = co_await socket_->recv_packet(read_buffer_.get(), options_.read_buffer_size);
    if (!recv) {
        co_return std::unexpected(recv.error());
    }
    co_return process_datagram(*recv, std::chrono::steady_clock::now());
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

bool QuicUdpEndpoint::QuicConnectionDcidLess::operator()(const QuicConnection::EndpointIndex *left,
                                                         const QuicConnection::EndpointIndex *right) const noexcept {
    return compare_dcid_key(left->dcid_hash, left->dcid_key, right->dcid_hash, right->dcid_key) < 0;
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

int QuicUdpEndpoint::compare_dcid_key(std::uint64_t left_hash, const QuicConnectionId &left,
                                      std::uint64_t right_hash, const QuicConnectionId &right) noexcept {
    if (left_hash != right_hash) {
        return left_hash < right_hash ? -1 : 1;
    }
    return compare_connection_id(left, right);
}

QuicConnection::EndpointIndex *QuicUdpEndpoint::index_from_dcid_hook(common::IntrusiveRbTreeHook *hook) noexcept {
    if (hook == nullptr || !hook->linked()) {
        return nullptr;
    }
    return reinterpret_cast<QuicConnection::EndpointIndex *>(reinterpret_cast<std::uint8_t *>(hook) -
                                                             offsetof(QuicConnection::EndpointIndex, dcid_hook));
}

const QuicConnection::EndpointIndex *
QuicUdpEndpoint::index_from_dcid_hook(const common::IntrusiveRbTreeHook *hook) noexcept {
    if (hook == nullptr || !hook->linked()) {
        return nullptr;
    }
    return reinterpret_cast<const QuicConnection::EndpointIndex *>(reinterpret_cast<const std::uint8_t *>(hook) -
                                                                   offsetof(QuicConnection::EndpointIndex, dcid_hook));
}

QuicConnection *QuicUdpEndpoint::find_connection(const QuicConnectionId &dcid, std::uint64_t hash) noexcept {
    QuicConnection::EndpointIndex *node = dcid_tree_.root();
    while (node != nullptr) {
        const int cmp = compare_dcid_key(hash, dcid, node->dcid_hash, node->dcid_key);
        if (cmp == 0) {
            return node->connection;
        }
        node = cmp < 0 ? index_from_dcid_hook(node->dcid_hook.left) : index_from_dcid_hook(node->dcid_hook.right);
    }
    return nullptr;
}

const QuicConnection *QuicUdpEndpoint::find_connection(const QuicConnectionId &dcid, std::uint64_t hash) const
        noexcept {
    const QuicConnection::EndpointIndex *node = dcid_tree_.root();
    while (node != nullptr) {
        const int cmp = compare_dcid_key(hash, dcid, node->dcid_hash, node->dcid_key);
        if (cmp == 0) {
            return node->connection;
        }
        node = cmp < 0 ? index_from_dcid_hook(node->dcid_hook.left) : index_from_dcid_hook(node->dcid_hook.right);
    }
    return nullptr;
}

void QuicUdpEndpoint::delete_connection(QuicConnection &connection) noexcept {
    QuicConnection::EndpointIndex &index = connection.endpoint_index;
    if (index.dcid_hook.linked()) {
        dcid_tree_.erase(index);
    }
    if (index.link.linked()) {
        connections_.erase(index);
    }
    if (active_connection_count_ != 0) {
        --active_connection_count_;
    }
    delete &connection;
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

    QuicConnection::Options conn_options{};
    conn_options.role = QuicConnectionRole::Server;
    conn_options.local_addr = datagram.local;
    conn_options.remote_addr = datagram.peer;
    conn_options.local_connection_id = packet.dcid;
    conn_options.remote_connection_id = packet.scid;

    auto *connection = new (std::nothrow) QuicConnection(conn_options);
    if (connection == nullptr) {
        ++rejected_connection_count_;
        return std::unexpected(common::IoErr::NoMem);
    }

    connection->endpoint_index.connection = connection;
    connection->endpoint_index.dcid_key = packet.dcid;
    connection->endpoint_index.dcid_hash = dcid_hash;
    dcid_tree_.insert(connection->endpoint_index);
    connections_.push_back(connection->endpoint_index);
    ++active_connection_count_;
    return connection;
}

common::IoResult<QuicUdpReceiveResult>
QuicUdpEndpoint::process_datagram(net::UdpPacketRecvResult recv, std::chrono::steady_clock::time_point now) noexcept {
    auto dcid = quic_get_packet_dcid(read_buffer_.get(), recv.size, options_.short_connection_id_length);
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
        auto packet = quic_parse_packet_header(read_buffer_.get(), recv.size, options_.short_connection_id_length);
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
    return out;
}

} // namespace fiber::quic
