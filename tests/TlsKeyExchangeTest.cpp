#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include <openssl/ec.h>
#include <openssl/evp.h>

#include <fiber/tls/crypto/TlsKeyExchange.h>

using namespace fiber::tls;

namespace {

std::vector<std::uint8_t> ramp(std::size_t len, std::uint8_t seed) {
    std::vector<std::uint8_t> out(len);
    for (std::size_t i = 0; i < len; ++i) {
        out[i] = static_cast<std::uint8_t>(seed + i);
    }
    return out;
}

// Deterministic off-curve P-256 encodings: (0,0) is not on
// y^2 = x^3 - 3x + b, and neither is (1,0) since b - 2 != 0 mod p.
std::vector<std::uint8_t> p256_bad_point(std::uint8_t x_last_byte, std::uint8_t y_last_byte) {
    std::vector<std::uint8_t> out(65, 0);
    out[0] = 0x04;
    out[31] = x_last_byte;
    out[63] = y_last_byte;
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// X25519
// ---------------------------------------------------------------------------

TEST(TlsKeyExchange, X25519AgreesBothWays) {
    TlsKeyExchange a(TlsNamedGroup::X25519);
    TlsKeyExchange b(TlsNamedGroup::X25519);
    ASSERT_TRUE(a.generate().has_value());
    ASSERT_TRUE(b.generate().has_value());
    EXPECT_EQ(32u, a.public_value().len);
    EXPECT_EQ(32u, b.public_value().len);

    const TlsKxShared za = a.shared_secret(b.public_value().bytes());
    const TlsKxShared zb = b.shared_secret(a.public_value().bytes());
    EXPECT_EQ(TlsKxStatus::Ok, za.status);
    EXPECT_EQ(TlsKxStatus::Ok, zb.status);
    EXPECT_EQ(0, std::memcmp(za.z.data(), zb.z.data(), sizeof(za.z)));
}

TEST(TlsKeyExchange, X25519DistinctPairsGiveDistinctSecrets) {
    TlsKeyExchange a(TlsNamedGroup::X25519), b(TlsNamedGroup::X25519), c(TlsNamedGroup::X25519),
            d(TlsNamedGroup::X25519);
    ASSERT_TRUE(a.generate().has_value());
    ASSERT_TRUE(b.generate().has_value());
    ASSERT_TRUE(c.generate().has_value());
    ASSERT_TRUE(d.generate().has_value());

    const TlsKxShared z1 = a.shared_secret(b.public_value().bytes());
    const TlsKxShared z2 = c.shared_secret(d.public_value().bytes());
    ASSERT_EQ(TlsKxStatus::Ok, z1.status);
    ASSERT_EQ(TlsKxStatus::Ok, z2.status);
    EXPECT_NE(0, std::memcmp(z1.z.data(), z2.z.data(), sizeof(z1.z)));
}

TEST(TlsKeyExchange, X25519RejectsBadPeerData) {
    TlsKeyExchange a(TlsNamedGroup::X25519);
    ASSERT_TRUE(a.generate().has_value());

    // Wrong lengths.
    EXPECT_EQ(TlsKxStatus::BadPeerData, a.shared_secret(ramp(31, 0)).status);
    EXPECT_EQ(TlsKxStatus::BadPeerData, a.shared_secret(ramp(33, 0)).status);
    // All-zero public key: small-order point, RFC 7748 §6.1 rejection.
    EXPECT_EQ(TlsKxStatus::BadPeerData, a.shared_secret(std::vector<std::uint8_t>(32, 0)).status);
}

// ---------------------------------------------------------------------------
// P-256
// ---------------------------------------------------------------------------

TEST(TlsKeyExchange, P256AgreesBothWays) {
    TlsKeyExchange a(TlsNamedGroup::Secp256r1);
    TlsKeyExchange b(TlsNamedGroup::Secp256r1);
    ASSERT_TRUE(a.generate().has_value());
    ASSERT_TRUE(b.generate().has_value());
    EXPECT_EQ(65u, a.public_value().len);
    EXPECT_EQ(0x04, a.public_value().buf[0]); // uncompressed marker
    EXPECT_EQ(65u, b.public_value().len);

    const TlsKxShared za = a.shared_secret(b.public_value().bytes());
    const TlsKxShared zb = b.shared_secret(a.public_value().bytes());
    EXPECT_EQ(TlsKxStatus::Ok, za.status);
    EXPECT_EQ(TlsKxStatus::Ok, zb.status);
    EXPECT_EQ(0, std::memcmp(za.z.data(), zb.z.data(), sizeof(za.z)));
}

TEST(TlsKeyExchange, P256InteropWithBoringSSLDerive) {
    // A raw BoringSSL EVP key pair on the other side: our shared secret must
    // equal plain EVP_PKEY_derive over our public key — self-agreement alone
    // would hide any encoding-level divergence.
    EVP_PKEY_CTX *gen = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    ASSERT_NE(nullptr, gen);
    ASSERT_EQ(1, EVP_PKEY_keygen_init(gen));
    ASSERT_EQ(1, EVP_PKEY_CTX_set_ec_paramgen_curve_nid(gen, NID_X9_62_prime256v1));
    EVP_PKEY *peer = nullptr;
    ASSERT_EQ(1, EVP_PKEY_keygen(gen, &peer));
    EVP_PKEY_CTX_free(gen);

    const EC_KEY *peer_ec = EVP_PKEY_get0_EC_KEY(peer);
    ASSERT_NE(nullptr, peer_ec);
    std::vector<std::uint8_t> peer_pub(65);
    ASSERT_EQ(65u, EC_POINT_point2oct(EC_KEY_get0_group(peer_ec), EC_KEY_get0_public_key(peer_ec),
                                      POINT_CONVERSION_UNCOMPRESSED, peer_pub.data(), 65, nullptr));

    TlsKeyExchange ours(TlsNamedGroup::Secp256r1);
    ASSERT_TRUE(ours.generate().has_value());
    const TlsKxShared z1 = ours.shared_secret(peer_pub);
    ASSERT_EQ(TlsKxStatus::Ok, z1.status);

    // BoringSSL derives with our public key.
    const auto our_pub = ours.public_value().bytes();
    ASSERT_EQ(65u, our_pub.size());
    EVP_PKEY_CTX *derive = EVP_PKEY_CTX_new(peer, nullptr);
    ASSERT_NE(nullptr, derive);
    ASSERT_EQ(1, EVP_PKEY_derive_init(derive));
    EC_KEY *our_ec = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    EC_POINT *our_point = nullptr;
    ASSERT_NE(nullptr, our_ec);
    our_point = EC_POINT_new(EC_KEY_get0_group(our_ec));
    ASSERT_NE(nullptr, our_point);
    ASSERT_EQ(1, EC_POINT_oct2point(EC_KEY_get0_group(our_ec), our_point, our_pub.data(), 65, nullptr));
    ASSERT_EQ(1, EC_KEY_set_public_key(our_ec, our_point));
    EC_POINT_free(our_point);
    EVP_PKEY *our_pkey = EVP_PKEY_new();
    ASSERT_NE(nullptr, our_pkey);
    ASSERT_EQ(1, EVP_PKEY_assign_EC_KEY(our_pkey, our_ec));
    ASSERT_EQ(1, EVP_PKEY_derive_set_peer(derive, our_pkey));

    std::uint8_t z2[32];
    std::size_t z2_len = sizeof(z2);
    ASSERT_EQ(1, EVP_PKEY_derive(derive, z2, &z2_len));
    EVP_PKEY_CTX_free(derive);
    EVP_PKEY_free(our_pkey);
    EVP_PKEY_free(peer);

    ASSERT_EQ(32u, z2_len);
    EXPECT_EQ(0, std::memcmp(z1.z.data(), z2, 32));
}

TEST(TlsKeyExchange, P256RejectsBadPeerData) {
    TlsKeyExchange a(TlsNamedGroup::Secp256r1);
    ASSERT_TRUE(a.generate().has_value());

    // Wrong lengths.
    EXPECT_EQ(TlsKxStatus::BadPeerData, a.shared_secret(ramp(64, 0x04)).status);
    EXPECT_EQ(TlsKxStatus::BadPeerData, a.shared_secret(ramp(66, 0x04)).status);
    // Compressed-point marker is not accepted.
    std::vector<std::uint8_t> compressed = ramp(65, 0);
    compressed[0] = 0x02;
    EXPECT_EQ(TlsKxStatus::BadPeerData, a.shared_secret(compressed).status);
    // Off-curve points: (0,0) and (1,0).
    EXPECT_EQ(TlsKxStatus::BadPeerData, a.shared_secret(p256_bad_point(0x00, 0x00)).status);
    EXPECT_EQ(TlsKxStatus::BadPeerData, a.shared_secret(p256_bad_point(0x01, 0x00)).status);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

TEST(TlsKeyExchange, WipeAllowsRegeneration) {
    TlsKeyExchange a(TlsNamedGroup::X25519);
    TlsKeyExchange b(TlsNamedGroup::X25519);
    ASSERT_TRUE(a.generate().has_value());
    ASSERT_TRUE(b.generate().has_value());
    const std::vector<std::uint8_t> first_pub(a.public_value().bytes().begin(), a.public_value().bytes().end());

    a.wipe();
    ASSERT_TRUE(a.generate().has_value());

    // Fresh keypair: shared secret still derivable and consistent.
    const TlsKxShared za = a.shared_secret(b.public_value().bytes());
    const TlsKxShared zb = b.shared_secret(a.public_value().bytes());
    EXPECT_EQ(TlsKxStatus::Ok, za.status);
    EXPECT_EQ(TlsKxStatus::Ok, zb.status);
    EXPECT_EQ(0, std::memcmp(za.z.data(), zb.z.data(), sizeof(za.z)));

    // Regenerated X25519 keys collide with probability 2^-256-ish; the two
    // public values must not match.
    EXPECT_NE(0, std::memcmp(first_pub.data(), a.public_value().bytes().data(), 32));
}

TEST(TlsKeyExchange, P256WipeFreesTheKeyHandle) {
    TlsKeyExchange a(TlsNamedGroup::Secp256r1);
    ASSERT_TRUE(a.generate().has_value());
    a.wipe();
    ASSERT_TRUE(a.generate().has_value());
    EXPECT_EQ(65u, a.public_value().len);
}

// ---------------------------------------------------------------------------
// Contract violations are engine bugs — FIBER_ASSERT
// ---------------------------------------------------------------------------

TEST(TlsKeyExchangeDeath, ContractViolations) {
    EXPECT_DEATH((void) TlsKeyExchange(TlsNamedGroup::Secp384r1), "FIBER_ASSERT failed");

    EXPECT_DEATH(
            {
                TlsKeyExchange a(TlsNamedGroup::X25519);
                (void) a.generate();
                (void) a.generate(); // twice
            },
            "FIBER_ASSERT failed");

    EXPECT_DEATH(
            {
                TlsKeyExchange a(TlsNamedGroup::X25519);
                (void) a.public_value(); // before generate
            },
            "FIBER_ASSERT failed");

    EXPECT_DEATH(
            {
                TlsKeyExchange a(TlsNamedGroup::X25519);
                (void) a.shared_secret(ramp(32, 0)); // before generate
            },
            "FIBER_ASSERT failed");

    EXPECT_DEATH(
            {
                TlsKeyExchange a(TlsNamedGroup::X25519);
                (void) a.generate();
                a.wipe();
                (void) a.public_value(); // after wipe
            },
            "FIBER_ASSERT failed");
}
