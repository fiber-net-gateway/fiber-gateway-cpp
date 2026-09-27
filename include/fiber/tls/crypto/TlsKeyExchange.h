#ifndef FIBER_TLS_CRYPTO_TLS_KEY_EXCHANGE_H
#define FIBER_TLS_CRYPTO_TLS_KEY_EXCHANGE_H

// (EC)DHE / KEM key exchange for the TLS handshake. Abstract base: one
// subclass per group (src side), constructed via the create() factory —
// structure mirrors BoringSSL's SSLKeyShare (ssl/ssl_key_share.cc). The API
// follows its KEM-unified shape (Generate/Encap/Decap): classical DH groups
// implement encap as the composite "keygen + shared secret" and decap as pure
// shared-secret recovery; PQ groups (future) genuinely fork the two. One
// instance per handshake; the client discards and rebuilds across a
// HelloRetryRequest / a 1.2 SKE group switch. The server's long-term signing
// key is NOT this type — it belongs to TlsSignature's credential material
// (02 §5).

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../handshake/TlsCipherSuites.h"

namespace fiber::tls {

// Wire key_share public value: X25519 = 32 raw bytes; P-256 / P-384 = 65 /
// 97 bytes uncompressed (0x04 || X || Y). Fixed capacity + live length.
struct TlsKeySharePub {
    std::array<std::uint8_t, 97> buf{};
    std::uint8_t len = 0;

    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept { return {buf.data(), len}; }
};

enum class TlsKxStatus : std::uint8_t {
    Ok,
    BadPeerData, // invalid peer public value -> engine maps illegal_parameter/decode_error
    PrimitiveFail, // crypto-library failure on our side -> internal error
};

// Shared secret: the group's fixed-length value the key schedule consumes —
// 32 bytes for X25519 and P-256, 48 for P-384 (the x-coordinate with leading
// zeros kept). Fixed capacity + live length.
struct TlsKxShared {
    TlsKxStatus status = TlsKxStatus::Ok;
    std::array<std::uint8_t, 48> z{};
    std::uint8_t len = 0;

    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept { return {z.data(), len}; }
};

class TlsKeyExchange : public common::NonCopyable, common::NonMovable {
public:
    // Factory. A group outside {X25519, Secp256r1, Secp384r1} is an engine bug
    // (FIBER_ASSERT — the negotiated set; FFDHE is out of scope, negotiation
    // skips it); allocation failure returns NoMem. The returned instance has
    // NOT yet run generate().
    [[nodiscard]] static common::IoResult<std::unique_ptr<TlsKeyExchange>> create(TlsNamedGroup group) noexcept;

    virtual ~TlsKeyExchange(); // wipes private material

    [[nodiscard]] virtual TlsNamedGroup group() const noexcept = 0;

    // Client CH: generates a fresh ephemeral keypair; public_value() becomes
    // available afterwards. X25519 cannot fail; P-256/P-384 fail only on
    // allocation.
    [[nodiscard]] virtual common::IoResult<void> generate() noexcept = 0;
    [[nodiscard]] virtual const TlsKeySharePub &public_value() const noexcept = 0; // asserts generated

    // Server side (consumed by 07): one step from the peer's key_share to our
    // own share (public_value() becomes available) + the shared secret —
    // classical DH composite: keygen + scalar-mult. Requires the instance has
    // NOT run generate(); on failure the instance stays ungenerated.
    [[nodiscard]] virtual TlsKxShared encap(std::span<const std::uint8_t> peer_public) noexcept = 0;

    // Client receiving the peer share (1.3 SH / 1.2 SKE): recovers the
    // shared secret. Length/encoding violations and off-curve points are
    // BadPeerData. Requires generate() has run.
    [[nodiscard]] virtual TlsKxShared decap(std::span<const std::uint8_t> peer_public) noexcept = 0;
};

} // namespace fiber::tls

#endif // FIBER_TLS_CRYPTO_TLS_KEY_EXCHANGE_H
