#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <vector>

#include <openssl/ec.h>
#include <openssl/ecdh.h>
#include <openssl/evp.h>

#include <fiber/tls/crypto/TlsKeyExchange.h>

using namespace fiber::tls;

namespace {

// Factory wrapper: allocation cannot realistically fail here, so a null
// return surfaces as the very next assertion.
std::unique_ptr<TlsKeyExchange> make_kx(TlsNamedGroup group) {
    auto made = TlsKeyExchange::create(group);
    EXPECT_TRUE(made.has_value());
    return made.has_value() ? std::move(*made) : nullptr;
}

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
    auto a = make_kx(TlsNamedGroup::X25519);
    auto b = make_kx(TlsNamedGroup::X25519);
    ASSERT_NE(nullptr, a.get());
    ASSERT_NE(nullptr, b.get());
    ASSERT_TRUE(a->generate().has_value());
    ASSERT_TRUE(b->generate().has_value());
    EXPECT_EQ(32u, a->public_value().len);
    EXPECT_EQ(32u, b->public_value().len);

    const TlsKxShared za = a->decap(b->public_value().bytes());
    const TlsKxShared zb = b->decap(a->public_value().bytes());
    EXPECT_EQ(TlsKxStatus::Ok, za.status);
    EXPECT_EQ(TlsKxStatus::Ok, zb.status);
    EXPECT_EQ(0, std::memcmp(za.z.data(), zb.z.data(), sizeof(za.z)));
}

TEST(TlsKeyExchange, X25519DistinctPairsGiveDistinctSecrets) {
    auto a = make_kx(TlsNamedGroup::X25519), b = make_kx(TlsNamedGroup::X25519), c = make_kx(TlsNamedGroup::X25519),
         d = make_kx(TlsNamedGroup::X25519);
    ASSERT_NE(nullptr, a.get());
    ASSERT_NE(nullptr, b.get());
    ASSERT_NE(nullptr, c.get());
    ASSERT_NE(nullptr, d.get());
    ASSERT_TRUE(a->generate().has_value());
    ASSERT_TRUE(b->generate().has_value());
    ASSERT_TRUE(c->generate().has_value());
    ASSERT_TRUE(d->generate().has_value());

    const TlsKxShared z1 = a->decap(b->public_value().bytes());
    const TlsKxShared z2 = c->decap(d->public_value().bytes());
    ASSERT_EQ(TlsKxStatus::Ok, z1.status);
    ASSERT_EQ(TlsKxStatus::Ok, z2.status);
    EXPECT_NE(0, std::memcmp(z1.z.data(), z2.z.data(), sizeof(z1.z)));
}

TEST(TlsKeyExchange, X25519RejectsBadPeerData) {
    auto a = make_kx(TlsNamedGroup::X25519);
    ASSERT_NE(nullptr, a.get());
    ASSERT_TRUE(a->generate().has_value());

    // Wrong lengths.
    EXPECT_EQ(TlsKxStatus::BadPeerData, a->decap(ramp(31, 0)).status);
    EXPECT_EQ(TlsKxStatus::BadPeerData, a->decap(ramp(33, 0)).status);
    // All-zero public key: small-order point, RFC 7748 §6.1 rejection.
    EXPECT_EQ(TlsKxStatus::BadPeerData, a->decap(std::vector<std::uint8_t>(32, 0)).status);
}

// ---------------------------------------------------------------------------
// P-256
// ---------------------------------------------------------------------------

TEST(TlsKeyExchange, P256AgreesBothWays) {
    auto a = make_kx(TlsNamedGroup::Secp256r1);
    auto b = make_kx(TlsNamedGroup::Secp256r1);
    ASSERT_NE(nullptr, a.get());
    ASSERT_NE(nullptr, b.get());
    ASSERT_TRUE(a->generate().has_value());
    ASSERT_TRUE(b->generate().has_value());
    EXPECT_EQ(65u, a->public_value().len);
    EXPECT_EQ(0x04, a->public_value().buf[0]); // uncompressed marker
    EXPECT_EQ(65u, b->public_value().len);

    const TlsKxShared za = a->decap(b->public_value().bytes());
    const TlsKxShared zb = b->decap(a->public_value().bytes());
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

    auto ours = make_kx(TlsNamedGroup::Secp256r1);
    ASSERT_NE(nullptr, ours.get());
    ASSERT_TRUE(ours->generate().has_value());
    const TlsKxShared z1 = ours->decap(peer_pub);
    ASSERT_EQ(TlsKxStatus::Ok, z1.status);

    // BoringSSL derives with our public key.
    const auto our_pub = ours->public_value().bytes();
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
    auto a = make_kx(TlsNamedGroup::Secp256r1);
    ASSERT_NE(nullptr, a.get());
    ASSERT_TRUE(a->generate().has_value());

    // Wrong lengths.
    EXPECT_EQ(TlsKxStatus::BadPeerData, a->decap(ramp(64, 0x04)).status);
    EXPECT_EQ(TlsKxStatus::BadPeerData, a->decap(ramp(66, 0x04)).status);
    // Compressed-point marker is not accepted.
    std::vector<std::uint8_t> compressed = ramp(65, 0);
    compressed[0] = 0x02;
    EXPECT_EQ(TlsKxStatus::BadPeerData, a->decap(compressed).status);
    // Off-curve points: (0,0) and (1,0).
    EXPECT_EQ(TlsKxStatus::BadPeerData, a->decap(p256_bad_point(0x00, 0x00)).status);
    EXPECT_EQ(TlsKxStatus::BadPeerData, a->decap(p256_bad_point(0x01, 0x00)).status);
}

// ---------------------------------------------------------------------------
// P-384: 97-byte uncompressed points, 48-byte shared secret
// ---------------------------------------------------------------------------

TEST(TlsKeyExchange, P384AgreesBothWays) {
    auto a = make_kx(TlsNamedGroup::Secp384r1);
    auto b = make_kx(TlsNamedGroup::Secp384r1);
    ASSERT_NE(nullptr, a.get());
    ASSERT_NE(nullptr, b.get());
    EXPECT_EQ(TlsNamedGroup::Secp384r1, a->group());
    ASSERT_TRUE(a->generate().has_value());
    ASSERT_TRUE(b->generate().has_value());
    EXPECT_EQ(97u, a->public_value().len);
    EXPECT_EQ(0x04, a->public_value().buf[0]);

    const TlsKxShared za = a->decap(b->public_value().bytes());
    const TlsKxShared zb = b->decap(a->public_value().bytes());
    ASSERT_EQ(TlsKxStatus::Ok, za.status);
    ASSERT_EQ(TlsKxStatus::Ok, zb.status);
    EXPECT_EQ(48u, za.len);
    EXPECT_EQ(48u, za.bytes().size());
    EXPECT_EQ(0, std::memcmp(za.z.data(), zb.z.data(), 48));
}

TEST(TlsKeyExchange, P384InteropWithBoringSSLDerive) {
    // As the P-256 case: our secret must equal a plain BoringSSL derive.
    EC_KEY *peer_ec = EC_KEY_new_by_curve_name(NID_secp384r1);
    ASSERT_NE(nullptr, peer_ec);
    ASSERT_EQ(1, EC_KEY_generate_key(peer_ec));
    std::vector<std::uint8_t> peer_pub(97);
    ASSERT_EQ(97u, EC_POINT_point2oct(EC_KEY_get0_group(peer_ec), EC_KEY_get0_public_key(peer_ec),
                                      POINT_CONVERSION_UNCOMPRESSED, peer_pub.data(), 97, nullptr));

    auto ours = make_kx(TlsNamedGroup::Secp384r1);
    ASSERT_NE(nullptr, ours.get());
    ASSERT_TRUE(ours->generate().has_value());
    const TlsKxShared z1 = ours->decap(peer_pub);
    ASSERT_EQ(TlsKxStatus::Ok, z1.status);

    const auto our_pub = ours->public_value().bytes();
    ASSERT_EQ(97u, our_pub.size());
    EC_POINT *our_point = EC_POINT_new(EC_KEY_get0_group(peer_ec));
    ASSERT_NE(nullptr, our_point);
    ASSERT_EQ(1, EC_POINT_oct2point(EC_KEY_get0_group(peer_ec), our_point, our_pub.data(), 97, nullptr));
    std::uint8_t z2[48];
    ASSERT_EQ(48, ECDH_compute_key(z2, sizeof(z2), our_point, peer_ec, nullptr));
    EC_POINT_free(our_point);
    EC_KEY_free(peer_ec);

    ASSERT_EQ(48u, z1.len);
    EXPECT_EQ(0, std::memcmp(z1.z.data(), z2, 48));
}

TEST(TlsKeyExchange, P384RejectsBadPeerData) {
    auto a = make_kx(TlsNamedGroup::Secp384r1);
    ASSERT_NE(nullptr, a.get());
    ASSERT_TRUE(a->generate().has_value());

    EXPECT_EQ(TlsKxStatus::BadPeerData, a->decap(ramp(96, 0x04)).status);
    EXPECT_EQ(TlsKxStatus::BadPeerData, a->decap(ramp(98, 0x04)).status);
    // A valid P-256 point is the wrong size (and curve) here.
    auto p256 = make_kx(TlsNamedGroup::Secp256r1);
    ASSERT_TRUE(p256->generate().has_value());
    EXPECT_EQ(TlsKxStatus::BadPeerData, a->decap(p256->public_value().bytes()).status);
    // Compressed marker, and the off-curve point (0,0).
    std::vector<std::uint8_t> bad(97, 0);
    bad[0] = 0x02;
    EXPECT_EQ(TlsKxStatus::BadPeerData, a->decap(bad).status);
    bad[0] = 0x04;
    EXPECT_EQ(TlsKxStatus::BadPeerData, a->decap(bad).status);
    // encap applies the same checks and stays ungenerated.
    auto server = make_kx(TlsNamedGroup::Secp384r1);
    EXPECT_EQ(TlsKxStatus::BadPeerData, server->encap(bad).status);
}

TEST(TlsKeyExchange, P384EncapAgreesWithDecap) {
    auto client = make_kx(TlsNamedGroup::Secp384r1);
    auto server = make_kx(TlsNamedGroup::Secp384r1);
    ASSERT_NE(nullptr, client.get());
    ASSERT_NE(nullptr, server.get());
    ASSERT_TRUE(client->generate().has_value());

    const TlsKxShared zs = server->encap(client->public_value().bytes());
    ASSERT_EQ(TlsKxStatus::Ok, zs.status);
    EXPECT_EQ(97u, server->public_value().len);
    const TlsKxShared zc = client->decap(server->public_value().bytes());
    ASSERT_EQ(TlsKxStatus::Ok, zc.status);
    ASSERT_EQ(48u, zs.len);
    ASSERT_EQ(48u, zc.len);
    EXPECT_EQ(0, std::memcmp(zs.z.data(), zc.z.data(), 48));
}

// ---------------------------------------------------------------------------
// encap — the server-side composite (07's consumer; keygen + shared secret)
// ---------------------------------------------------------------------------

TEST(TlsKeyExchange, X25519EncapAgreesWithDecap) {
    auto client = make_kx(TlsNamedGroup::X25519);
    auto server = make_kx(TlsNamedGroup::X25519);
    ASSERT_NE(nullptr, client.get());
    ASSERT_NE(nullptr, server.get());
    ASSERT_TRUE(client->generate().has_value());

    const TlsKxShared zs = server->encap(client->public_value().bytes());
    ASSERT_EQ(TlsKxStatus::Ok, zs.status);
    EXPECT_EQ(TlsNamedGroup::X25519, server->group());
    EXPECT_EQ(32u, server->public_value().len); // encap produced the share

    const TlsKxShared zc = client->decap(server->public_value().bytes());
    ASSERT_EQ(TlsKxStatus::Ok, zc.status);
    EXPECT_EQ(0, std::memcmp(zs.z.data(), zc.z.data(), sizeof(zs.z)));
}

TEST(TlsKeyExchange, P256EncapAgreesWithDecap) {
    auto client = make_kx(TlsNamedGroup::Secp256r1);
    auto server = make_kx(TlsNamedGroup::Secp256r1);
    ASSERT_NE(nullptr, client.get());
    ASSERT_NE(nullptr, server.get());
    ASSERT_TRUE(client->generate().has_value());

    const TlsKxShared zs = server->encap(client->public_value().bytes());
    ASSERT_EQ(TlsKxStatus::Ok, zs.status);
    EXPECT_EQ(TlsNamedGroup::Secp256r1, server->group());
    EXPECT_EQ(65u, server->public_value().len);
    EXPECT_EQ(0x04, server->public_value().buf[0]);

    const TlsKxShared zc = client->decap(server->public_value().bytes());
    ASSERT_EQ(TlsKxStatus::Ok, zc.status);
    EXPECT_EQ(0, std::memcmp(zs.z.data(), zc.z.data(), sizeof(zs.z)));
}

TEST(TlsKeyExchange, EncapRejectsBadPeerData) {
    auto x = make_kx(TlsNamedGroup::X25519);
    auto p = make_kx(TlsNamedGroup::Secp256r1);
    ASSERT_NE(nullptr, x.get());
    ASSERT_NE(nullptr, p.get());

    // Wrong lengths.
    EXPECT_EQ(TlsKxStatus::BadPeerData, x->encap(ramp(31, 0)).status);
    EXPECT_EQ(TlsKxStatus::BadPeerData, x->encap(ramp(33, 0)).status);
    // All-zero public key: small-order point rejection.
    EXPECT_EQ(TlsKxStatus::BadPeerData, x->encap(std::vector<std::uint8_t>(32, 0)).status);
    EXPECT_EQ(TlsKxStatus::BadPeerData, p->encap(ramp(64, 0x04)).status);
    // Compressed-point marker is not accepted.
    std::vector<std::uint8_t> compressed = ramp(65, 0);
    compressed[0] = 0x02;
    EXPECT_EQ(TlsKxStatus::BadPeerData, p->encap(compressed).status);
    // Off-curve point: (0,0).
    EXPECT_EQ(TlsKxStatus::BadPeerData, p->encap(p256_bad_point(0x00, 0x00)).status);
}

// ---------------------------------------------------------------------------
// Lifecycle: regeneration is instance replacement — destruction wipes
// ---------------------------------------------------------------------------

TEST(TlsKeyExchange, FreshInstanceGivesFreshPair) {
    auto a = make_kx(TlsNamedGroup::X25519);
    auto b = make_kx(TlsNamedGroup::X25519);
    ASSERT_NE(nullptr, a.get());
    ASSERT_NE(nullptr, b.get());
    ASSERT_TRUE(a->generate().has_value());
    ASSERT_TRUE(b->generate().has_value());
    const std::vector<std::uint8_t> first_pub(a->public_value().bytes().begin(), a->public_value().bytes().end());

    a = make_kx(TlsNamedGroup::X25519); // destruction wipes the old scalar
    ASSERT_NE(nullptr, a.get());
    ASSERT_TRUE(a->generate().has_value());

    // Fresh keypair: shared secret still derivable and consistent.
    const TlsKxShared za = a->decap(b->public_value().bytes());
    const TlsKxShared zb = b->decap(a->public_value().bytes());
    EXPECT_EQ(TlsKxStatus::Ok, za.status);
    EXPECT_EQ(TlsKxStatus::Ok, zb.status);
    EXPECT_EQ(0, std::memcmp(za.z.data(), zb.z.data(), sizeof(za.z)));

    // Regenerated X25519 keys collide with probability 2^-256-ish; the two
    // public values must not match.
    EXPECT_NE(0, std::memcmp(first_pub.data(), a->public_value().bytes().data(), 32));
}

TEST(TlsKeyExchange, P256FreshInstanceFreesTheKeyHandle) {
    auto a = make_kx(TlsNamedGroup::Secp256r1);
    ASSERT_NE(nullptr, a.get());
    ASSERT_TRUE(a->generate().has_value());
    a = make_kx(TlsNamedGroup::Secp256r1); // destruction frees the EVP_PKEY handle
    ASSERT_NE(nullptr, a.get());
    ASSERT_TRUE(a->generate().has_value());
    EXPECT_EQ(65u, a->public_value().len);
}

// ---------------------------------------------------------------------------
// Contract violations are engine bugs — FIBER_ASSERT
// ---------------------------------------------------------------------------

TEST(TlsKeyExchangeDeath, ContractViolations) {
    EXPECT_DEATH((void) TlsKeyExchange::create(TlsNamedGroup::Ffdhe2048), "FIBER_ASSERT failed");

    EXPECT_DEATH(
            {
                auto a = make_kx(TlsNamedGroup::X25519);
                (void) a->generate();
                (void) a->generate(); // twice
            },
            "FIBER_ASSERT failed");

    EXPECT_DEATH(
            {
                auto a = make_kx(TlsNamedGroup::X25519);
                (void) a->public_value(); // before generate
            },
            "FIBER_ASSERT failed");

    EXPECT_DEATH(
            {
                auto a = make_kx(TlsNamedGroup::X25519);
                (void) a->decap(ramp(32, 0)); // before generate
            },
            "FIBER_ASSERT failed");

    EXPECT_DEATH(
            {
                auto a = make_kx(TlsNamedGroup::X25519);
                (void) a->generate();
                (void) a->encap(ramp(32, 0)); // after generate
            },
            "FIBER_ASSERT failed");
}
