#ifndef FIBER_HTTP_HTTP3_SERVER_OPTIONS_H
#define FIBER_HTTP_HTTP3_SERVER_OPTIONS_H

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "../net/UdpSocket.h"
#include "../quic/QuicConnection.h"
#include "../quic/QuicSendScheduler.h"
#include "Http3Protocol.h"

namespace fiber::http {

// Everything an HTTP/3 server endpoint reads: the QUIC transport and endpoint
// knobs, plus the two request-level fields ServerHttp3Request uses. Split out
// of the old catch-all HttpServerOptions (D5), whose Http3Options member this
// replaces; body_timeout lived there as a shared field but HTTP/1 never read
// it, so it belongs here.
struct Http3ServerOptions {
    // One QuicUdpEndpoint is bound per worker loop (SO_REUSEPORT), so this is
    // a per-shard ceiling rather than a server-wide one.
    std::size_t max_connections_per_shard = 1024;
    std::size_t retained_storage_limit = quic::kQuicDefaultEndpointRetainedStorageLimit;
    net::UdpBindOptions udp{};
    quic::QuicSendScheduler::Options send{};
    quic::QuicTransportSettings transport{};
    quic::QuicRecvFlowControlSettings recv_flow{};
    Http3Settings settings{};
    std::chrono::milliseconds keepalive_interval{0};
    std::chrono::milliseconds max_ack_delay{25};
    // Budget for reading one request body chunk.
    std::chrono::seconds body_timeout{60};
    // Request-header arena sizing for the large path.
    std::size_t header_large_size = 32 * 1024;
    std::uint64_t ack_delay_exponent = 3;
    // Advertise SETTINGS_ENABLE_CONNECT_PROTOCOL (RFC 9220).
    bool enable_connect_protocol = false;
    bool retry = false;
    bool issue_new_token = false;
    bool enable_early_data = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP3_SERVER_OPTIONS_H
