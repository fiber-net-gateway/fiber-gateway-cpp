#include <fiber/tls/crypto/TlsKeyExchange.h>

#include <cstring>

#include <fiber/common/Assert.h>

#include "TlsCryptoPrimitives.h"

namespace fiber::tls {

TlsKeyExchange::TlsKeyExchange(TlsNamedGroup group) noexcept : group_(group) {
    FIBER_ASSERT(group_ == TlsNamedGroup::X25519 || group_ == TlsNamedGroup::Secp256r1);
}

TlsKeyExchange::~TlsKeyExchange() { wipe(); }

common::IoResult<void> TlsKeyExchange::generate() noexcept {
    FIBER_ASSERT(!generated_);
    switch (group_) {
        case TlsNamedGroup::X25519: {
            tls_x25519_keypair(pub_.buf.data(), x25519_priv_.data());
            pub_.len = 32;
            generated_ = true;
            return {};
        }
        case TlsNamedGroup::Secp256r1: {
            TlsP256Key key{p256_};
            if (!tls_p256_generate(key)) {
                return std::unexpected(common::IoErr::NoMem);
            }
            p256_ = key.impl;
            if (!tls_p256_public(key, pub_.buf.data())) {
                return std::unexpected(common::IoErr::Unknown);
            }
            pub_.len = 65;
            generated_ = true;
            return {};
        }
        default:
            FIBER_ASSERT(false); // ctor pinned the group
            return std::unexpected(common::IoErr::NotSupported);
    }
}

const TlsKeySharePub &TlsKeyExchange::public_value() const noexcept {
    FIBER_ASSERT(generated_);
    return pub_;
}

TlsKxShared TlsKeyExchange::shared_secret(std::span<const std::uint8_t> peer_public) const noexcept {
    FIBER_ASSERT(generated_);
    TlsKxShared out;
    switch (group_) {
        case TlsNamedGroup::X25519:
            if (peer_public.size() != 32) {
                out.status = TlsKxStatus::BadPeerData;
                return out;
            }
            if (!tls_x25519_shared(out.z.data(), x25519_priv_.data(), peer_public.data())) {
                out.status = TlsKxStatus::BadPeerData; // rejected peer (small-order / zero)
            }
            return out;
        case TlsNamedGroup::Secp256r1: {
            if (peer_public.size() != 65 || peer_public.front() != 0x04) {
                out.status = TlsKxStatus::BadPeerData;
                return out;
            }
            TlsP256Key key{p256_};
            if (!tls_p256_shared(key, peer_public, out.z.data())) {
                out.status = TlsKxStatus::BadPeerData; // malformed or off-curve point
            }
            return out;
        }
        default:
            FIBER_ASSERT(false);
            out.status = TlsKxStatus::PrimitiveFail;
            return out;
    }
}

void TlsKeyExchange::wipe() noexcept {
    tls_secure_wipe(x25519_priv_.data(), x25519_priv_.size());
    tls_secure_wipe(pub_.buf.data(), pub_.buf.size());
    pub_.len = 0;
    TlsP256Key key{p256_};
    tls_p256_free(key);
    p256_ = key.impl;
    generated_ = false;
}

} // namespace fiber::tls
