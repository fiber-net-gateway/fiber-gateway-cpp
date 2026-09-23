#ifndef FIBER_TLS_TLS_TICKET_SERVICE_H
#define FIBER_TLS_TLS_TICKET_SERVICE_H

// Stateless self-encrypted session tickets (feature/tls/08 §3) — the server
// side of both NewSessionTicket flavors. The server remembers NOTHING about
// issued tickets: the ticket blob IS the resumption state, sealed under a
// ticket-protection key (AES-128-GCM over the EVP_AEAD primitive layer). The
// future resumption lookup is this class's open() plus the handshake's
// binder/age gates — no "was this issued?" memory exists or is needed, so
// restarts, extra workers, and scale-out all behave identically as long as
// they hold the same key material.
//
// Stateless contract (08 §2, pinned by design):
//   * zero storage — a replayed ticket decrypts as valid every time, so
//     0-RTT replay rejection is NOT provided; enable_early_data stays off
//     unless the deployment explicitly accepts replay (RFC 8446 §8);
//   * the TPK is the PFS boundary — rotate it; 1.3 tickets carry the derived
//     PSK (not the resumption master) and resumed connections still negotiate
//     (EC)DHE, so a leaked TPK cannot decrypt past passively recorded traffic;
//   * expiry is clock-based (issued_ms rides inside the sealed payload) —
//     multi-instance clock drift shifts the acceptance window by the drift.
//
// Key management — INJECTED and IMMUTABLE (08 §5, lock-free): the key set is
// supplied at construction and never changes afterwards, so mint/open are
// pure reads with no internal lock. Concurrent calls are race-free: the
// EVP_AEAD_CTX seal/open entry points take the ctx by const pointer and the
// GCM context holds no mutable call state. Rotation is therefore EXTERNAL —
// the assembly injects [current, retained...] material (config file or a
// persisted seed) and rotates by swapping the service; feeding the SAME
// material to one service per event loop is exact, so loops need not share
// an instance at all. When every key's mint window has passed, minting
// returns 0 — no NST, no resumption, never a wrong-key ticket (the safe
// degradation). Because keys come from outside, a restart with the same
// material keeps every issued ticket openable.
//
// Container (version 1, big-endian throughout):
//   ver(1)=1 || key_id(4) || nonce(12) || ciphertext || tag(16)
//   AAD = ver || key_id || be16(name.len) || name   (name = the CH's SNI)
// Plaintext payload, kind-disjoint by version:
//   1.3: kind(1)=0x13 psk_len(1) psk suite(2) alpn_len(1) alpn
//        age_add(4) max_early_data(4) issued_ms(8) timeout_s(4)
//   1.2: kind(1)=0x12 master_len(1)=48 master(48) suite(2) alpn_len(1) alpn
//        issued_ms(8) timeout_s(4)
// The four longevity elements (version byte, name-in-AAD, key_id, the
// PSK-not-master choice) are frozen: the resumption-side slice must open
// these tickets without a format change once they can persist across
// restarts.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <openssl/aead.h>

#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>

#include "TlsConfig.h"
#include "TlsVersion.h"
#include "crypto/TlsSecret.h"
#include "handshake/TlsCipherSuites.h"

namespace fiber::tls {

// Window policy per key, counted from the material's created_ms:
//   mint window = [created, created + lifetime)
//   open window = [created, created + lifetime + retention)
// Retention must cover the ticket timeout the mint side advertises, else
// tickets die with their key — a safe degradation, not an error.
struct TlsTicketKeyPolicy {
    std::uint32_t key_lifetime_s = 86400; // 24 h as the mint key
    std::uint32_t key_retention_s = 604800; // 7 d opening retired-key tickets
};

// One ticket-protection key: the raw AES-128 key bytes, its identity, and
// its birth time. `id` must be unique within a service (open() addresses
// keys by it). Persist and rotate these OUTSIDE the service (config / seed).
struct TlsTicketKeyMaterial {
    std::uint32_t id = 0;
    std::array<std::uint8_t, 16> bytes{}; // AES-128-GCM key
    std::int64_t created_ms = 0; // the policy windows count from here
};

// What a sealed ticket decodes back into — the future lookup's input. The
// secret is already the resumption PSK (1.3, derived at mint from the
// resumption master + ticket nonce) or the 1.2 master secret.
struct TlsTicketContents {
    TlsProtocolVersion version = TlsProtocolVersion::Tls13;
    TlsSecret secret{}; // move-only; open() fills it
    TlsCipherSuiteId suite = TlsCipherSuiteId::TlsAes128GcmSha256;
    std::array<std::uint8_t, 256> alpn{}; // selected protocol bytes; empty when alpn_len == 0
    std::uint16_t alpn_len = 0;
    std::uint32_t ticket_age_add = 0; // 1.3 only
    std::uint32_t max_early_data = 0; // 1.3 only (0 while enable_early_data is off)
    std::int64_t issued_ms = 0;
    std::uint32_t timeout_s = 0;

    [[nodiscard]] std::string_view alpn_view() const noexcept {
        return {reinterpret_cast<const char *>(alpn.data()), alpn_len};
    }
};

class TlsTicketService final : public common::NonCopyable, public common::NonMovable {
public:
    static constexpr std::size_t kMaxNameLen = 255; // SNI wire bound
    static constexpr std::size_t kMaxTicketLen = 640; // header + tag + worst-case payload
    static constexpr std::size_t kMaxKeys = 8; // more injected: the 8 newest by created_ms

    enum class OpenStatus : std::uint8_t {
        Ok,
        Expired, // key fine, session timed out — full-handshake fallback
        Rejected, // bad format / auth failure / unknown or dropped key / wrong name
    };

    // Initializes every key's AEAD context once; afterwards the service is
    // immutable and mint/open are lock-free. Empty input, duplicate ids, or
    // an init failure leaves valid() false — minter() then declines every
    // mint (0 = no NST this connection, the hook contract's safe path).
    TlsTicketService(std::span<const TlsTicketKeyMaterial> keys, const TlsTicketKeyPolicy &policy) noexcept;
    ~TlsTicketService();

    [[nodiscard]] bool valid() const noexcept { return key_count_ != 0; }

    // Entropy helper for zero-config assembly: one fresh key born at
    // created_ms. False only on entropy failure.
    [[nodiscard]] static bool random_key(std::uint32_t id, std::int64_t created_ms, TlsTicketKeyMaterial &out) noexcept;

    // The TlsTicketMinter adapter the 07 engines take (borrowed: keep the
    // service alive for the engine's lifetime).
    [[nodiscard]] TlsTicketMinter minter() const noexcept;
    static std::size_t mint_thunk(void *ctx, const TlsTicketRequest &req, std::span<std::uint8_t> out) noexcept;

    // The TlsResumptionLookup adapter both 07 version sub-flows take (borrowed
    // like minter()): opens the presented identity against the CH's SNI and
    // the engine clock. Version-blind — the result carries the payload's
    // version and each engine rejects a ticket minted for the other one. A
    // Rejected/Expired open is a miss, so the handshake falls back to a full
    // one. The out spans borrow a thread-local staging cell (the engine reads
    // them right after the call returns; the same thread's next lookup
    // overwrites the cell).
    [[nodiscard]] TlsResumptionLookup lookup() const noexcept;
    static bool lookup_thunk(void *ctx, std::span<const std::uint8_t> identity, std::string_view name,
                             std::int64_t now_unix_ms, TlsResumedSession &out) noexcept;

    // Authenticates + decrypts + freshness-checks a presented ticket.
    // `name` must equal the mint-time SNI (empty == empty). Contents are
    // moved into `out` on Ok only.
    [[nodiscard]] OpenStatus open(std::span<const std::uint8_t> ticket, std::string_view name, std::int64_t now_unix_ms,
                                  TlsTicketContents &out) const noexcept;

    [[nodiscard]] std::size_t key_count() const noexcept; // keys actually kept

private:
    // The ring entry. EVP_AEAD_CTX appears here by value for the same
    // reasons as TlsRecordCipher.h (the accepted exception): a complete
    // type is needed, and zeroed == uninitialized/cleanup-safe.
    struct Key {
        EVP_AEAD_CTX aead{}; // zeroed == uninitialized; cleanup zeroizes
        std::uint32_t id = 0;
        std::int64_t created_at_ms = 0;
        std::int64_t retire_at_ms = 0; // stops minting
        std::int64_t drop_at_ms = 0; // stops opening
    };

    // The mint pick is a pure function of the clock: the NEWEST key that was
    // born and has not retired. Keys are ordered oldest-first, so the scan
    // from the end finds it directly. A not-yet-born key (created in the
    // future relative to `now`) is skipped — injected [current, successor]
    // sets hand over exactly at the successor's birth time.
    [[nodiscard]] const Key *mint_key(std::int64_t now_ms) const noexcept;

    [[nodiscard]] const Key *find_key(std::uint32_t id, std::int64_t now_ms) const noexcept;

    std::array<Key, kMaxKeys> keys_{};
    std::size_t key_count_ = 0; // keys [0, key_count_) are live; 0 == invalid
};

} // namespace fiber::tls

#endif // FIBER_TLS_TLS_TICKET_SERVICE_H
