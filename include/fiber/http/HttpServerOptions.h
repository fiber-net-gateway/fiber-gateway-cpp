#ifndef FIBER_HTTP_HTTP_SERVER_OPTIONS_H
#define FIBER_HTTP_HTTP_SERVER_OPTIONS_H

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "../net/TcpSocketOptions.h"
#include "../net/TlsParams.h"
#include "../net/UdpSocket.h"
#include "../quic/QuicConnection.h"
#include "../quic/QuicSendScheduler.h"
#include "Http3Protocol.h"

namespace fiber::http {

struct HttpServerOptions {
    struct Http3Options {
        bool enabled = false;
        std::size_t max_connections_per_shard = 1024;
        std::size_t retained_storage_limit = quic::kQuicDefaultEndpointRetainedStorageLimit;
        net::UdpBindOptions udp{};
        quic::QuicSendScheduler::Options send{};
        quic::QuicTransportSettings transport{};
        quic::QuicRecvFlowControlSettings recv_flow{};
        Http3Settings settings{};
        std::chrono::milliseconds keepalive_interval{0};
        std::chrono::milliseconds max_ack_delay{25};
        std::uint64_t ack_delay_exponent = 3;
        bool retry = false;
        bool issue_new_token = false;
        bool enable_early_data = false;
    };

    net::TcpSocketOptions tcp{.no_delay = net::TcpOptionMode::Enabled};
    std::chrono::seconds keep_alive_timeout{70};
    std::chrono::seconds header_timeout{10};
    std::chrono::seconds body_timeout{60};
    std::chrono::seconds write_timeout{30};
    std::size_t header_init_size = 8 * 1024;
    std::size_t header_large_size = 32 * 1024;
    std::size_t header_large_num = 4;
    bool drain_unread_body = false;
    bool enable_extended_connect = false;
    net::TlsServerParam tls{};
    // Backing storage for tls.alpn; rebuilt by the normalize_*_alpn calls at
    // bind time, frozen afterwards (the span must stay valid across handshakes).
    net::TlsAlpnList tls_alpn{};
    Http3Options http3{};
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP_SERVER_OPTIONS_H
