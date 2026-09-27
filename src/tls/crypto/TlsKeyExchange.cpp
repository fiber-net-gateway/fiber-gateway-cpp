#include <fiber/tls/crypto/TlsKeyExchange.h>

#include <cstring>
#include <new>

#include <fiber/common/Assert.h>

#include "TlsCryptoPrimitives.h"

namespace fiber::tls {

TlsKeyExchange::~TlsKeyExchange() = default;

namespace {

// ---- X25519: raw 32-byte scalar in place, no pimpl ----

class X25519KeyExchange final : public TlsKeyExchange {
public:
    ~X25519KeyExchange() override {
        tls_secure_wipe(scalar_.data(), scalar_.size());
        tls_secure_wipe(pub_.buf.data(), pub_.buf.size());
    }

    [[nodiscard]] TlsNamedGroup group() const noexcept override { return TlsNamedGroup::X25519; }

    [[nodiscard]] common::IoResult<void> generate() noexcept override {
        FIBER_ASSERT(!generated_);
        tls_x25519_keypair(pub_.buf.data(), scalar_.data());
        pub_.len = 32;
        generated_ = true;
        return {};
    }

    [[nodiscard]] const TlsKeySharePub &public_value() const noexcept override {
        FIBER_ASSERT(generated_);
        return pub_;
    }

    [[nodiscard]] TlsKxShared encap(std::span<const std::uint8_t> peer_public) noexcept override {
        FIBER_ASSERT(!generated_);
        TlsKxShared out;
        if (peer_public.size() != 32) {
            out.status = TlsKxStatus::BadPeerData;
            return out;
        }
        tls_x25519_keypair(pub_.buf.data(), scalar_.data());
        if (!tls_x25519_shared(out.z.data(), scalar_.data(), peer_public.data())) {
            // Rejected peer (small-order / zero): discard the pair we just
            // drew and stay ungenerated.
            tls_secure_wipe(scalar_.data(), scalar_.size());
            tls_secure_wipe(pub_.buf.data(), pub_.buf.size());
            out.status = TlsKxStatus::BadPeerData;
            return out;
        }
        out.len = 32;
        pub_.len = 32;
        generated_ = true;
        return out;
    }

    [[nodiscard]] TlsKxShared decap(std::span<const std::uint8_t> peer_public) noexcept override {
        FIBER_ASSERT(generated_);
        TlsKxShared out;
        if (peer_public.size() != 32) {
            out.status = TlsKxStatus::BadPeerData;
            return out;
        }
        if (!tls_x25519_shared(out.z.data(), scalar_.data(), peer_public.data())) {
            out.status = TlsKxStatus::BadPeerData; // rejected peer (small-order / zero)
            return out;
        }
        out.len = 32;
        return out;
    }

private:
    std::array<std::uint8_t, 32> scalar_{};
    TlsKeySharePub pub_{};
    bool generated_ = false;
};

// ---- P-256: EVP_PKEY on the heap behind the adapter's typed handle ----

// NIST ECDH over P-256 or P-384: one class, the curve fixes the point and
// shared-secret sizes.
class EcdhKeyExchange final : public TlsKeyExchange {
public:
    explicit EcdhKeyExchange(TlsEcCurve curve) noexcept : curve_(curve) {}
    ~EcdhKeyExchange() override { tls_ec_free(key_); }

    [[nodiscard]] TlsNamedGroup group() const noexcept override {
        return curve_ == TlsEcCurve::P256 ? TlsNamedGroup::Secp256r1 : TlsNamedGroup::Secp384r1;
    }

    [[nodiscard]] common::IoResult<void> generate() noexcept override {
        FIBER_ASSERT(!generated_);
        if (!tls_ec_generate(key_, curve_)) {
            return std::unexpected(common::IoErr::NoMem);
        }
        if (!tls_ec_public(key_, curve_, pub_.buf.data())) {
            return std::unexpected(common::IoErr::Unknown);
        }
        pub_.len = static_cast<std::uint8_t>(tls_ec_point_len(curve_));
        generated_ = true;
        return {};
    }

    [[nodiscard]] const TlsKeySharePub &public_value() const noexcept override {
        FIBER_ASSERT(generated_);
        return pub_;
    }

    [[nodiscard]] TlsKxShared encap(std::span<const std::uint8_t> peer_public) noexcept override {
        FIBER_ASSERT(!generated_);
        TlsKxShared out;
        if (!well_formed(peer_public)) {
            out.status = TlsKxStatus::BadPeerData;
            return out;
        }
        if (!tls_ec_generate(key_, curve_)) {
            out.status = TlsKxStatus::PrimitiveFail; // allocation — our side
            return out;
        }
        if (!tls_ec_shared(key_, curve_, peer_public, out.z.data())) {
            tls_ec_free(key_);
            out.status = TlsKxStatus::BadPeerData; // malformed or off-curve point
            return out;
        }
        if (!tls_ec_public(key_, curve_, pub_.buf.data())) {
            tls_ec_free(key_);
            out.status = TlsKxStatus::PrimitiveFail;
            return out;
        }
        out.len = static_cast<std::uint8_t>(tls_ec_field_len(curve_));
        pub_.len = static_cast<std::uint8_t>(tls_ec_point_len(curve_));
        generated_ = true;
        return out;
    }

    [[nodiscard]] TlsKxShared decap(std::span<const std::uint8_t> peer_public) noexcept override {
        FIBER_ASSERT(generated_);
        TlsKxShared out;
        if (!well_formed(peer_public)) {
            out.status = TlsKxStatus::BadPeerData;
            return out;
        }
        if (!tls_ec_shared(key_, curve_, peer_public, out.z.data())) {
            out.status = TlsKxStatus::BadPeerData; // malformed or off-curve point
            return out;
        }
        out.len = static_cast<std::uint8_t>(tls_ec_field_len(curve_));
        return out;
    }

private:
    // Uncompressed form of exactly this curve's size (RFC 8446 §4.2.8.2).
    [[nodiscard]] bool well_formed(std::span<const std::uint8_t> peer_public) const noexcept {
        return peer_public.size() == tls_ec_point_len(curve_) && peer_public.front() == 0x04;
    }

    TlsEcKey key_{}; // EVP_PKEY* — generate frees, dtor frees; null when empty
    TlsKeySharePub pub_{};
    TlsEcCurve curve_;
    bool generated_ = false;
};

} // namespace

common::IoResult<std::unique_ptr<TlsKeyExchange>> TlsKeyExchange::create(TlsNamedGroup group) noexcept {
    std::unique_ptr<TlsKeyExchange> kx;
    switch (group) {
        case TlsNamedGroup::X25519:
            kx.reset(new (std::nothrow) X25519KeyExchange());
            break;
        case TlsNamedGroup::Secp256r1:
            kx.reset(new (std::nothrow) EcdhKeyExchange(TlsEcCurve::P256));
            break;
        case TlsNamedGroup::Secp384r1:
            kx.reset(new (std::nothrow) EcdhKeyExchange(TlsEcCurve::P384));
            break;
        default:
            FIBER_ASSERT(false); // ctor-equivalent contract: the negotiated set only
            return std::unexpected(common::IoErr::NotSupported);
    }
    if (kx == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return kx;
}

} // namespace fiber::tls
