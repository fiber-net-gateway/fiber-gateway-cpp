#include <gtest/gtest.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "TlsRfc8448Constants.h"

#include <fiber/tls/crypto/TlsKeySchedule.h>
#include <fiber/tls/record/TlsRecordCipher.h>

using namespace fiber::tls;

namespace {

// All RFC 8448 §3/§4 values below come from tests/TlsRfc8448Constants.h —
// machine-generated from the RFC text by temp/gen8448.py, which re-verified
// every derivation with an independent hashlib recomputation before emitting
// it. Nothing here is transcribed by hand.

template<std::size_t N>
std::span<const std::uint8_t> as_span(const std::array<std::uint8_t, N> &a) {
    return {a.data(), a.size()};
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> ramp(std::size_t len, std::uint8_t seed) {
    std::vector<std::uint8_t> out(len);
    for (std::size_t i = 0; i < len; ++i) {
        out[i] = static_cast<std::uint8_t>(seed + i);
    }
    return out;
}

void expect_eq_bytes(std::span<const std::uint8_t> got, std::span<const std::uint8_t> want) {
    ASSERT_EQ(want.size(), got.size());
    if (want.size() != got.size()) {
        return;
    }
    EXPECT_EQ(0, std::memcmp(got.data(), want.data(), got.size()));
}

void expect_secret(const TlsSecret &secret, std::span<const std::uint8_t> want) {
    expect_eq_bytes(secret.bytes(), want);
}

// ---------------------------------------------------------------------------
// Independent reference implementation. Hand-rolled RFC 5869 HKDF and
// RFC 5246 §5 P_hash on top of the raw OpenSSL one-shot HMAC — no shared code
// path with the adapter (which uses HKDF_*/TlsHmac). RFC 8448 has no SHA-384
// traces, so this reference is the oracle for the SHA-384 tree and all of
// TLS 1.2; it is itself calibrated against RFC 8448 §3/§4 below.
// ---------------------------------------------------------------------------

const EVP_MD *ref_md(TlsHashAlgorithm hash) { return hash == TlsHashAlgorithm::Sha256 ? EVP_sha256() : EVP_sha384(); }

std::size_t ref_md_len(TlsHashAlgorithm hash) { return hash == TlsHashAlgorithm::Sha256 ? 32 : 48; }

std::vector<std::uint8_t> ref_hmac(TlsHashAlgorithm hash, std::span<const std::uint8_t> key,
                                   std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> out(ref_md_len(hash));
    unsigned len = 0;
    EXPECT_NE(nullptr,
              HMAC(ref_md(hash), key.data(), static_cast<int>(key.size()), data.data(), data.size(), out.data(), &len));
    return out;
}

// Hash of the empty input — the TLS 1.3 "empty transcript" context (the
// "" of Derive-Secret(., label, "") is the empty message SEQUENCE, so the
// context is Transcript-Hash("") = a full-length digest, not zero bytes).
std::vector<std::uint8_t> ref_hash_empty(TlsHashAlgorithm hash) {
    std::vector<std::uint8_t> out(ref_md_len(hash));
    unsigned len = 0;
    EXPECT_EQ(1, EVP_Digest(nullptr, 0, out.data(), &len, ref_md(hash), nullptr));
    return out;
}

std::vector<std::uint8_t> concat(std::initializer_list<std::span<const std::uint8_t>> parts) {
    std::vector<std::uint8_t> out;
    for (std::span<const std::uint8_t> part: parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

// RFC 5869 HKDF-Expand: T(i) = HMAC(PRK, T(i-1) || info || i), counter from 1.
std::vector<std::uint8_t> ref_hkdf_expand(TlsHashAlgorithm hash, std::span<const std::uint8_t> prk,
                                          std::span<const std::uint8_t> info, std::size_t len) {
    std::vector<std::uint8_t> out;
    std::vector<std::uint8_t> t;
    std::uint8_t counter = 1;
    while (out.size() < len) {
        t = ref_hmac(hash, prk, concat({t, info, {&counter, 1}}));
        out.insert(out.end(), t.begin(), t.end());
        ++counter;
    }
    out.resize(len);
    return out;
}

// RFC 8446 §7.1 HKDF-Expand-Label; label WITHOUT the "tls13 " prefix.
std::vector<std::uint8_t> ref_expand_label(TlsHashAlgorithm hash, std::span<const std::uint8_t> secret,
                                           std::string_view label, std::span<const std::uint8_t> context,
                                           std::size_t len) {
    const std::string full_label = std::string("tls13 ").append(label);
    std::vector<std::uint8_t> info;
    info.push_back(static_cast<std::uint8_t>(len >> 8));
    info.push_back(static_cast<std::uint8_t>(len));
    info.push_back(static_cast<std::uint8_t>(full_label.size()));
    info.insert(info.end(), full_label.begin(), full_label.end());
    info.push_back(static_cast<std::uint8_t>(context.size()));
    info.insert(info.end(), context.begin(), context.end());
    return ref_hkdf_expand(hash, secret, info, len);
}

std::vector<std::uint8_t> label_seed(std::string_view label, std::span<const std::uint8_t> seed) {
    std::vector<std::uint8_t> out(label.begin(), label.end());
    out.insert(out.end(), seed.begin(), seed.end());
    return out;
}

// RFC 5246 §5 P_hash.
std::vector<std::uint8_t> ref_p_hash(TlsHashAlgorithm hash, std::span<const std::uint8_t> secret,
                                     std::span<const std::uint8_t> seed_label_seed, std::size_t len) {
    std::vector<std::uint8_t> a = ref_hmac(hash, secret, seed_label_seed);
    std::vector<std::uint8_t> out;
    while (out.size() < len) {
        const std::vector<std::uint8_t> t = ref_hmac(hash, secret, concat({a, seed_label_seed}));
        out.insert(out.end(), t.begin(), t.end());
        a = ref_hmac(hash, secret, a); // A-chain is independent of the T outputs
    }
    out.resize(len);
    return out;
}

// Runs the §3 tree and hands back the four traffic secrets.
struct Section3Result {
    TlsSecret c_hs, s_hs, c_ap, s_ap;
};

Section3Result run_section3_tree() {
    TlsKeySchedule13 ks(TlsCipherSuiteId::TlsAes128GcmSha256);
    Section3Result r;
    EXPECT_TRUE(
            ks.handshake_secrets(as_span(rfc8448::kS3Z), as_span(rfc8448::kS3HashChSh), r.c_hs, r.s_hs).has_value());
    EXPECT_TRUE(ks.application_secrets(as_span(rfc8448::kS3HashChServerFin), r.c_ap, r.s_ap).has_value());
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// TLS 1.3 staged machine — RFC 8448 KATs
// ---------------------------------------------------------------------------

TEST(TlsKeySchedule13, Rfc8448Section3Tree) {
    TlsKeySchedule13 ks(TlsCipherSuiteId::TlsAes128GcmSha256);

    TlsSecret c_hs, s_hs, c_ap, s_ap;
    ASSERT_TRUE(ks.handshake_secrets(as_span(rfc8448::kS3Z), as_span(rfc8448::kS3HashChSh), c_hs, s_hs).has_value());
    expect_secret(c_hs, as_span(rfc8448::kS3ClientHsSecret));
    expect_secret(s_hs, as_span(rfc8448::kS3ServerHsSecret));

    ASSERT_TRUE(ks.application_secrets(as_span(rfc8448::kS3HashChServerFin), c_ap, s_ap).has_value());
    expect_secret(c_ap, as_span(rfc8448::kS3ClientAppSecret));
    expect_secret(s_ap, as_span(rfc8448::kS3ServerAppSecret));

    auto resumption = ks.resumption_master_secret(as_span(rfc8448::kS3HashChClientFin));
    ASSERT_TRUE(resumption.has_value());
    expect_secret(*resumption, as_span(rfc8448::kS3ResMaster));
}

TEST(TlsKeySchedule13, EmptyPskEqualsNoPsk) {
    TlsKeySchedule13 plain(TlsCipherSuiteId::TlsAes128GcmSha256);
    TlsKeySchedule13 empty_psk(TlsCipherSuiteId::TlsAes128GcmSha256);
    TlsKeySchedule13 zero_psk(TlsCipherSuiteId::TlsAes128GcmSha256);
    ASSERT_TRUE(empty_psk.set_psk({}).has_value());
    ASSERT_TRUE(zero_psk.set_psk(std::vector<std::uint8_t>(32, 0)).has_value());

    TlsSecret c1, s1, c2, s2, c3, s3;
    ASSERT_TRUE(plain.handshake_secrets(as_span(rfc8448::kS3Z), as_span(rfc8448::kS3HashChSh), c1, s1).has_value());
    ASSERT_TRUE(empty_psk.handshake_secrets(as_span(rfc8448::kS3Z), as_span(rfc8448::kS3HashChSh), c2, s2).has_value());
    ASSERT_TRUE(zero_psk.handshake_secrets(as_span(rfc8448::kS3Z), as_span(rfc8448::kS3HashChSh), c3, s3).has_value());
    expect_eq_bytes(c1.bytes(), c2.bytes());
    expect_eq_bytes(s1.bytes(), s2.bytes());
    expect_eq_bytes(c1.bytes(), c3.bytes());
    expect_eq_bytes(s1.bytes(), s3.bytes());
}

TEST(TlsKeySchedule13Psk, Rfc8448Section4BinderAndEarlyTraffic) {
    TlsKeySchedule13 ks(TlsCipherSuiteId::TlsAes128GcmSha256);
    ASSERT_TRUE(ks.set_psk(as_span(rfc8448::kS4Psk)).has_value());

    auto binder = ks.binder_key(TlsPskBinderKind::Resumption);
    ASSERT_TRUE(binder.has_value());
    expect_secret(*binder, as_span(rfc8448::kS4BinderKey));

    // The binder's finished-key intermediate (the trace's "expanded" value).
    expect_eq_bytes(ref_expand_label(TlsHashAlgorithm::Sha256, binder->bytes(), "finished", {}, 32),
                    as_span(rfc8448::kS4BinderFinKey));

    std::array<std::uint8_t, 32> mac{};
    ASSERT_TRUE(tls13_psk_binder_mac(*binder, as_span(rfc8448::kS4HashTruncatedCh), mac).has_value());
    expect_eq_bytes(mac, as_span(rfc8448::kS4BinderMac));

    auto early = ks.client_early_traffic_secret(as_span(rfc8448::kS4HashCh));
    ASSERT_TRUE(early.has_value());
    expect_secret(*early, as_span(rfc8448::kS4ClientEarlySecret));

    auto keys = tls13_traffic_keys(*early, TlsCipherSuiteId::TlsAes128GcmSha256);
    ASSERT_TRUE(keys.has_value());
    expect_eq_bytes({keys->key.data(), keys->key_len}, as_span(rfc8448::kS4EarlyKey));
    expect_eq_bytes({keys->iv.data(), keys->iv_len}, as_span(rfc8448::kS4EarlyIv));
}

TEST(TlsKeySchedule13Psk, ExternalBinderDiffersFromResumptionBinder) {
    TlsKeySchedule13 ext_ks(TlsCipherSuiteId::TlsAes128GcmSha256);
    TlsKeySchedule13 res_ks(TlsCipherSuiteId::TlsAes128GcmSha256);
    ASSERT_TRUE(ext_ks.set_psk(as_span(rfc8448::kS4Psk)).has_value());
    ASSERT_TRUE(res_ks.set_psk(as_span(rfc8448::kS4Psk)).has_value());
    auto ext = ext_ks.binder_key(TlsPskBinderKind::External);
    auto res = res_ks.binder_key(TlsPskBinderKind::Resumption);
    ASSERT_TRUE(ext.has_value());
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(32u, ext->len());
    EXPECT_NE(0, std::memcmp(ext->bytes().data(), res->bytes().data(), 32));
}

// Calibrates the independent reference against vectors straight from the RFC
// before it is trusted as the SHA-384 / TLS 1.2 oracle: the no-PSK early
// extract (IKM = zeros(hash_len), NOT empty) and the binder key hop (context
// = Hash(""), NOT empty).
TEST(TlsKeyScheduleReference, MatchesRfc8448CalibrationVectors) {
    const std::vector<std::uint8_t> zeros(32, 0);
    expect_eq_bytes(ref_hmac(TlsHashAlgorithm::Sha256, zeros, zeros), as_span(rfc8448::kS3EarlySecret));
    expect_eq_bytes(ref_hmac(TlsHashAlgorithm::Sha256, zeros, as_span(rfc8448::kS4Psk)),
                    as_span(rfc8448::kS4EarlySecret));
    expect_eq_bytes(ref_expand_label(TlsHashAlgorithm::Sha256, as_span(rfc8448::kS4EarlySecret), "res binder",
                                     ref_hash_empty(TlsHashAlgorithm::Sha256), 32),
                    as_span(rfc8448::kS4BinderKey));
    expect_eq_bytes(ref_expand_label(TlsHashAlgorithm::Sha256, as_span(rfc8448::kS3EarlySecret), "derived",
                                     ref_hash_empty(TlsHashAlgorithm::Sha256), 32),
                    as_span(rfc8448::kS3DerivedHs));
}

// RFC 8448 has no SHA-384 trace; the whole tree is checked against the
// hand-rolled reference instead (no-PSK extract = HMAC(0^48, 0^48), derived
// and master contexts = Hash(""), master IKM = zeros(48)).
TEST(TlsKeySchedule13, Sha384TreeMatchesIndependentReference) {
    const auto h = TlsHashAlgorithm::Sha384;
    const auto z = ramp(32, 0x10);
    const auto hash_ch_sh = ramp(48, 0x20);
    const auto hash_ch_sf = ramp(48, 0x30);
    const auto hash_ch_cf = ramp(48, 0x40);

    const std::vector<std::uint8_t> zeros(48, 0);
    const auto empty_hash = ref_hash_empty(h);
    const auto early = ref_hmac(h, zeros, zeros);
    const auto derived1 = ref_expand_label(h, early, "derived", empty_hash, 48);
    const auto hs = ref_hmac(h, derived1, z);
    const auto ref_c_hs = ref_expand_label(h, hs, "c hs traffic", hash_ch_sh, 48);
    const auto ref_s_hs = ref_expand_label(h, hs, "s hs traffic", hash_ch_sh, 48);
    const auto derived2 = ref_expand_label(h, hs, "derived", empty_hash, 48);
    const auto master = ref_hmac(h, derived2, zeros);
    const auto ref_c_ap = ref_expand_label(h, master, "c ap traffic", hash_ch_sf, 48);
    const auto ref_s_ap = ref_expand_label(h, master, "s ap traffic", hash_ch_sf, 48);
    const auto ref_res = ref_expand_label(h, master, "res master", hash_ch_cf, 48);

    TlsKeySchedule13 ks(TlsCipherSuiteId::TlsAes256GcmSha384);
    TlsSecret c_hs, s_hs, c_ap, s_ap;
    ASSERT_TRUE(ks.handshake_secrets(z, hash_ch_sh, c_hs, s_hs).has_value());
    ASSERT_TRUE(ks.application_secrets(hash_ch_sf, c_ap, s_ap).has_value());
    auto res = ks.resumption_master_secret(hash_ch_cf);
    ASSERT_TRUE(res.has_value());

    expect_eq_bytes(c_hs.bytes(), ref_c_hs);
    expect_eq_bytes(s_hs.bytes(), ref_s_hs);
    expect_eq_bytes(c_ap.bytes(), ref_c_ap);
    expect_eq_bytes(s_ap.bytes(), ref_s_ap);
    expect_eq_bytes(res->bytes(), ref_res);
}

// ---------------------------------------------------------------------------
// TLS 1.3 free functions
// ---------------------------------------------------------------------------

TEST(Tls13TrafficKeys, Rfc8448Section3WriteKeys) {
    const Section3Result r = run_section3_tree();
    const auto suite = TlsCipherSuiteId::TlsAes128GcmSha256;

    const auto check = [&](const TlsSecret &secret, std::span<const std::uint8_t> want_key,
                           std::span<const std::uint8_t> want_iv) {
        auto keys = tls13_traffic_keys(secret, suite);
        ASSERT_TRUE(keys.has_value());
        EXPECT_EQ(16, keys->key_len);
        EXPECT_EQ(12, keys->iv_len);
        expect_eq_bytes({keys->key.data(), keys->key_len}, want_key);
        expect_eq_bytes({keys->iv.data(), keys->iv_len}, want_iv);
    };
    check(r.c_hs, as_span(rfc8448::kS3ClientHsKey), as_span(rfc8448::kS3ClientHsIv));
    check(r.s_hs, as_span(rfc8448::kS3ServerHsKey), as_span(rfc8448::kS3ServerHsIv));
    check(r.c_ap, as_span(rfc8448::kS3ClientAppKey), as_span(rfc8448::kS3ClientAppIv));
    check(r.s_ap, as_span(rfc8448::kS3ServerAppKey), as_span(rfc8448::kS3ServerAppIv));
}

TEST(Tls13TrafficKeys, ChaCha20SuiteMatchesReference) {
    const TlsSecret secret = TlsSecret::from_bytes(ramp(32, 0x70));
    auto keys = tls13_traffic_keys(secret, TlsCipherSuiteId::TlsChacha20Poly1305Sha256);
    ASSERT_TRUE(keys.has_value());
    EXPECT_EQ(32, keys->key_len);
    EXPECT_EQ(12, keys->iv_len);
    expect_eq_bytes({keys->key.data(), keys->key_len},
                    ref_expand_label(TlsHashAlgorithm::Sha256, secret.bytes(), "key", {}, 32));
    expect_eq_bytes({keys->iv.data(), keys->iv_len},
                    ref_expand_label(TlsHashAlgorithm::Sha256, secret.bytes(), "iv", {}, 12));
}

// The schedule-derived write keys must actually protect records through the
// record cipher, with both directions of the sequence-number nonce.
TEST(Tls13TrafficKeys, Rfc8448Section3KeysProtectRecords) {
    const Section3Result r = run_section3_tree();
    auto keys = tls13_traffic_keys(r.c_hs, TlsCipherSuiteId::TlsAes128GcmSha256);
    ASSERT_TRUE(keys.has_value());

    TlsRecordCipher writer, reader;
    ASSERT_TRUE(writer.init(TlsCipherSuiteId::TlsAes128GcmSha256, TlsRecordProtectionKind::Tls13,
                            {keys->key.data(), keys->key_len}, {keys->iv.data(), keys->iv_len})
                        .has_value());
    ASSERT_TRUE(reader.init(TlsCipherSuiteId::TlsAes128GcmSha256, TlsRecordProtectionKind::Tls13,
                            {keys->key.data(), keys->key_len}, {keys->iv.data(), keys->iv_len})
                        .has_value());

    const std::vector<std::uint8_t> first = ramp(5, 0);
    const std::vector<std::uint8_t> second = ramp(300, 0);
    std::array<std::uint8_t, 5 + 17> sealed1{};
    std::array<std::uint8_t, 300 + 17> sealed2{};

    auto s1 = writer.seal(TlsContentType::Handshake, first, sealed1);
    ASSERT_EQ(TlsRecordCipher::Status::Ok, s1.status);
    auto s2 = writer.seal(TlsContentType::ApplicationData, second, sealed2);
    ASSERT_EQ(TlsRecordCipher::Status::Ok, s2.status);

    // 1.3 open needs ciphertext_len - 16 capacity (plaintext + inner type);
    // 317-byte records therefore need 301 bytes even though only 300 land.
    std::array<std::uint8_t, 301> out{};
    auto o1 = reader.open(TlsContentType::ApplicationData, 0x0303, static_cast<std::uint16_t>(sealed1.size()), sealed1,
                          out);
    ASSERT_EQ(TlsRecordCipher::Status::Ok, o1.status);
    EXPECT_EQ(TlsContentType::Handshake, o1.inner_type);
    EXPECT_EQ(5u, o1.plain_len);

    auto o2 = reader.open(TlsContentType::ApplicationData, 0x0303, static_cast<std::uint16_t>(sealed2.size()), sealed2,
                          out);
    ASSERT_EQ(TlsRecordCipher::Status::Ok, o2.status);
    EXPECT_EQ(TlsContentType::ApplicationData, o2.inner_type);
    EXPECT_EQ(300u, o2.plain_len);
    EXPECT_EQ(0, std::memcmp(out.data(), second.data(), second.size()));

    // A fresh reader (seq 0) cannot open the seq-1 record: nonce binding.
    TlsRecordCipher desynced;
    ASSERT_TRUE(desynced.init(TlsCipherSuiteId::TlsAes128GcmSha256, TlsRecordProtectionKind::Tls13,
                              {keys->key.data(), keys->key_len}, {keys->iv.data(), keys->iv_len})
                        .has_value());
    auto o3 = desynced.open(TlsContentType::ApplicationData, 0x0303, static_cast<std::uint16_t>(sealed2.size()),
                            sealed2, out);
    EXPECT_EQ(TlsRecordCipher::Status::AuthFail, o3.status);
}

// Both Finished directions pinned by trace values: client over
// Hash(CH..server Fin), server over Hash(CH..server CertificateVerify).
TEST(Tls13Finished, Rfc8448Section3BothDirections) {
    const Section3Result r = run_section3_tree();

    // Intermediates: the finished keys expanded from the traffic secrets.
    expect_eq_bytes(ref_expand_label(TlsHashAlgorithm::Sha256, r.c_hs.bytes(), "finished", {}, 32),
                    as_span(rfc8448::kS3ClientFinKey));
    expect_eq_bytes(ref_expand_label(TlsHashAlgorithm::Sha256, r.s_hs.bytes(), "finished", {}, 32),
                    as_span(rfc8448::kS3ServerFinKey));

    std::array<std::uint8_t, 32> cli_mac{};
    ASSERT_TRUE(tls13_finished_mac(r.c_hs, as_span(rfc8448::kS3HashChServerFin), cli_mac).has_value());
    expect_eq_bytes(cli_mac, as_span(rfc8448::kS3ClientFinMac));

    std::array<std::uint8_t, 32> srv_mac{};
    ASSERT_TRUE(tls13_finished_mac(r.s_hs, as_span(rfc8448::kS3HashChServerCv), srv_mac).has_value());
    expect_eq_bytes(srv_mac, as_span(rfc8448::kS3ServerFinMac));
}

TEST(Tls13KeyUpdate, MatchesReferenceAndChains) {
    const auto check = [](std::size_t len, TlsHashAlgorithm hash) {
        const TlsSecret s0 = TlsSecret::from_bytes(ramp(len, 0x50));
        auto s1 = tls13_key_update(s0);
        ASSERT_TRUE(s1.has_value());
        expect_eq_bytes(s1->bytes(), ref_expand_label(hash, s0.bytes(), "traffic upd", {}, len));
        auto s2 = tls13_key_update(*s1);
        ASSERT_TRUE(s2.has_value());
        expect_eq_bytes(s2->bytes(), ref_expand_label(hash, s1->bytes(), "traffic upd", {}, len));
        EXPECT_NE(0, std::memcmp(s1->bytes().data(), s2->bytes().data(), len));
    };
    check(32, TlsHashAlgorithm::Sha256);
    check(48, TlsHashAlgorithm::Sha384);
}

// ---------------------------------------------------------------------------
// TLS 1.2 PRF
// ---------------------------------------------------------------------------

TEST(Tls12Prf, MasterKeyBlockVerifyDataMatchReference) {
    const struct {
        TlsCipherSuiteId suite;
        TlsHashAlgorithm hash;
        std::uint8_t key_len;
    } cases[] = {
            {TlsCipherSuiteId::EcdheRsaAes128GcmSha256, TlsHashAlgorithm::Sha256, 16},
            {TlsCipherSuiteId::EcdheRsaAes256GcmSha384, TlsHashAlgorithm::Sha384, 32},
    };
    for (const auto &c: cases) {
        const auto z = ramp(32, 0x60);
        const auto cr = ramp(32, 0x70);
        const auto sr = ramp(32, 0x80);
        const auto hh = ramp(ref_md_len(c.hash), 0x90);

        auto master = tls12_master_secret(c.suite, z, cr, sr);
        ASSERT_TRUE(master.has_value());
        EXPECT_EQ(48u, master->len());
        expect_eq_bytes(master->bytes(), ref_p_hash(c.hash, z, label_seed("master secret", concat({cr, sr})), 48));

        const std::size_t block_len = 2 * c.key_len + 8;
        const auto ref_block =
                ref_p_hash(c.hash, master->bytes(), label_seed("key expansion", concat({sr, cr})), block_len);
        auto keys = tls12_key_block(c.suite, *master, cr, sr);
        ASSERT_TRUE(keys.has_value());
        expect_eq_bytes({keys->client.key.data(), c.key_len}, {ref_block.data(), c.key_len});
        expect_eq_bytes({keys->server.key.data(), c.key_len}, {ref_block.data() + c.key_len, c.key_len});
        expect_eq_bytes({keys->client.iv.data(), 4}, {ref_block.data() + 2 * c.key_len, 4});
        expect_eq_bytes({keys->server.iv.data(), 4}, {ref_block.data() + 2 * c.key_len + 4, 4});

        auto vd_c = tls12_verify_data(c.suite, *master, true, hh);
        ASSERT_TRUE(vd_c.has_value());
        EXPECT_EQ(12u, vd_c->size());
        expect_eq_bytes(*vd_c, ref_p_hash(c.hash, master->bytes(), label_seed("client finished", hh), 12));
        auto vd_s = tls12_verify_data(c.suite, *master, false, hh);
        ASSERT_TRUE(vd_s.has_value());
        expect_eq_bytes(*vd_s, ref_p_hash(c.hash, master->bytes(), label_seed("server finished", hh), 12));
    }
}

// RFC 7627: extended_master_secret replaces the randoms seed with the
// handshake-hash snapshot and the label — both must show up in the output.
TEST(Tls12Prf, ExtendedMasterSecretMatchesReference) {
    const struct {
        TlsCipherSuiteId suite;
        TlsHashAlgorithm hash;
    } cases[] = {
            {TlsCipherSuiteId::EcdheRsaAes128GcmSha256, TlsHashAlgorithm::Sha256},
            {TlsCipherSuiteId::EcdheRsaAes256GcmSha384, TlsHashAlgorithm::Sha384},
    };
    for (const auto &c: cases) {
        const auto z = ramp(32, 0xA0);
        const auto session_hash = ramp(ref_md_len(c.hash), 0xB0);
        const auto cr = ramp(32, 0x70);
        const auto sr = ramp(32, 0x80);

        auto ems = tls12_extended_master_secret(c.suite, z, session_hash);
        ASSERT_TRUE(ems.has_value());
        EXPECT_EQ(48u, ems->len());
        expect_eq_bytes(ems->bytes(), ref_p_hash(c.hash, z, label_seed("extended master secret", session_hash), 48));

        // A distinct construction from the classic master secret (label and
        // seed both differ), and the session_hash actually feeds it.
        auto classic = tls12_master_secret(c.suite, z, cr, sr);
        ASSERT_TRUE(classic.has_value());
        EXPECT_NE(0, std::memcmp(ems->bytes().data(), classic->bytes().data(), 48));
        auto other_hash = tls12_extended_master_secret(c.suite, z, ramp(ref_md_len(c.hash), 0xB1));
        ASSERT_TRUE(other_hash.has_value());
        EXPECT_NE(0, std::memcmp(ems->bytes().data(), other_hash->bytes().data(), 48));
    }
}

// The pair must be coherent protection material and the two directions must
// differ: the peer (holding the CLIENT's write keys) opens the client's
// record, a cipher holding the SERVER's write keys must not.
TEST(Tls12KeyBlock, DirectionalMaterialPinnedByRecordCipher) {
    const auto suite = TlsCipherSuiteId::EcdheRsaAes128GcmSha256;
    const auto z = ramp(32, 1);
    const auto cr = ramp(32, 2);
    const auto sr = ramp(32, 3);
    auto master = tls12_master_secret(suite, z, cr, sr);
    ASSERT_TRUE(master.has_value());
    auto keys = tls12_key_block(suite, *master, cr, sr);
    ASSERT_TRUE(keys.has_value());

    TlsRecordCipher writer, peer_reader, wrong_reader;
    ASSERT_TRUE(writer.init(suite, TlsRecordProtectionKind::Tls12, {keys->client.key.data(), keys->client.key_len},
                            {keys->client.iv.data(), 4})
                        .has_value());
    ASSERT_TRUE(peer_reader
                        .init(suite, TlsRecordProtectionKind::Tls12, {keys->client.key.data(), keys->client.key_len},
                              {keys->client.iv.data(), 4})
                        .has_value());
    ASSERT_TRUE(wrong_reader
                        .init(suite, TlsRecordProtectionKind::Tls12, {keys->server.key.data(), keys->server.key_len},
                              {keys->server.iv.data(), 4})
                        .has_value());

    const auto plain = ramp(40, 0xAA);
    std::array<std::uint8_t, 40 + 24> sealed{};
    auto s = writer.seal(TlsContentType::ApplicationData, plain, sealed);
    ASSERT_EQ(TlsRecordCipher::Status::Ok, s.status);
    ASSERT_EQ(sealed.size(), s.out_len);

    std::array<std::uint8_t, 40> out{};
    auto o1 = peer_reader.open(TlsContentType::ApplicationData, 0x0303, static_cast<std::uint16_t>(sealed.size()),
                               sealed, out);
    ASSERT_EQ(TlsRecordCipher::Status::Ok, o1.status);
    ASSERT_EQ(40u, o1.plain_len);
    EXPECT_EQ(0, std::memcmp(out.data(), plain.data(), plain.size()));

    auto o2 = wrong_reader.open(TlsContentType::ApplicationData, 0x0303, static_cast<std::uint16_t>(sealed.size()),
                                sealed, out);
    EXPECT_EQ(TlsRecordCipher::Status::AuthFail, o2.status);
}

// ---------------------------------------------------------------------------
// Suite registry
// ---------------------------------------------------------------------------

TEST(TlsSuiteRegistry, CoversTheNineImplementedSuites) {
    EXPECT_EQ(nullptr, tls_suite_info(static_cast<TlsCipherSuiteId>(0x0000)));
    EXPECT_EQ(9u, kTlsSuiteRegistry.size());

    const auto check = [](TlsCipherSuiteId suite, TlsAeadAlgorithm aead, TlsHashAlgorithm hash, std::uint8_t key_len,
                          bool tls13) {
        const TlsSuiteInfo *info = tls_suite_info(suite);
        ASSERT_NE(nullptr, info);
        EXPECT_EQ(suite, info->suite);
        EXPECT_EQ(aead, info->aead);
        EXPECT_EQ(hash, info->hash);
        EXPECT_EQ(key_len, info->key_len);
        EXPECT_EQ(tls13, info->is_tls13);
    };
    check(TlsCipherSuiteId::TlsAes128GcmSha256, TlsAeadAlgorithm::Aes128Gcm, TlsHashAlgorithm::Sha256, 16, true);
    check(TlsCipherSuiteId::TlsAes256GcmSha384, TlsAeadAlgorithm::Aes256Gcm, TlsHashAlgorithm::Sha384, 32, true);
    check(TlsCipherSuiteId::TlsChacha20Poly1305Sha256, TlsAeadAlgorithm::Chacha20Poly1305, TlsHashAlgorithm::Sha256, 32,
          true);
    check(TlsCipherSuiteId::EcdheEcdsaAes128GcmSha256, TlsAeadAlgorithm::Aes128Gcm, TlsHashAlgorithm::Sha256, 16,
          false);
    check(TlsCipherSuiteId::EcdheEcdsaAes256GcmSha384, TlsAeadAlgorithm::Aes256Gcm, TlsHashAlgorithm::Sha384, 32,
          false);
    check(TlsCipherSuiteId::EcdheRsaAes128GcmSha256, TlsAeadAlgorithm::Aes128Gcm, TlsHashAlgorithm::Sha256, 16, false);
    check(TlsCipherSuiteId::EcdheRsaAes256GcmSha384, TlsAeadAlgorithm::Aes256Gcm, TlsHashAlgorithm::Sha384, 32, false);
    check(TlsCipherSuiteId::EcdheEcdsaChacha20Poly1305, TlsAeadAlgorithm::Chacha20Poly1305, TlsHashAlgorithm::Sha256,
          32, false);
    check(TlsCipherSuiteId::EcdheRsaChacha20Poly1305, TlsAeadAlgorithm::Chacha20Poly1305, TlsHashAlgorithm::Sha256, 32,
          false);
}

// ---------------------------------------------------------------------------
// Contract violations are engine bugs — FIBER_ASSERT
// ---------------------------------------------------------------------------

TEST(TlsKeySchedule13Death, ContractViolations) {
    EXPECT_DEATH((void) TlsKeySchedule13(TlsCipherSuiteId::EcdheRsaAes128GcmSha256), "FIBER_ASSERT failed");

    EXPECT_DEATH(
            {
                TlsKeySchedule13 ks(TlsCipherSuiteId::TlsAes128GcmSha256);
                (void) ks.binder_key(TlsPskBinderKind::Resumption); // set_psk never ran
            },
            "FIBER_ASSERT failed");

    EXPECT_DEATH(
            {
                TlsKeySchedule13 ks(TlsCipherSuiteId::TlsAes128GcmSha256);
                (void) ks.set_psk(as_span(rfc8448::kS4Psk));
                (void) ks.set_psk(as_span(rfc8448::kS4Psk)); // twice
            },
            "FIBER_ASSERT failed");

    EXPECT_DEATH(
            {
                TlsKeySchedule13 ks(TlsCipherSuiteId::TlsAes128GcmSha256);
                (void) ks.set_psk(as_span(rfc8448::kS4Psk));
                auto first = ks.binder_key(TlsPskBinderKind::Resumption);
                (void) first;
                auto second = ks.binder_key(TlsPskBinderKind::External); // twice
                (void) second;
            },
            "FIBER_ASSERT failed");

    EXPECT_DEATH(
            {
                TlsKeySchedule13 ks(TlsCipherSuiteId::TlsAes128GcmSha256);
                TlsSecret c;
                TlsSecret s;
                (void) ks.handshake_secrets(ramp(31, 0), as_span(rfc8448::kS3HashChSh), c, s); // z != 32
            },
            "FIBER_ASSERT failed");

    EXPECT_DEATH(
            {
                TlsKeySchedule13 ks(TlsCipherSuiteId::TlsAes128GcmSha256);
                TlsSecret c;
                TlsSecret s;
                (void) ks.application_secrets(as_span(rfc8448::kS3HashChServerFin), c, s); // wrong stage
            },
            "FIBER_ASSERT failed");

    EXPECT_DEATH(
            {
                TlsKeySchedule13 ks(TlsCipherSuiteId::TlsAes128GcmSha256);
                TlsSecret c;
                TlsSecret s;
                (void) ks.handshake_secrets(as_span(rfc8448::kS3Z), as_span(rfc8448::kS3HashChSh), c, s);
                (void) ks.application_secrets(as_span(rfc8448::kS3HashChServerFin), c, s);
                (void) ks.application_secrets(as_span(rfc8448::kS3HashChServerFin), c, s); // twice
            },
            "FIBER_ASSERT failed");

    EXPECT_DEATH(
            {
                TlsKeySchedule13 ks(TlsCipherSuiteId::TlsAes128GcmSha256);
                TlsSecret c;
                TlsSecret s;
                (void) ks.handshake_secrets(as_span(rfc8448::kS3Z), as_span(rfc8448::kS3HashChSh), c, s);
                (void) ks.application_secrets(as_span(rfc8448::kS3HashChServerFin), c, s);
                auto first = ks.resumption_master_secret(as_span(rfc8448::kS3HashChClientFin));
                (void) first;
                auto second = ks.resumption_master_secret(as_span(rfc8448::kS3HashChClientFin)); // twice
                (void) second;
            },
            "FIBER_ASSERT failed");

    EXPECT_DEATH(
            {
                TlsKeySchedule13 ks(TlsCipherSuiteId::TlsAes128GcmSha256);
                TlsSecret c;
                TlsSecret s;
                (void) ks.handshake_secrets(as_span(rfc8448::kS3Z), as_span(rfc8448::kS3HashChSh), c, s);
                auto keys = tls13_traffic_keys(c, TlsCipherSuiteId::EcdheRsaAes128GcmSha256); // 1.2 suite
                (void) keys;
            },
            "FIBER_ASSERT failed");

    EXPECT_DEATH((void) TlsSecret::from_bytes(ramp(49, 0)), "FIBER_ASSERT failed");
}
