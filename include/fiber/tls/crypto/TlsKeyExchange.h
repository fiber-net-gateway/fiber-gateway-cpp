#ifndef FIBER_TLS_CRYPTO_TLS_KEY_EXCHANGE_H
#define FIBER_TLS_CRYPTO_TLS_KEY_EXCHANGE_H

// (EC)DHE ephemeral key exchange for the TLS handshake: X25519 (preferred)
// and P-256. One instance per handshake; the server keeps its instance across
// a HelloRetryRequest (its key_share stays valid), the client discards and
// rebuilds on HRR. The server's long-term signing key is NOT this type — it
// belongs to TlsSignature's credential material (02 §5).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../handshake/TlsCipherSuites.h"

namespace fiber::tls {

// Wire key_share public value: X25519 = 32 raw bytes; P-256 = 65 bytes
// uncompressed (0x04 || X || Y). Fixed capacity + live length.
struct TlsKeySharePub {
    std::array<std::uint8_t, 65> buf{};
    std::uint8_t len = 0;

    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept { return {buf.data(), len}; }
};

enum class TlsKxStatus : std::uint8_t {
    Ok,
    BadPeerData, // invalid peer public value -> engine maps illegal_parameter/decode_error
    PrimitiveFail, // crypto-library failure on our side -> internal error
};

// Shared secret is always the fixed-length 32-byte value the key schedule
// consumes (X25519 output; P-256 x-coordinate with leading zeros kept).
struct TlsKxShared {
    TlsKxStatus status = TlsKxStatus::Ok;
    std::array<std::uint8_t, 32> z{};
};

class TlsKeyExchange : public common::NonCopyable, common::NonMovable {
public:
    // Asserts group is X25519 or Secp256r1 — the negotiated set; anything
    // else is an engine bug (FFDHE is out of scope, negotiation skips it).
    explicit TlsKeyExchange(TlsNamedGroup group) noexcept;
    ~TlsKeyExchange(); // wipes private material

    // Generates a fresh ephemeral keypair. X25519 cannot fail; P-256 fails
    // only on allocation.
    [[nodiscard]] common::IoResult<void> generate() noexcept;

    [[nodiscard]] const TlsKeySharePub &public_value() const noexcept; // asserts generated

    // Derives the 32-byte shared secret from the peer's public value.
    // Length/encoding violations and off-curve points are BadPeerData.
    [[nodiscard]] TlsKxShared shared_secret(std::span<const std::uint8_t> peer_public) const noexcept;

    // Explicit cleanup; also run by the destructor.
    void wipe() noexcept;

private:
    TlsNamedGroup group_;
    std::array<std::uint8_t, 32> x25519_priv_{}; // X25519: private scalar in place, no pimpl
    TlsKeySharePub pub_{};
    void *p256_ = nullptr; // EVP_PKEY* (P-256), hidden behind the adapter
    bool generated_ = false;
};

} // namespace fiber::tls

#endif // FIBER_TLS_CRYPTO_TLS_KEY_EXCHANGE_H
