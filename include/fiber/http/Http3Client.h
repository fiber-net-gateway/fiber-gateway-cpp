#ifndef FIBER_HTTP_HTTP3_CLIENT_H
#define FIBER_HTTP_HTTP3_CLIENT_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../net/TlsCredential.h"
#include "../net/TlsParams.h"
#include "../net/TrustStore.h"
#include "../quic/QuicClientConnect.h"
#include "../quic/QuicUdpEndpoint.h"
#include "Http3ClientConnection.h"

namespace fiber::http {

// Configuration shared by the HTTP/3 client connections built on one
// endpoint: TLS security, the session cache and the HTTP/3 settings. Holds no
// per-connection state; connections are Http3ClientConnection values the
// caller owns.
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

    // Credential and trust-store pointers are borrowed; their material, the
    // endpoint and this object must outlive every connection built on it.
    Http3Client(quic::QuicUdpEndpoint &endpoint, Options options) noexcept;
    ~Http3Client() = default;

    [[nodiscard]] quic::QuicUdpEndpoint &endpoint() const noexcept { return endpoint_; }
    [[nodiscard]] const Options &options() const noexcept { return options_; }
    // Fixed at "h3"; owned here so every connection's TlsClientParam can borrow it.
    [[nodiscard]] std::span<const std::string_view> alpn() const noexcept { return alpn_.view(); }
    [[nodiscard]] const net::TlsCredential *tls_credential() const noexcept { return options_.tls.credential; }
    [[nodiscard]] const net::TrustStore *trust_store() const noexcept { return options_.tls.trust_store; }

private:
    quic::QuicUdpEndpoint &endpoint_;
    Options options_;
    net::TlsAlpnList alpn_;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP3_CLIENT_H
