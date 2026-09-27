#include "TlsCryptoPrimitives.h"

#include <openssl/curve25519.h>
#include <openssl/digest.h>
#include <openssl/ec.h>
#include <openssl/ec_key.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hkdf.h>
#include <openssl/mem.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>

#include <cstring>

#include <fiber/common/Assert.h>

namespace fiber::tls {

namespace {

// Every false return below flows through here so the OpenSSL error queue is
// always drained: stale error state must never leak into the next call.
void drain_errors() noexcept { ERR_clear_error(); }

[[nodiscard]] const EVP_MD *evp_md(TlsHashAlgorithm hash) noexcept {
    return hash == TlsHashAlgorithm::Sha256 ? EVP_sha256() : EVP_sha384();
}

} // namespace

std::size_t tls_hash_len(TlsHashAlgorithm hash) noexcept { return hash == TlsHashAlgorithm::Sha256 ? 32 : 48; }

bool tls_hkdf_extract(std::span<std::uint8_t> prk_out, TlsHashAlgorithm hash, std::span<const std::uint8_t> ikm,
                      std::span<const std::uint8_t> salt) noexcept {
    FIBER_ASSERT(prk_out.size() >= 64); // EVP_MAX_MD_SIZE
    size_t out_len = 0;
    if (HKDF_extract(prk_out.data(), &out_len, evp_md(hash), ikm.data(), ikm.size(), salt.data(), salt.size()) != 1) {
        drain_errors();
        return false;
    }
    FIBER_ASSERT(out_len == tls_hash_len(hash));
    return true;
}

bool tls_hkdf_expand(std::span<std::uint8_t> out, TlsHashAlgorithm hash, std::span<const std::uint8_t> prk,
                     std::span<const std::uint8_t> info) noexcept {
    FIBER_ASSERT(prk.size() == tls_hash_len(hash));
    if (HKDF_expand(out.data(), out.size(), evp_md(hash), prk.data(), prk.size(), info.data(), info.size()) != 1) {
        drain_errors();
        return false;
    }
    return true;
}

bool tls_digest_empty(std::span<std::uint8_t> out, TlsHashAlgorithm hash) noexcept {
    FIBER_ASSERT(out.size() >= tls_hash_len(hash));
    unsigned out_len = 0;
    if (EVP_Digest(nullptr, 0, out.data(), &out_len, evp_md(hash), nullptr) != 1) {
        drain_errors();
        return false;
    }
    FIBER_ASSERT(out_len == tls_hash_len(hash));
    return true;
}

TlsHmac::~TlsHmac() { HMAC_CTX_cleanup(&ctx_); }

bool TlsHmac::init(TlsHashAlgorithm hash, std::span<const std::uint8_t> key) noexcept {
    if (HMAC_Init_ex(&ctx_, key.data(), static_cast<int>(key.size()), evp_md(hash), nullptr) != 1) {
        drain_errors();
        return false;
    }
    return true;
}

bool TlsHmac::update(std::span<const std::uint8_t> data) noexcept {
    if (HMAC_Update(&ctx_, data.data(), data.size()) != 1) {
        drain_errors();
        return false;
    }
    return true;
}

bool TlsHmac::final(std::span<std::uint8_t> out) noexcept {
    // HMAC_Final documents EVP_MAX_MD_SIZE capacity; write through a scratch
    // buffer so callers may pass exactly hash_len-sized spans.
    std::uint8_t buf[64];
    unsigned out_len = 0;
    if (HMAC_Final(&ctx_, buf, &out_len) != 1) {
        drain_errors();
        tls_secure_wipe(buf, sizeof buf);
        return false;
    }
    FIBER_ASSERT(out_len <= out.size());
    std::memcpy(out.data(), buf, out_len);
    tls_secure_wipe(buf, sizeof buf);
    return true;
}

bool TlsHash::init(TlsHashAlgorithm hash) noexcept {
    inited_ = false;
    if (hash == TlsHashAlgorithm::Sha256) {
        if (SHA256_Init(&ctx_.sha256) != 1) {
            drain_errors();
            return false;
        }
    } else {
        // SHA-384 shares SHA512_CTX (different IV / truncated output).
        if (SHA384_Init(&ctx_.sha384) != 1) {
            drain_errors();
            return false;
        }
    }
    hash_ = hash;
    inited_ = true;
    return true;
}

bool TlsHash::update(std::span<const std::uint8_t> data) noexcept {
    FIBER_ASSERT(inited_);
    if (data.empty()) {
        return true;
    }
    const int ok = hash_ == TlsHashAlgorithm::Sha256 ? SHA256_Update(&ctx_.sha256, data.data(), data.size())
                                                     : SHA384_Update(&ctx_.sha384, data.data(), data.size());
    if (ok != 1) {
        drain_errors();
        return false;
    }
    return true;
}

bool TlsHash::final(std::span<std::uint8_t> out) noexcept {
    FIBER_ASSERT(inited_);
    FIBER_ASSERT(out.size() >= tls_hash_len(hash_));
    const int ok = hash_ == TlsHashAlgorithm::Sha256 ? SHA256_Final(out.data(), &ctx_.sha256)
                                                     : SHA384_Final(out.data(), &ctx_.sha384);
    if (ok != 1) {
        drain_errors();
        return false;
    }
    return true;
}

bool tls_random_bytes(std::span<std::uint8_t> out) noexcept {
    if (RAND_bytes(out.data(), out.size()) != 1) {
        drain_errors();
        return false;
    }
    return true;
}

void tls_secure_wipe(void *ptr, std::size_t len) noexcept { OPENSSL_cleanse(ptr, len); }

bool tls_constant_time_equal(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) noexcept {
    return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

void tls_x25519_keypair(std::uint8_t out_public[32], std::uint8_t out_private[32]) noexcept {
    X25519_keypair(out_public, out_private);
}

bool tls_x25519_shared(std::uint8_t out_shared[32], const std::uint8_t private_key[32],
                       const std::uint8_t peer_public[32]) noexcept {
    if (X25519(out_shared, private_key, peer_public) != 1) {
        return false;
    }
    // RFC 7748 §6.1: an all-zero shared secret means the peer's public key is
    // a small-order point. BoringSSL's X25519 reports success for those, so
    // the rejection lives here, in constant time.
    static const std::uint8_t kZero[32] = {};
    if (tls_constant_time_equal(std::span<const std::uint8_t>(out_shared, 32),
                                std::span<const std::uint8_t>(kZero, 32))) {
        tls_secure_wipe(out_shared, 32);
        return false;
    }
    return true;
}

namespace {

[[nodiscard]] int ec_nid(TlsEcCurve curve) noexcept {
    return curve == TlsEcCurve::P256 ? NID_X9_62_prime256v1 : NID_secp384r1;
}

} // namespace

void tls_ec_free(TlsEcKey &key) noexcept {
    if (key.impl != nullptr) {
        EVP_PKEY_free(static_cast<EVP_PKEY *>(key.impl));
        key.impl = nullptr;
    }
}

bool tls_ec_generate(TlsEcKey &key, TlsEcCurve curve) noexcept {
    tls_ec_free(key);
    EC_KEY *ec = EC_KEY_new_by_curve_name(ec_nid(curve));
    if (ec == nullptr || EC_KEY_generate_key(ec) != 1) {
        EC_KEY_free(ec);
        drain_errors();
        return false;
    }
    EVP_PKEY *pkey = EVP_PKEY_new();
    if (pkey == nullptr || EVP_PKEY_assign_EC_KEY(pkey, ec) != 1) {
        EC_KEY_free(ec);
        EVP_PKEY_free(pkey);
        drain_errors();
        return false;
    }
    key.impl = pkey;
    return true;
}

bool tls_ec_public(const TlsEcKey &key, TlsEcCurve curve, std::uint8_t *out_uncompressed) noexcept {
    FIBER_ASSERT(key.impl != nullptr);
    const std::size_t point_len = tls_ec_point_len(curve);
    // get1 semantics: returns an up-referenced copy we must free.
    EC_KEY *ec = EVP_PKEY_get1_EC_KEY(static_cast<const EVP_PKEY *>(key.impl));
    if (ec == nullptr) {
        drain_errors();
        return false;
    }
    const size_t len = EC_POINT_point2oct(EC_KEY_get0_group(ec), EC_KEY_get0_public_key(ec),
                                          POINT_CONVERSION_UNCOMPRESSED, out_uncompressed, point_len, nullptr);
    EC_KEY_free(ec);
    if (len != point_len) {
        drain_errors();
        return false;
    }
    return true;
}

bool tls_ec_shared(const TlsEcKey &key, TlsEcCurve curve, std::span<const std::uint8_t> peer_uncompressed,
                   std::uint8_t *out_shared) noexcept {
    FIBER_ASSERT(key.impl != nullptr);
    const std::size_t point_len = tls_ec_point_len(curve);
    const std::size_t field_len = tls_ec_field_len(curve);
    if (peer_uncompressed.size() != point_len) {
        return false;
    }

    // Parse the peer point first: oct2point rejects encodings that are not a
    // point on the curve, so a malformed peer never reaches the derive.
    EVP_PKEY *peer_pkey = nullptr;
    EC_KEY *peer_ec = EC_KEY_new_by_curve_name(ec_nid(curve));
    EC_POINT *point = nullptr;
    bool ok = peer_ec != nullptr;
    if (ok) {
        const EC_GROUP *group = EC_KEY_get0_group(peer_ec);
        point = EC_POINT_new(group);
        ok = point != nullptr && EC_POINT_oct2point(group, point, peer_uncompressed.data(), point_len, nullptr) == 1 &&
             EC_KEY_set_public_key(peer_ec, point) == 1;
    }
    EC_POINT_free(point);
    if (ok) {
        peer_pkey = EVP_PKEY_new();
        ok = peer_pkey != nullptr && EVP_PKEY_assign_EC_KEY(peer_pkey, peer_ec) == 1;
    }
    if (!ok) {
        EC_KEY_free(peer_ec);
        EVP_PKEY_free(peer_pkey);
        drain_errors();
        return false;
    }

    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(static_cast<EVP_PKEY *>(key.impl), nullptr);
    ok = ctx != nullptr && EVP_PKEY_derive_init(ctx) == 1 && EVP_PKEY_derive_set_peer(ctx, peer_pkey) == 1;
    if (ok) {
        size_t out_len = field_len;
        ok = EVP_PKEY_derive(ctx, out_shared, &out_len) == 1 && out_len == field_len;
    }
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(peer_pkey);
    if (!ok) {
        drain_errors();
    }
    return ok;
}

} // namespace fiber::tls
