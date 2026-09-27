#ifndef FIBER_TLS_CRYPTO_TLS12_KEY_SCHEDULE_H
#define FIBER_TLS_CRYPTO_TLS12_KEY_SCHEDULE_H

// TLS 1.2 (RFC 5246) PRF and keying material: master secret (plain and
// extended), the key block, and verify_data (structure mirrors BoringSSL's
// ssl/t1_enc.cc). Free functions — the handshake flow itself guarantees
// ordering, no staged machine needed. Pure keying-material logic: the
// handshake hash inputs are snapshots owned by the handshake context
// (feature/tls/02 §4). No OpenSSL types appear (the adapter
// src/tls/crypto/TlsCryptoPrimitives.h is the only OpenSSL touchpoint).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../common/IoError.h"
#include "../handshake/TlsCipherSuites.h"
#include "TlsSecret.h"

namespace fiber::tls {

// master_secret = PRF(z, "master secret", client_random || server_random)[48].
// z is the ECDHE shared secret (32 bytes; 48 for P-384); both randoms are 32 bytes. Asserts a
// 1.2 suite.
[[nodiscard]] common::IoResult<TlsSecret> tls12_master_secret(TlsCipherSuiteId suite, std::span<const std::uint8_t> z,
                                                              std::span<const std::uint8_t> client_random,
                                                              std::span<const std::uint8_t> server_random) noexcept;

// Extended master secret (RFC 7627 §4): when the peer echoed the
// extended_master_secret extension, master_secret =
// PRF(z, "extended master secret", session_hash)[48] instead. session_hash is
// the suite-hash snapshot of the handshake_messages buffer through
// ServerKeyExchange inclusive (NOT the client flight); its length is the
// suite hash length (32/48). Asserts a 1.2 suite.
[[nodiscard]] common::IoResult<TlsSecret>
tls12_extended_master_secret(TlsCipherSuiteId suite, std::span<const std::uint8_t> z,
                             std::span<const std::uint8_t> session_hash) noexcept;

// Per-direction write material sliced from
// PRF(master, "key expansion", server_random || client_random)
// (seed order deliberately REVERSED vs the master secret), in RFC 5246 §6.3
// order: client_mac || server_mac || client_key || server_key ||
// client_fixed_iv || server_fixed_iv. The MAC keys exist only for CBC suites
// (empty for AEADs) and ride in front of each direction's key (the CBC
// record cipher takes MAC key || encryption key); the fixed IV length is
// suite-shaped — 4 (RFC 5288 GCM), 12 (RFC 7905 §2 ChaCha20, whose whole
// nonce is implicit) or 0 (CBC, whose IV is explicit per record).
struct Tls12WriteKeys {
    TlsTrafficKeys client;
    TlsTrafficKeys server;
};

[[nodiscard]] common::IoResult<Tls12WriteKeys> tls12_key_block(TlsCipherSuiteId suite, const TlsSecret &master,
                                                               std::span<const std::uint8_t> client_random,
                                                               std::span<const std::uint8_t> server_random) noexcept;

// verify_data = PRF(master, "client finished"/"server finished",
// handshake_hash)[12]. The handshake hash is the suite-hash snapshot of the
// 1.2 handshake_messages buffer.
[[nodiscard]] common::IoResult<std::array<std::uint8_t, 12>>
tls12_verify_data(TlsCipherSuiteId suite, const TlsSecret &master, bool client,
                  std::span<const std::uint8_t> handshake_hash) noexcept;

} // namespace fiber::tls

#endif // FIBER_TLS_CRYPTO_TLS12_KEY_SCHEDULE_H
