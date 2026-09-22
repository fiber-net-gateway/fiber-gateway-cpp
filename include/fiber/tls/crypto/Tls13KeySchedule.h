#ifndef FIBER_TLS_CRYPTO_TLS13_KEY_SCHEDULE_H
#define FIBER_TLS_CRYPTO_TLS13_KEY_SCHEDULE_H

// TLS 1.3 key schedule: the RFC 8446 §7.1 HKDF tree and the cipher-ready
// traffic keys it derives (structure mirrors BoringSSL's ssl/tls13_enc.cc).
// Pure keying-material logic: transcript hashes are INPUTS here — the
// handshake context owns the running hash and snapshots it at the right
// moments (feature/tls/02 §3.2). No OpenSSL types appear (the adapter
// src/tls/crypto/TlsCryptoPrimitives.h is the only OpenSSL touchpoint).

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../handshake/TlsCipherSuites.h"
#include "TlsSecret.h"

namespace fiber::tls {

enum class TlsPskBinderKind : std::uint8_t { External, Resumption };

// TLS 1.3 key schedule as a one-shot staged machine. Role-symmetric: client
// and server run the IDENTICAL call sequence (02 §3.2) — only which secret
// feeds the write-side cipher flips with the role, which the engine expresses
// when it picks secrets. Stage ordering and call-once are contract invariants
// (FIBER_ASSERT): a violation is an engine bug, not a protocol failure.
//
// Transcript hash arguments must be exactly hash_len bytes (the suite's
// digest size) — asserted. Zero allocation; all secrets are fixed-size PODs.
// HRR does NOT reuse an instance: the transcript restarts, so the engine
// destroys and reconstructs the schedule.
class TlsKeySchedule13 : public common::NonCopyable, public common::NonMovable {
public:
    // Asserts a TLS 1.3 suite; derives the no-PSK Early Secret
    // Extract(zeros(hash_len), zeros(hash_len)) — the diagram's "" is a
    // zeros(hash_len) IKM, not an empty span (RFC 8448 §3).
    explicit TlsKeySchedule13(TlsCipherSuiteId suite) noexcept;
    // Wipes all internal secrets (deterministic member cleansing).
    ~TlsKeySchedule13();

    // Re-derives the Early Secret from the PSK. Must run before any secret is
    // derived (PSK changes the entire tree). Optional: without it the Early
    // Secret is the no-PSK value; an empty psk normalizes to zeros(hash_len)
    // and reproduces exactly that.
    [[nodiscard]] common::IoResult<void> set_psk(std::span<const std::uint8_t> psk) noexcept;

    // ---- stage 0 outputs (Early Secret); each at most once ----

    // Requires set_psk (binders only exist in PSK flows). Context is
    // Transcript-Hash("") — Hash of the empty message list, a full-length
    // digest — per RFC 8448 §4 (binder key 69 fe 13 1a ...).
    [[nodiscard]] common::IoResult<TlsSecret> binder_key(TlsPskBinderKind kind) noexcept;
    // 0-RTT read/write secret; input = Hash(ClientHello).
    [[nodiscard]] common::IoResult<TlsSecret>
    client_early_traffic_secret(std::span<const std::uint8_t> hash_client_hello) noexcept;

    // ---- stage transitions ----

    // z: the 32-byte (EC)DHE shared secret (X25519 or P-256 x-coordinate).
    // Input hash = Hash(CH..SH). Advances to the handshake stage.
    [[nodiscard]] common::IoResult<void> handshake_secrets(std::span<const std::uint8_t> z,
                                                           std::span<const std::uint8_t> hash_ch_sh,
                                                           TlsSecret &client_hs, TlsSecret &server_hs) noexcept;
    // Input hash = Hash(CH..server Finished). Advances to the application
    // stage. server_app0 feeds the ConnectedState (KeyUpdate base).
    [[nodiscard]] common::IoResult<void> application_secrets(std::span<const std::uint8_t> hash_ch_server_fin,
                                                             TlsSecret &client_app0, TlsSecret &server_app0) noexcept;
    // Input hash = Hash(CH..client Finished); at most once, application stage.
    [[nodiscard]] common::IoResult<TlsSecret>
    resumption_master_secret(std::span<const std::uint8_t> hash_ch_client_fin) noexcept;

    [[nodiscard]] TlsCipherSuiteId suite() const noexcept { return suite_; }
    [[nodiscard]] TlsHashAlgorithm hash() const noexcept { return hash_; }

private:
    enum class Stage : std::uint8_t { Early, Handshake, Application };

    TlsCipherSuiteId suite_;
    TlsHashAlgorithm hash_;
    std::uint8_t hash_len_;
    Stage stage_ = Stage::Early;
    bool psk_set_ = false;
    bool binder_done_ = false;
    bool early_done_ = false;
    bool resumption_done_ = false;
    TlsSecret early_secret_;
    TlsSecret handshake_secret_;
    TlsSecret master_secret_;
};

// ---- free functions ----

// key = Expand-Label(secret, "key", "", key_len); iv = (…, "iv", "", 12).
// Asserts a 1.3 suite.
[[nodiscard]] common::IoResult<TlsTrafficKeys> tls13_traffic_keys(const TlsSecret &secret,
                                                                  TlsCipherSuiteId suite) noexcept;
// KeyUpdate: secret_N+1 = Expand-Label(secret_N, "traffic upd", "", hash_len)
// — empty context, verified against BoringSSL's tls13_rotate_traffic_key
// (02 §3.5). The secret's length carries the suite hash.
[[nodiscard]] common::IoResult<TlsSecret> tls13_key_update(const TlsSecret &current) noexcept;
// Resumption PSK: Expand-Label(resumption_master, "resumption", nonce) — the
// NST-to-PSK derivation the 08 session layer consumes. The master secret's
// length carries the suite hash; the ticket nonce is the HKDF context.
[[nodiscard]] common::IoResult<TlsSecret> tls13_resumption_psk(const TlsSecret &resumption_master,
                                                               std::span<const std::uint8_t> nonce) noexcept;
// Finished MAC: HMAC(Expand-Label(traffic_secret, "finished", "", hash_len),
// transcript_hash). out must have hash_len capacity (= secret.len()).
[[nodiscard]] common::IoResult<void> tls13_finished_mac(const TlsSecret &traffic_secret,
                                                        std::span<const std::uint8_t> transcript_hash,
                                                        std::span<std::uint8_t> out_mac) noexcept;
// PSK binder — same construction as Finished (RFC 8446 §4.2.11.2): the
// binder_key is first expanded into a finished key, then MACed over
// Hash(truncated ClientHello). out must have binder_key.len() capacity.
[[nodiscard]] common::IoResult<void> tls13_psk_binder_mac(const TlsSecret &binder_key,
                                                          std::span<const std::uint8_t> truncated_ch_hash,
                                                          std::span<std::uint8_t> out_mac) noexcept;

} // namespace fiber::tls

#endif // FIBER_TLS_CRYPTO_TLS13_KEY_SCHEDULE_H
