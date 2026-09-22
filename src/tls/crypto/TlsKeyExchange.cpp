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
        }
        return out;
    }

private:
    std::array<std::uint8_t, 32> scalar_{};
    TlsKeySharePub pub_{};
    bool generated_ = false;
};

// ---- P-256: EVP_PKEY on the heap behind the adapter's typed handle ----

class P256KeyExchange final : public TlsKeyExchange {
public:
    ~P256KeyExchange() override { tls_p256_free(key_); }

    [[nodiscard]] TlsNamedGroup group() const noexcept override { return TlsNamedGroup::Secp256r1; }

    [[nodiscard]] common::IoResult<void> generate() noexcept override {
        FIBER_ASSERT(!generated_);
        if (!tls_p256_generate(key_)) {
            return std::unexpected(common::IoErr::NoMem);
        }
        if (!tls_p256_public(key_, pub_.buf.data())) {
            return std::unexpected(common::IoErr::Unknown);
        }
        pub_.len = 65;
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
        if (peer_public.size() != 65 || peer_public.front() != 0x04) {
            out.status = TlsKxStatus::BadPeerData;
            return out;
        }
        if (!tls_p256_generate(key_)) {
            out.status = TlsKxStatus::PrimitiveFail; // allocation — our side
            return out;
        }
        if (!tls_p256_shared(key_, peer_public, out.z.data())) {
            tls_p256_free(key_);
            out.status = TlsKxStatus::BadPeerData; // malformed or off-curve point
            return out;
        }
        if (!tls_p256_public(key_, pub_.buf.data())) {
            tls_p256_free(key_);
            out.status = TlsKxStatus::PrimitiveFail;
            return out;
        }
        pub_.len = 65;
        generated_ = true;
        return out;
    }

    [[nodiscard]] TlsKxShared decap(std::span<const std::uint8_t> peer_public) noexcept override {
        FIBER_ASSERT(generated_);
        TlsKxShared out;
        if (peer_public.size() != 65 || peer_public.front() != 0x04) {
            out.status = TlsKxStatus::BadPeerData;
            return out;
        }
        if (!tls_p256_shared(key_, peer_public, out.z.data())) {
            out.status = TlsKxStatus::BadPeerData; // malformed or off-curve point
        }
        return out;
    }

private:
    TlsP256Key key_{}; // EVP_PKEY* — generate frees, dtor frees; null when empty
    TlsKeySharePub pub_{};
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
            kx.reset(new (std::nothrow) P256KeyExchange());
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
