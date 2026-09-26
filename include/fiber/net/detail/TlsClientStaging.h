#ifndef FIBER_NET_DETAIL_TLS_CLIENT_STAGING_H
#define FIBER_NET_DETAIL_TLS_CLIENT_STAGING_H

// The TlsClientParam → tls::TlsClientConfig staging shared by the two client
// glue paths (TlsStreamFd over TCP, QuicTlsSession over QUIC) — internal to
// the fiber_lib glue, not a public face.

#include <array>
#include <cstdint>

#include "../../common/IoError.h"
#include "../../tls/TlsConfig.h"
#include "../TlsParams.h"

namespace fiber::net::detail {

// Certificate-validity snapshot: wall clock (the engines' now_unix_ms is a
// real-time input; EventLoop::now() is a steady monotonic source). Also the
// ticket-age base for session receipts and offers.
[[nodiscard]] std::int64_t system_now_unix_ms() noexcept;

// The TlsClientParam → tls::TlsClientConfig staging shared by the two client
// glue paths (TlsStreamFd over TCP, QuicTlsSession over QUIC). A static-only
// class so it can friend into TlsCredential/TrustStore's private tls-material
// accessors, like TlsServerHandshakeConfig does.
class TlsClientStager {
public:
    // Stages the transport-common subset of `param` into `cfg`: SNI (a non-IP
    // server_name), peer verification (a null trust store = the process-wide
    // system roots; the verify-name host/IP split), the client certificate,
    // ALPN, and the clock snapshot. Version bounds, QUIC-mode fields, and
    // resumption staging stay with the caller — the TCP path validates and
    // copies the param's window, the QUIC path pins 1.3 and installs its
    // transport params. `ip_bytes` backs cfg.verify_ip for an IP verify-name
    // and must outlive the config. IoErr::Invalid: verify_peer with no usable
    // check name; NotFound: no system CA bundle on this host.
    [[nodiscard]] static common::IoResult<void> stage(const TlsClientParam &param, tls::TlsClientConfig &cfg,
                                                      std::array<std::uint8_t, 16> &ip_bytes) noexcept;
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_TLS_CLIENT_STAGING_H
