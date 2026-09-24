#ifndef FIBER_TLS_TLS_CONFIG_H
#define FIBER_TLS_TLS_CONFIG_H

// Client handshake inputs (06 §3). 07 adds the server shape here. The 01
// §3.2 plan for TlsCredentials.h / TlsTrustAnchors.h was dropped: the 02b
// TlsCertificateChain / TlsPrivateKey / TlsTrustStore types ARE the
// SSL-free credential layer and this config consumes them directly (06 §5.4).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "TlsTypes.h"
#include "TlsVersion.h"
#include "crypto/TlsCertificate.h"
#include "crypto/TlsSecret.h"
#include "crypto/TlsSignature.h"
#include "handshake/TlsCipherSuites.h"
#include "handshake/TlsHandshakeMessage.h"

namespace fiber::tls {

// ---- QUIC mode (feature/tls/10 §3): the transport callbacks ----

// Mirrors BoringSSL's ssl_encryption_level_t (RFC 9001 §4.1.1) across the two
// callbacks that carry a level: add_handshake_data never fires EarlyData
// (CRYPTO frames exist at Initial/Handshake/Application only) and set_secret
// never fires Initial (Initial secrets are QUIC-local, derived in
// src/quic/QuicCrypto — the engine never sees them).
enum class TlsQuicLevel : std::uint8_t {
    Initial, // outbound CRYPTO only: CH (client) / HRR (server)
    EarlyData, // set_secret only: client_early_traffic_secret
    Handshake, // SH..Fin CRYPTO; hs traffic secrets
    Application, // NST CRYPTO; app traffic secrets
};

// The QUIC transport contract (10 §3.1). A non-null `quic` field on the
// config switches the engine into QUIC mode for the WHOLE handshake: records
// are never framed (inbound = raw CRYPTO-stream handshake messages, outbound
// = per-message add_handshake_data), no compat CCS is sent, fatal alerts are
// reported (not encoded — the QUIC layer translates to a CONNECTION_CLOSE
// 0x0100|desc), and no record cipher is ever constructed — every secret
// derivation point calls set_secret instead. All four pointers must be set
// when the struct is used (enable_quic asserts it). Raw function pointers,
// no std::function — null quic on the config is the TCP path, unchanged.
struct TlsQuicCallbacks {
    // Secret export at each derivation point (10 §4). `secret` is a
    // short-lived span into engine scratch — copy before returning. false =
    // installation failure (the engine fails the handshake, internal_error).
    bool (*set_secret)(void *ctx, TlsQuicLevel level, bool write_secret, TlsCipherSuiteId suite,
                       std::span<const std::uint8_t> secret) noexcept = nullptr;
    // Outbound handshake bytes (one fully-encoded TLS message per call) at
    // the engine's current outbound level — QUIC frames them as CRYPTO.
    // false = buffer failure (the engine fails the handshake).
    bool (*add_handshake_data)(void *ctx, TlsQuicLevel level, std::span<const std::uint8_t> data) noexcept = nullptr;
    // The peer's quic_transport_parameters (extension 0x39), extracted from
    // the CH (server) / EE (client) as soon as decoded. `params` borrows the
    // inbound stream — copy before returning. Not re-validated by the engine.
    void (*on_peer_transport_params)(void *ctx, std::span<const std::uint8_t> params) noexcept = nullptr;
    // Fatal alert report (nothing is encoded outbound in QUIC mode).
    void (*send_alert)(void *ctx, TlsAlertDesc desc) noexcept = nullptr;
    void *ctx = nullptr;
};

// All-borrowed views that must outlive the engine (the net glue holds the
// config). Immutable by contract; groups/suites preferences,
// record_size_limit and max_early_data policy stay engine-internal registry
// constants until 09 asks for them (06 §5.4 YAGNI note).
struct TlsClientConfig {
    std::string_view sni_host; // SNI send name + default certificate check name (SAN-only, 02b semantics)
    std::span<const std::uint8_t> verify_ip; // optional: IP check in place of host; when set, no SNI is sent
    // Certificate check name when it differs from the SNI send name (09 §4.3:
    // the net layer's server_name/verify_name split). Empty = sni_host.
    std::string_view check_host;
    std::span<const std::string_view> alpn; // offered protocols, order = preference; empty = no ALPN
    const TlsCertificateChain *client_chain = nullptr; // mTLS, optional
    const TlsPrivateKey *client_key = nullptr; // mTLS, paired with the chain
    const TlsTrustStore *trust = nullptr; // trust anchors (02b), required
    bool verify_peer = true; // false is loopback-test only (lite_nginx parity)
    std::int64_t now_unix_ms = 0; // certificate validity snapshot; the engine has no clock
    // Version bounds (09 §4.2). The offered supported_versions list narrows to
    // [min, max] (domain {1.2, 1.3} — 1.3 is the implementation ceiling); a
    // ServerHello negotiating outside the window is fatal protocol_version.
    std::uint16_t min_version = kTlsVersionTls12;
    std::uint16_t max_version = kTlsVersionTls13;
    // QUIC mode (10 §3): non-null switches the engine per TlsQuicCallbacks.
    // The glue pins min_version = max_version = 1.3 (QUIC is TLS 1.3 only —
    // the engine asserts the window) and stages the 0x39 payload somewhere
    // that outlives the engine (borrowed here, injected into the CH).
    const TlsQuicCallbacks *quic = nullptr;
    std::span<const std::uint8_t> quic_transport_params; // non-empty (in QUIC mode) => CH extension 0x39
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

// An owning session receipt assembled from a received NewSessionTicket
// (10 §6.2): everything a later connection needs to offer resumption. The
// QUIC layer's session cache stores these (the engine never caches). The
// vector/array owning shape is deliberate — this is a cold-path cache value,
// not a per-request allocation (IoBuf is wrong for long-lived storage).
struct TlsSessionState {
    std::vector<std::uint8_t> identity; // the opaque ticket blob
    TlsSecret psk; // tls13_resumption_psk(master, nonce) at receipt time
    TlsCipherSuiteId suite = TlsCipherSuiteId::TlsAes128GcmSha256; // the handshake's suite
    std::uint32_t ticket_age_add = 0; // the NST's obfuscation key
    std::uint32_t ticket_lifetime_s = 0; // expiry policy input for the cache owner
    std::uint32_t max_early_data = 0; // the NST's value (0xffffffff sentinel in QUIC)
    std::array<std::uint8_t, 256> alpn{}; // the negotiated protocol the ticket is bound to
    std::uint16_t alpn_len = 0;
    std::int64_t issued_ms = 0; // wall-clock receipt time (age arithmetic base)

    [[nodiscard]] bool empty() const noexcept { return identity.empty() || psk.empty(); }

    // RFC 8446 §4.6.1 ticket age: (now - issued) + ticket_age_add, mod 2^32.
    // A clock reading before issue time counts as zero elapsed age.
    [[nodiscard]] std::uint32_t obfuscated_ticket_age(std::int64_t now_unix_ms) const noexcept {
        const std::int64_t elapsed = now_unix_ms > issued_ms ? now_unix_ms - issued_ms : 0;
        return static_cast<std::uint32_t>(elapsed) + ticket_age_add;
    }
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
    // Version bounds (09 §4.2): the ClientHello fork gates on [min, max]
    // (domain {1.2, 1.3}); no offered version inside the window →
    // protocol_version.
    std::uint16_t min_version = kTlsVersionTls12;
    std::uint16_t max_version = kTlsVersionTls13;
    // QUIC mode (10 §3): as on TlsClientConfig. The 0x39 payload rides the
    // EncryptedExtensions. A per-ClientHello selector must not change the
    // quic pointer (the engine asserts it — the context binds at ctor).
    const TlsQuicCallbacks *quic = nullptr;
    std::span<const std::uint8_t> quic_transport_params; // non-empty (in QUIC mode) => EE extension 0x39
    // The server's 0-RTT-relevant transport params prefix (10 §6.1) — the
    // leading slice of quic_transport_params the codec marks as governing
    // early data (BoringSSL SSL_set_quic_early_data_context's context). The
    // engine seals it into minted tickets and hands the current value to the
    // resumption lookup; the implementer's comparison is the 0-RTT
    // consistency gate. Empty on the TCP face (the gate is QUIC-only).
    std::span<const std::uint8_t> quic_early_data_context;
};

// Per-ClientHello config selection (09 §4.1): the fork calls `select` right
// after decoding the ClientHello (spans borrow the engine's retained copy —
// valid for the call). Return the config that drives THIS connection (the
// pointer is read immediately and copied; it may point at caller staging),
// or null when the ClientHello selects none — an SNI the host does not
// serve — which answers handshake_failure.
struct TlsServerConfigSource {
    const TlsServerConfig *(*select)(void *ctx, const TlsClientHello &client_hello) noexcept = nullptr;
    void *ctx = nullptr;
};

// Resumption lookup hook (08 boundary): identity → a borrowed projection of
// the decrypted resumption parameters. 08's real implementation does ticket
// decryption + anti-replay; the test side uses an in-memory table. Returning
// false = miss (the handshake continues as a full one). The hook serves BOTH
// version sub-flows and is version-blind: `version` in the result says which
// payload came out, and each engine rejects a ticket minted for the other
// version (miss → full handshake).
struct TlsResumedSession { // all-borrowed views; the lookup caller owns the bytes
    std::span<const std::uint8_t> psk; // 1.3: the resumption PSK; 1.2: the 48-byte master secret
    TlsProtocolVersion version = TlsProtocolVersion::Tls13;
    TlsCipherSuiteId suite = TlsCipherSuiteId::TlsAes128GcmSha256; // binds the binder + transcript hash
    std::string_view alpn; // the ticket's early_alpn (empty = none)
    std::uint32_t ticket_age_add = 0; // 1.3 only
    std::uint32_t max_early_data = 0; // 0 = this ticket allows no 0-RTT (1.3 only)
    std::int64_t ticket_issued_ms = 0; // 1.3: the age-window check; 1.2: expiry only (in open)
    bool quic = false; // the ticket's mint face; the engine rejects the other face as a miss
                       // (BoringSSL ssl_session_is_resumable's is_quic match, 10 §6.1)
};
struct TlsResumptionLookup {
    // name = the CH's SNI (the stateless ticket binds it into its AAD),
    // now_unix_ms = the engine's clock for the expiry check — the same
    // snapshot the age gate uses. quic_early_data_context = the server's
    // current 0-RTT-relevant transport params prefix (10 §6.1); empty on the
    // TCP face. The stateless implementer compares it against the context
    // sealed at mint — a mismatch vetoes only the ticket's early data, the
    // session still resumes. The out spans borrow the hook's storage and
    // must stay valid until the caller has consumed them (the engine reads
    // them right after the call returns).
    bool (*lookup)(void *ctx, std::span<const std::uint8_t> identity, std::string_view name, std::int64_t now_unix_ms,
                   std::span<const std::uint8_t> quic_early_data_context, TlsResumedSession &out) noexcept = nullptr;
    void *ctx = nullptr;
};

// Ticket minting hook (08 boundary): write one opaque ticket into out and
// return its length; 0 = send no NST this connection. The stateless
// implementation is TlsTicketService (08 §3): the ticket blob is the AEAD-
// sealed resumption state itself — the server stores nothing about issued
// tickets. Test-side minters serialize the request or file it in a table.
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
    bool quic = false; // the mint face — sealed so the other face's lookup rejects it
    std::span<const std::uint8_t> quic_early_data_context; // the 0-RTT consistency gate's mint-time binding (10 §6.1)
    TlsProtocolVersion version = TlsProtocolVersion::Tls13; // selects the payload field set
    std::string_view name; // the CH's SNI — bound into the ticket AAD (cross-vhost replay guard)
};
struct TlsTicketMinter {
    std::size_t (*mint)(void *ctx, const TlsTicketRequest &, std::span<std::uint8_t> out) noexcept = nullptr;
    void *ctx = nullptr;
};

} // namespace fiber::tls

#endif // FIBER_TLS_TLS_CONFIG_H
