#ifndef FIBER_QUIC_QUIC_UDP_ENDPOINT_H
#define FIBER_QUIC_QUIC_UDP_ENDPOINT_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "../async/Task.h"
#include "../common/IntrusiveList.h"
#include "../common/IntrusiveRbTree.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "../net/SocketAddress.h"
#include "../net/UdpSocket.h"
#include "QuicConnection.h"
#include "QuicPacketProcessor.h"
#include "QuicSendScheduler.h"
#include "QuicStreamReassembler.h"

namespace fiber::net {
class TlsServerContext;
} // namespace fiber::net

namespace fiber::quic {

inline constexpr std::size_t kQuicUdpDefaultReadBufferSize = 65536;
inline constexpr std::size_t kQuicUdpDefaultPlaintextBufferSize = 65536;

struct QuicUdpReceiveResult {
    QuicConnection *connection = nullptr;
    QuicPacketProcessResult packet{};
    bool created = false;
};

class QuicUdpEndpoint : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        net::SocketAddress bind_addr{};
        std::size_t max_connections = 1024;
        net::TlsServerContext *tls_context = nullptr;
        net::UdpBindOptions udp{};
        QuicSendScheduler::Options send{};
        QuicTransportSettings transport{};
        std::chrono::milliseconds max_ack_delay{25};
        std::uint64_t ack_delay_exponent = 3;
    };

    QuicUdpEndpoint() noexcept;
    ~QuicUdpEndpoint();

    [[nodiscard]] common::IoResult<void> init(event::EventLoop &loop, const Options &options) noexcept;
    void close() noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const net::SocketAddress &local_addr() const noexcept;
    [[nodiscard]] std::size_t active_connection_count() const noexcept { return active_connection_count_; }
    [[nodiscard]] std::size_t dropped_datagram_count() const noexcept { return dropped_datagram_count_; }
    [[nodiscard]] std::size_t rejected_connection_count() const noexcept { return rejected_connection_count_; }
    [[nodiscard]] QuicRecvExtentPool &recv_extent_pool() noexcept { return recv_extent_pool_; }

    [[nodiscard]] QuicConnection *find_connection(const QuicConnectionId &dcid) noexcept;
    [[nodiscard]] const QuicConnection *find_connection(const QuicConnectionId &dcid) const noexcept;
    [[nodiscard]] common::IoResult<void> remove_connection(const QuicConnectionId &dcid) noexcept;
    void schedule_send(QuicConnection &connection) noexcept;
    void schedule_send_after(QuicConnection &connection, std::chrono::milliseconds delay) noexcept;

    [[nodiscard]] async::Task<common::IoResult<QuicUdpReceiveResult>> recv_once() noexcept;
    [[nodiscard]] async::Task<void> recv_loop() noexcept;

private:
    friend class QuicSendScheduler;

    struct QuicConnectionDcidLess {
        [[nodiscard]] bool operator()(const QuicConnection::ConnectionIdIndex *left,
                                      const QuicConnection::ConnectionIdIndex *right) const noexcept;
    };

    using DcidTree =
            common::IntrusiveRbTree<QuicConnection::ConnectionIdIndex,
                                    offsetof(QuicConnection::ConnectionIdIndex, cid_hook), QuicConnectionDcidLess>;
    using ConnectionList =
            common::IntrusiveList<QuicConnection::EndpointIndex, offsetof(QuicConnection::EndpointIndex, link)>;

    [[nodiscard]] static std::uint64_t hash_connection_id(const QuicConnectionId &id) noexcept;
    [[nodiscard]] static int compare_connection_id(const QuicConnectionId &left,
                                                   const QuicConnectionId &right) noexcept;
    [[nodiscard]] static int compare_dcid_key(std::uint64_t left_hash, const QuicConnectionId &left,
                                              std::uint64_t right_hash, const QuicConnectionId &right) noexcept;
    [[nodiscard]] static QuicConnection::ConnectionIdIndex *
    index_from_dcid_hook(common::IntrusiveRbTreeHook *hook) noexcept;
    [[nodiscard]] static const QuicConnection::ConnectionIdIndex *
    index_from_dcid_hook(const common::IntrusiveRbTreeHook *hook) noexcept;

    [[nodiscard]] QuicConnection *find_connection(const QuicConnectionId &dcid, std::uint64_t hash) noexcept;
    [[nodiscard]] const QuicConnection *find_connection(const QuicConnectionId &dcid,
                                                        std::uint64_t hash) const noexcept;
    void delete_connection(QuicConnection &connection) noexcept;
    [[nodiscard]] common::IoResult<QuicConnectionId> generate_connection_id() noexcept;
    [[nodiscard]] common::IoResult<void> register_connection_id(QuicConnection &connection,
                                                                QuicConnection::ConnectionIdIndex &index,
                                                                const QuicConnectionId &cid) noexcept;
    [[nodiscard]] common::IoResult<QuicConnection *> create_connection(const QuicPacketHeader &packet,
                                                                       const QuicReceivedDatagram &datagram) noexcept;
    [[nodiscard]] common::IoResult<QuicUdpReceiveResult>
    process_datagram(net::UdpPacketRecvResult recv, std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] common::IoResult<QuicBuildSendResult> build_send_datagram(QuicConnection &connection,
                                                                            QuicSendDatagram &datagram) noexcept;
    void commit_send_datagram(QuicConnection &connection, const QuicSendDatagram &datagram) noexcept;
    void rollback_send_datagram(QuicConnection &connection, const QuicSendDatagram &datagram) noexcept;
    [[nodiscard]] bool connection_has_send_work(const QuicConnection &connection) const noexcept;
    void schedule_after_receive(QuicConnection &connection, const QuicPacketProcessResult &result) noexcept;
    [[nodiscard]] bool should_delay_ack(const QuicPacketNumberSpace &space, QuicTime now) const noexcept;
    [[nodiscard]] std::chrono::milliseconds ack_delay_remaining(const QuicPacketNumberSpace &space,
                                                                QuicTime now) const noexcept;

    Options options_{};
    event::EventLoop *loop_ = nullptr;
    std::unique_ptr<net::UdpSocket> socket_{};
    std::unique_ptr<std::uint8_t[]> read_buffer_{};
    std::unique_ptr<std::uint8_t[]> plaintext_buffer_{};
    QuicOutputFramePool output_frame_pool_{};
    QuicRecvExtentPool recv_extent_pool_{};
    QuicSendScheduler send_scheduler_{};
    DcidTree dcid_tree_{};
    ConnectionList connections_{};
    std::size_t active_connection_count_ = 0;
    std::size_t dropped_datagram_count_ = 0;
    std::size_t rejected_connection_count_ = 0;
    bool initialized_ = false;
    bool closing_ = false;
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_UDP_ENDPOINT_H
