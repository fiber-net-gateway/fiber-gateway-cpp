#ifndef FIBER_TLS_CRYPTO_TLS_SECRET_H
#define FIBER_TLS_CRYPTO_TLS_SECRET_H

// Shared keying-material types for both version schedules (Tls12KeySchedule /
// Tls13KeySchedule). Pure data: transcript hashes are INPUTS to the schedules —
// the handshake context owns the running hash and snapshots it at the right
// moments (feature/tls/02 §3.2). No OpenSSL types appear (the adapter
// src/tls/crypto/TlsCryptoPrimitives.h is the only OpenSSL touchpoint).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../common/IoError.h"

namespace fiber::tls {

class TlsKeySchedule13; // fills TlsSecret via from_bytes; no other writers

// A keying-material value from the schedule (traffic secrets, master secrets,
// PSKs). Capacity 48 covers SHA-384, the largest digest in scope. Move-only:
// moving wipes the source; there is deliberately NO wiping destructor —
// TlsSecret stays trivially destructible and wiping happens at explicit
// handoff points (cipher init consumed the material, engine drops it) and in
// the schedule object's own destructor (02 §8.1).
class TlsSecret {
public:
    static constexpr std::size_t kMaxLen = 48;

    TlsSecret() noexcept = default;
    TlsSecret(const TlsSecret &) = delete;
    TlsSecret &operator=(const TlsSecret &) = delete;
    TlsSecret(TlsSecret &&other) noexcept;
    TlsSecret &operator=(TlsSecret &&other) noexcept;

    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept { return {buf_.data(), len_}; }
    [[nodiscard]] std::uint8_t len() const noexcept { return len_; }
    [[nodiscard]] bool empty() const noexcept { return len_ == 0; }

    // Explicit zeroing (OPENSSL_cleanse); idempotent.
    void wipe() noexcept;

    // Adopts a copy of the given bytes; size > kMaxLen is a contract violation.
    [[nodiscard]] static TlsSecret from_bytes(std::span<const std::uint8_t> src) noexcept;

private:
    friend class TlsKeySchedule13; // fills via from_bytes; no other writers
    std::array<std::uint8_t, kMaxLen> buf_{};
    std::uint8_t len_ = 0;
};

// Cipher-ready material derived from a traffic secret (1.3) or sliced from
// the 1.2 key_block. Plain data: consumed by TlsRecordCipher::init, which
// copies it into the EVP_AEAD_CTX; the caller wipes it at that handoff.
struct TlsTrafficKeys {
    std::array<std::uint8_t, 32> key{};
    std::array<std::uint8_t, 12> iv{}; // 1.3: static iv (12B); 1.2: fixed iv (4 GCM / 12 ChaCha)
    std::uint8_t key_len = 0;
    std::uint8_t iv_len = 0; // 12 (1.3) / 4|12 (1.2 GCM|ChaCha)
};

} // namespace fiber::tls

#endif // FIBER_TLS_CRYPTO_TLS_SECRET_H
