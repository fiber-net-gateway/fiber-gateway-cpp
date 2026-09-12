#ifndef FIBER_HTTP_HTTP3_CLIENT_H
#define FIBER_HTTP_HTTP3_CLIENT_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../net/TlsCredential.h"
#include "../net/TlsParams.h"
#include "../net/TrustStore.h"
#include "../quic/QuicClientConnect.h"
#include "../quic/QuicUdpEndpoint.h"
#include "Http3ClientConnection.h"

namespace fiber::http {

enum class Http3ClientConnectPhase : std::uint8_t {
    ClientInit,
    Quic,
    Alpn,
    Http3,
};

struct Http3ClientConnectError {
    Http3ClientConnectPhase phase = Http3ClientConnectPhase::ClientInit;
    common::IoErr io_error = common::IoErr::Unknown;
    quic::QuicConnectError quic_error{};
};

using Http3ClientConnectResult = std::expected<Http3ClientConnection, Http3ClientConnectError>;

struct Http3ClientConnectOptions {
    net::SocketAddress remote_addr{};
    std::string server_name{};
    std::string verify_name{};
    quic::QuicTransportSettings transport{};
    quic::QuicRecvFlowControlSettings recv_flow{};
    std::chrono::milliseconds keepalive_interval{0};
    std::chrono::milliseconds handshake_timeout{net::kDefaultTlsHandshakeTimeout};
    bool allow_insecure = false;
};

class Http3Client : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        net::TlsClientSecurity tls{.verify_peer = true};
        quic::QuicClientCacheOps cache{};
        Http3Settings local_settings{};
        std::chrono::milliseconds drain_timeout = std::chrono::seconds(3);
        std::uint32_t max_qpack_string_size = 64 * 1024;
        std::size_t max_field_section_size = 128 * 1024;
    };

    Http3Client(quic::QuicUdpEndpoint &endpoint, Options options) noexcept;
    // Connections borrow the cache callbacks and TLS material through this
    // object. The client and endpoint must outlive every connection created by
    // connect(); QuicUdpEndpoint::shutdown() is how an owner waits for that.
    ~Http3Client() = default;

    [[nodiscard]] common::IoResult<void> init() noexcept;
    // On the endpoint's loop. Every view in `options` is borrowed only until
    // the returned task completes.
    [[nodiscard]] async::Task<Http3ClientConnectResult> connect(Http3ClientConnectOptions options) noexcept;

    [[nodiscard]] const net::TlsCredential *tls_credential() const noexcept { return options_.tls.credential; }
    [[nodiscard]] const net::TrustStore *trust_store() const noexcept { return options_.tls.trust_store; }

private:
    [[nodiscard]] static Http3ClientConnectError make_error(Http3ClientConnectPhase phase,
                                                            common::IoErr error) noexcept;
    [[nodiscard]] static Http3ClientConnectError make_quic_error(const quic::QuicConnectError &error) noexcept;

    quic::QuicUdpEndpoint *endpoint_ = nullptr;
    Options options_{};
    net::TlsAlpnList alpn_{};
    bool initialized_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP3_CLIENT_H
