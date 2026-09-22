#ifndef FIBER_TLS_CRYPTO_TLS_CRYPTO_PRIMITIVES_H
#define FIBER_TLS_CRYPTO_TLS_CRYPTO_PRIMITIVES_H

// The ONE translation-unit-facing file under src/tls/ that includes OpenSSL
// headers (feature/tls/01 §3.2). The crypto key schedule and key exchange
// include only this adapter; OpenSSL types never appear in
// include/fiber/tls/. Public headers keep zero OpenSSL includes (the record
// cipher's EVP_AEAD_CTX member is the one pre-existing exception, 05 §4).
//
// Failure model: a false return is an internal primitive failure (allocation
// or misuse on our side); callers translate it to IoErr after the error queue
// has been drained here. Bad peer data (invalid public key, failed
// authentication) is reported by the callers, not by these wrappers — X25519
// maps its rejected peers to false, which the key exchange layer reads as
// TlsKxStatus::BadPeerData (the scalar is ours, freshly generated, so a
// zero-output can only be the peer's fault).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <fiber/tls/handshake/TlsCipherSuites.h>

namespace fiber::tls {

// Digest size in bytes (32 / 48).
[[nodiscard]] std::size_t tls_hash_len(TlsHashAlgorithm hash) noexcept;

// HKDF (RFC 5869) over SHA-256/SHA-384.
//
// tls_hkdf_extract writes hash_len bytes of PRK to prk_out (capacity
// EVP_MAX_MD_SIZE == 64 asserted). Note the argument order the BoringSSL
// export uses: ikm first, salt second.
[[nodiscard]] bool tls_hkdf_extract(std::span<std::uint8_t> prk_out, TlsHashAlgorithm hash,
                                    std::span<const std::uint8_t> ikm, std::span<const std::uint8_t> salt) noexcept;
// tls_hkdf_expand writes exactly out.size() bytes. prk must be hash_len bytes.
[[nodiscard]] bool tls_hkdf_expand(std::span<std::uint8_t> out, TlsHashAlgorithm hash,
                                   std::span<const std::uint8_t> prk, std::span<const std::uint8_t> info) noexcept;
// Digest of the empty input (Hash("")); writes hash_len bytes. This is the
// TLS 1.3 "empty transcript" context — NOT the same as a zero-length
// HKDF-Expand-Label context. Mirrors BoringSSL's EVP_Digest(nullptr, 0, ...)
// at tls13_advance_key_schedule / tls13_psk_binder.
[[nodiscard]] bool tls_digest_empty(std::span<std::uint8_t> out, TlsHashAlgorithm hash) noexcept;

// Incremental HMAC over SHA-256/SHA-384. Copyable state (the schedule never
// copies, but the shape mirrors the trivially-copyable hash contexts the
// transcript will use).
class TlsHmac {
public:
    TlsHmac() noexcept { HMAC_CTX_init(&ctx_); }
    TlsHmac(const TlsHmac &) = delete;
    TlsHmac &operator=(const TlsHmac &) = delete;
    ~TlsHmac();

    [[nodiscard]] bool init(TlsHashAlgorithm hash, std::span<const std::uint8_t> key) noexcept;
    [[nodiscard]] bool update(std::span<const std::uint8_t> data) noexcept;
    // Writes hash_len bytes; out must have that capacity.
    [[nodiscard]] bool final(std::span<std::uint8_t> out) noexcept;

private:
    HMAC_CTX ctx_;
};

// Incremental SHA-256/SHA-384 — the running-hash primitive behind the
// handshake transcript. The OpenSSL SHA contexts are pointer-free PODs, so
// this class is trivially copyable: forking a transcript = copying the state
// (TLS 1.3 post-handshake CertificateVerify forks the transcript this way).
// final() writes hash_len bytes; out must have that capacity.
class TlsHash {
public:
    TlsHash() noexcept = default;
    TlsHash(const TlsHash &) noexcept = default;
    TlsHash &operator=(const TlsHash &) noexcept = default;
    ~TlsHash() = default;

    [[nodiscard]] bool init(TlsHashAlgorithm hash) noexcept;
    [[nodiscard]] bool update(std::span<const std::uint8_t> data) noexcept;
    [[nodiscard]] bool final(std::span<std::uint8_t> out) noexcept;

private:
    TlsHashAlgorithm hash_ = TlsHashAlgorithm::Sha256;
    bool inited_ = false;
    union {
        SHA256_CTX sha256;
        SHA512_CTX sha384;
    } ctx_{};
};

// CSPRNG. Never silently degrades: a false return is a failed handshake.
[[nodiscard]] bool tls_random_bytes(std::span<std::uint8_t> out) noexcept;

// Zeroing the compiler cannot elide.
void tls_secure_wipe(void *ptr, std::size_t len) noexcept;
// Constant-time equality; spans of different length are simply unequal.
[[nodiscard]] bool tls_constant_time_equal(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) noexcept;

// ---- X25519: raw 32-byte scalars, no pimpl needed (curve25519.h) ----

void tls_x25519_keypair(std::uint8_t out_public[32], std::uint8_t out_private[32]) noexcept;
// False = peer public key rejected (small-order / all-zero shared secret);
// out is zeroed by the library in that case.
[[nodiscard]] bool tls_x25519_shared(std::uint8_t out_shared[32], const std::uint8_t private_key[32],
                                     const std::uint8_t peer_public[32]) noexcept;

// ---- P-256: EVP_PKEY is opaque in the public headers, so the key lives on
// the heap behind a void* handle. Public values are uncompressed points
// (0x04 || X || Y, 65 bytes); the shared secret is the fixed-length 32-byte
// x-coordinate (leading zeros kept). ----

struct TlsP256Key {
    void *impl = nullptr; // EVP_PKEY*, owned by whoever holds this handle
};

void tls_p256_free(TlsP256Key &key) noexcept;
[[nodiscard]] bool tls_p256_generate(TlsP256Key &key) noexcept;
[[nodiscard]] bool tls_p256_public(const TlsP256Key &key, std::uint8_t out_uncompressed[65]) noexcept;
// peer must be exactly 65 bytes. False = invalid point (not on the curve) or
// derive failure; invalid encodings are the caller's protocol error to raise.
[[nodiscard]] bool tls_p256_shared(const TlsP256Key &key, std::span<const std::uint8_t> peer_uncompressed,
                                   std::uint8_t out_shared[32]) noexcept;

} // namespace fiber::tls

#endif // FIBER_TLS_CRYPTO_TLS_CRYPTO_PRIMITIVES_H
