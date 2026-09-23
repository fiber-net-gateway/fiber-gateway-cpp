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

// ---- server handshake inputs (07 §3.2) ----

// All-borrowed views that must outlive the engine (the net glue holds the
// config). Immutable by contract; suite/group/sigalg preferences stay
// engine-fixed registry constants (§10.2: server preference order, not
// config).
struct TlsServerConfig {
    const TlsCertificateChain *chain = nullptr; // required (no PSK-only mode)
    const TlsPrivateKey *key = nullptr; // paired with chain (02b)
    std::span<const std::string_view> alpn; // supported protocols, order = preference; empty = ignore ALPN
    // mTLS: client_trust == nullptr does not request a client certificate;
    // require_client_cert decides whether an empty chain is fatal.
    const TlsTrustStore *client_trust = nullptr;
    bool require_client_cert = false;
    std::int64_t now_unix_ms = 0; // certificate validity + ticket-age snapshot; the engine has no clock
    bool enable_early_data = false; // 0-RTT master switch (off until anti-replay exists, §10.6)
    std::uint32_t session_timeout_s = 7200; // NST lifetime
};

// Resumption lookup hook (08 boundary): identity → a borrowed projection of
// the decrypted resumption parameters. 08's real implementation does ticket
// decryption + anti-replay; the test side uses an in-memory table. Returning
// false = miss (the handshake continues as a full one).
struct TlsResumedSession { // all-borrowed views; the lookup caller owns the bytes
    std::span<const std::uint8_t> psk; // resumption PSK (08 derives it from the NST)
    TlsCipherSuiteId suite = TlsCipherSuiteId::TlsAes128GcmSha256; // binds the binder + transcript hash
    std::string_view alpn; // the ticket's early_alpn (empty = none)
    std::uint32_t ticket_age_add = 0;
    std::uint32_t max_early_data = 0; // 0 = this ticket allows no 0-RTT
    std::int64_t ticket_issued_ms = 0; // for the age-window check (60 s skew, §10.4)
};
struct TlsResumptionLookup {
    bool (*lookup)(void *ctx, std::span<const std::uint8_t> identity, TlsResumedSession &out) noexcept = nullptr;
    void *ctx = nullptr;
};

// Ticket minting hook (08 boundary): write one opaque ticket into out and
// return its length; 0 = send no NST this connection. 08's real
// implementation does AEAD encryption; the test side serializes the
// TlsTicketRequest into the ticket bytes (a ticket is an opaque blob to the
// client — interop does not constrain the format).
struct TlsTicketRequest {
    std::span<const std::uint8_t> resumption_master; // this connection's resumption secret
    std::uint8_t ticket_nonce = 0; // = the sequence number (1.3; §10.9: always one ticket, nonce 0)
    TlsCipherSuiteId suite = TlsCipherSuiteId::TlsAes128GcmSha256;
    std::string_view alpn; // the negotiated protocol (early_alpn semantics)
    std::uint32_t ticket_age_add = 0; // the NST's obfuscation key — bind it into the ticket so a
                                      // later lookup can reproduce the age arithmetic
    std::uint32_t max_early_data = 0; // enable_early_data ? 14336 : 0
    std::uint32_t timeout_s = 0;
    std::int64_t now_unix_ms = 0;
};
struct TlsTicketMinter {
    std::size_t (*mint)(void *ctx, const TlsTicketRequest &, std::span<std::uint8_t> out) noexcept = nullptr;
    void *ctx = nullptr;
};

} // namespace fiber::tls

#endif // FIBER_TLS_TLS_CONFIG_H
