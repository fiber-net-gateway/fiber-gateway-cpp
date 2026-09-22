#ifndef FIBER_TLS_TLS_CONFIG_H
#define FIBER_TLS_TLS_CONFIG_H

// Client handshake inputs (06 §3). 07 adds the server shape here. The 01
// §3.2 plan for TlsCredentials.h / TlsTrustAnchors.h was dropped: the 02b
// TlsCertificateChain / TlsPrivateKey / TlsTrustStore types ARE the
// SSL-free credential layer and this config consumes them directly (06 §5.4).

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "crypto/TlsCertificate.h"
#include "crypto/TlsSignature.h"
#include "handshake/TlsCipherSuites.h"

namespace fiber::tls {

// All-borrowed views that must outlive the engine (the net glue holds the
// config). Immutable by contract; groups/suites preferences,
// record_size_limit and max_early_data policy stay engine-internal registry
// constants until 09 asks for them (06 §5.4 YAGNI note).
struct TlsClientConfig {
    std::string_view sni_host; // SNI send name + certificate check name (SAN-only, 02b semantics)
    std::span<const std::uint8_t> verify_ip; // optional: IP check in place of host; when set, no SNI is sent
    std::span<const std::string_view> alpn; // offered protocols, order = preference; empty = no ALPN
    const TlsCertificateChain *client_chain = nullptr; // mTLS, optional
    const TlsPrivateKey *client_key = nullptr; // mTLS, paired with the chain
    const TlsTrustStore *trust = nullptr; // trust anchors (02b), required
    bool verify_peer = true; // false is loopback-test only (lite_nginx parity)
    std::int64_t now_unix_ms = 0; // certificate validity snapshot; the engine has no clock
};

// Resumption attempt: a borrowed projection of 08's future TlsSessionState
// (06 §1.2). No ownership — the caller keeps the ticket/PSK bytes alive for
// the handshake's duration.
struct TlsSessionOffer {
    std::span<const std::uint8_t> identity; // ticket bytes
    std::uint32_t obfuscated_ticket_age = 0; // computed by 08 (age_add folded in); passed through
    TlsCipherSuiteId suite = TlsCipherSuiteId::TlsAes128GcmSha256; // binds the binder + transcript hash
    std::span<const std::uint8_t> psk; // resumption PSK bytes (08 derives them from the NST)
    std::size_t max_early_data = 0; // 0 = no early_data extension
};

} // namespace fiber::tls

#endif // FIBER_TLS_TLS_CONFIG_H
