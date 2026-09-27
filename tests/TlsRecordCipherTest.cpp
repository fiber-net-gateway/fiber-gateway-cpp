#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <openssl/aead.h>

#include <fiber/common/mem/IoBuf.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/tls/record/TlsRecord.h>
#include <fiber/tls/record/TlsRecordCipher.h>
#include <fiber/tls/record/TlsRecordReader.h>
#include <fiber/tls/record/TlsRecordWriter.h>
#include "LoopTestSupport.h"

// RFC 8448 vectors were unavailable offline; instead every fixed-expectation
// test cross-checks TlsRecordCipher against an INDEPENDENT construction:
// this file builds the expected wire bytes with a bare EVP_AEAD_CTX, with
// the nonce / AAD / payload layout assembled BY HAND per RFC 8446 §5.2 and
// RFC 5288 §3 — sharing no code path with the unit under test. TODO: add the
// literal RFC 8448 §3 traces once network access allows fetching them.

namespace {

using fiber::tls::TlsCipherSuiteId;
using fiber::tls::TlsContentType;
using fiber::tls::TlsRecordCipher;
using fiber::tls::TlsRecordProtectionKind;

std::vector<std::uint8_t> ramp(std::size_t len, std::uint8_t seed) {
    std::vector<std::uint8_t> out(len);
    for (std::size_t i = 0; i < len; ++i) {
        out[i] = static_cast<std::uint8_t>(seed + i);
    }
    return out;
}

std::vector<std::uint8_t> key_iv(const char *hex) {
    std::vector<std::uint8_t> out;
    for (; hex[0] && hex[1]; hex += 2) {
        out.push_back(static_cast<std::uint8_t>(std::strtoul(hex, nullptr, 16)));
    }
    return out;
}

void store_be64(std::uint8_t *dst, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        dst[i] = static_cast<std::uint8_t>(value);
        value >>= 8;
    }
}

// ---- independent expected-value builders (bare EVP, hand-assembled wire) ----

const EVP_AEAD *aead_for(TlsCipherSuiteId suite) {
    switch (suite) {
        case TlsCipherSuiteId::TlsAes128GcmSha256:
        case TlsCipherSuiteId::EcdheEcdsaAes128GcmSha256:
        case TlsCipherSuiteId::EcdheRsaAes128GcmSha256:
            return EVP_aead_aes_128_gcm();
        case TlsCipherSuiteId::TlsAes256GcmSha384:
        case TlsCipherSuiteId::EcdheEcdsaAes256GcmSha384:
        case TlsCipherSuiteId::EcdheRsaAes256GcmSha384:
            return EVP_aead_aes_256_gcm();
        case TlsCipherSuiteId::TlsChacha20Poly1305Sha256:
        case TlsCipherSuiteId::EcdheEcdsaChacha20Poly1305:
        case TlsCipherSuiteId::EcdheRsaChacha20Poly1305:
            return EVP_aead_chacha20_poly1305();
    }
    return nullptr;
}

// RFC 8446 §5.2: nonce = static_iv XOR BE64(seq) in the low 8 bytes;
// AAD = record header (23, 0x0303, plain+1+16); AEAD input = plaintext ||
// inner_type. Returns the protected payload (with tag), length plain.size()+17.
std::vector<std::uint8_t> expected_tls13_payload(TlsCipherSuiteId suite, const std::vector<std::uint8_t> &key,
                                                 const std::vector<std::uint8_t> &iv, std::uint64_t seq,
                                                 TlsContentType inner_type, const std::vector<std::uint8_t> &plain) {
    std::array<std::uint8_t, 12> nonce{};
    std::memcpy(nonce.data(), iv.data(), 12);
    nonce[4] ^= static_cast<std::uint8_t>(seq >> 56);
    nonce[5] ^= static_cast<std::uint8_t>(seq >> 48);
    nonce[6] ^= static_cast<std::uint8_t>(seq >> 40);
    nonce[7] ^= static_cast<std::uint8_t>(seq >> 32);
    nonce[8] ^= static_cast<std::uint8_t>(seq >> 24);
    nonce[9] ^= static_cast<std::uint8_t>(seq >> 16);
    nonce[10] ^= static_cast<std::uint8_t>(seq >> 8);
    nonce[11] ^= static_cast<std::uint8_t>(seq);

    const std::size_t out_len = plain.size() + 17;
    std::array<std::uint8_t, 5> aad{23, 0x03, 0x03, static_cast<std::uint8_t>(out_len >> 8),
                                    static_cast<std::uint8_t>(out_len)};

    std::vector<std::uint8_t> input = plain;
    input.push_back(static_cast<std::uint8_t>(inner_type));

    EVP_AEAD_CTX ctx;
    EVP_AEAD_CTX_zero(&ctx);
    EXPECT_EQ(EVP_AEAD_CTX_init(&ctx, aead_for(suite), key.data(), key.size(), EVP_AEAD_DEFAULT_TAG_LENGTH, nullptr),
              1);
    std::vector<std::uint8_t> out(out_len);
    size_t written = 0;
    EXPECT_EQ(EVP_AEAD_CTX_seal(&ctx, out.data(), &written, out.size(), nonce.data(), 12, input.data(), input.size(),
                                aad.data(), aad.size()),
              1);
    EVP_AEAD_CTX_cleanup(&ctx);
    EXPECT_EQ(written, out_len);
    return out;
}

// RFC 5288 §3 (GCM suites): nonce = fixed_iv || BE64(seq); AAD = BE64(seq) ||
// type || 0x0303 || BE16(plain_len); wire = BE64(seq) || AEAD(plain) || tag.
// RFC 7905 §2 (ChaCha20 suites): nonce = the full 12-byte iv XOR BE64(seq) in
// its low half (the 1.3 construction); NO nonce bytes on the wire, so the
// wire is AEAD(plain) || tag alone (plain + 16).
std::vector<std::uint8_t> expected_tls12_payload(TlsCipherSuiteId suite, const std::vector<std::uint8_t> &key,
                                                 const std::vector<std::uint8_t> &fixed_iv, std::uint64_t seq,
                                                 TlsContentType inner_type, const std::vector<std::uint8_t> &plain) {
    const bool chacha = suite == TlsCipherSuiteId::EcdheRsaChacha20Poly1305 ||
                        suite == TlsCipherSuiteId::EcdheEcdsaChacha20Poly1305;
    std::array<std::uint8_t, 12> nonce{};
    std::memcpy(nonce.data(), fixed_iv.data(), chacha ? 12 : 4);
    if (chacha) {
        std::array<std::uint8_t, 8> seq_be{};
        store_be64(seq_be.data(), seq);
        for (int i = 0; i < 8; ++i) {
            nonce[4 + i] ^= seq_be[i];
        }
    } else {
        store_be64(nonce.data() + 4, seq);
    }

    std::array<std::uint8_t, 13> aad{};
    store_be64(aad.data(), seq);
    aad[8] = static_cast<std::uint8_t>(inner_type);
    aad[9] = 0x03;
    aad[10] = 0x03;
    aad[11] = static_cast<std::uint8_t>(plain.size() >> 8);
    aad[12] = static_cast<std::uint8_t>(plain.size());

    EVP_AEAD_CTX ctx;
    EVP_AEAD_CTX_zero(&ctx);
    EXPECT_EQ(EVP_AEAD_CTX_init(&ctx, aead_for(suite), key.data(), key.size(), EVP_AEAD_DEFAULT_TAG_LENGTH, nullptr),
              1);
    std::vector<std::uint8_t> sealed(plain.size() + 16);
    size_t written = 0;
    EXPECT_EQ(EVP_AEAD_CTX_seal(&ctx, sealed.data(), &written, sealed.size(), nonce.data(), 12, plain.data(),
                                plain.size(), aad.data(), aad.size()),
              1);
    EVP_AEAD_CTX_cleanup(&ctx);
    EXPECT_EQ(written, plain.size() + 16);

    if (chacha) {
        return sealed;
    }
    std::vector<std::uint8_t> out(plain.size() + 24);
    store_be64(out.data(), seq);
    std::memcpy(out.data() + 8, sealed.data(), sealed.size());
    return out;
}

struct SuiteVectors {
    TlsCipherSuiteId suite;
    TlsRecordProtectionKind kind;
    std::vector<std::uint8_t> key;
    std::vector<std::uint8_t> iv;
};

const std::vector<SuiteVectors> &tls13_suites() {
    static const std::vector<SuiteVectors> suites = {
            {TlsCipherSuiteId::TlsAes128GcmSha256, TlsRecordProtectionKind::Tls13,
             key_iv("3fce516009c21727d0f2e4e86ee403bc"), key_iv("5d313eb2671276ee13000b30")},
            {TlsCipherSuiteId::TlsAes256GcmSha384, TlsRecordProtectionKind::Tls13,
             key_iv("dbfaa693d1762c5b666af5d950258d019f02283b6c9c07efc26bb9f2ac92e356"),
             key_iv("5bd3c71b836e0b76bb73265f")},
            {TlsCipherSuiteId::TlsChacha20Poly1305Sha256, TlsRecordProtectionKind::Tls13,
             key_iv("17422dda596ed5d9acd890e3c63f50519f02283b6c9c07efc26bb9f2ac92e356"),
             key_iv("5b78923dee08579033e523d9")},
    };
    return suites;
}

const std::vector<SuiteVectors> &tls12_suites() {
    static const std::vector<SuiteVectors> suites = {
            {TlsCipherSuiteId::EcdheRsaAes128GcmSha256, TlsRecordProtectionKind::Tls12,
             key_iv("0102030405060708090a0b0c0d0e0f10"), key_iv("11121314")},
            {TlsCipherSuiteId::EcdheRsaAes256GcmSha384, TlsRecordProtectionKind::Tls12,
             key_iv("0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"), key_iv("11121314")},
            {TlsCipherSuiteId::EcdheRsaChacha20Poly1305, TlsRecordProtectionKind::Tls12,
             key_iv("21022dda596ed5d9acd890e3c63f5051a1b2c3d4e5f60718293a4b5c6d7e8f90"),
             key_iv("1112131415161718191a1b1c")}, // RFC 7905: 12-byte implicit IV
    };
    return suites;
}

void init_cipher(TlsRecordCipher &cipher, const SuiteVectors &v) {
    EXPECT_TRUE(cipher.init(v.suite, v.kind, v.key, v.iv).has_value());
}

const std::vector<SuiteVectors> &all_suites(); // defined below, beside the scatter tests

// The wire form's explicit-nonce prefix length for a suite/kind pair: 8 for
// 1.2 GCM (RFC 5288), 0 for 1.3 and 1.2 ChaCha20 (RFC 7905 §2).
std::size_t wire_nonce_len(const SuiteVectors &v) {
    if (v.kind == TlsRecordProtectionKind::Tls13) {
        return 0;
    }
    switch (v.suite) {
        case TlsCipherSuiteId::EcdheRsaChacha20Poly1305:
        case TlsCipherSuiteId::EcdheEcdsaChacha20Poly1305:
            return 0;
        default:
            return 8;
    }
}

// ---------------------------------------------------------------- init

TEST(TlsRecordCipherInit, RejectsBadPairingAndLengths) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        const auto key16 = key_iv("000102030405060708090a0b0c0d0e0f");
        const auto iv12 = key_iv("000102030405060708090a0b");
        const auto iv4 = key_iv("00010203");

        TlsRecordCipher mixed;
        EXPECT_EQ(mixed.init(TlsCipherSuiteId::TlsAes128GcmSha256, TlsRecordProtectionKind::Tls12, key16, iv4).error(),
                  fiber::common::IoErr::Invalid);
        EXPECT_EQ(mixed.init(TlsCipherSuiteId::EcdheRsaAes128GcmSha256, TlsRecordProtectionKind::Tls13, key16, iv12)
                          .error(),
                  fiber::common::IoErr::Invalid);

        TlsRecordCipher bad_len;
        EXPECT_EQ(
                bad_len.init(TlsCipherSuiteId::TlsAes128GcmSha256, TlsRecordProtectionKind::Tls13, key_iv("0001"), iv12)
                        .error(),
                fiber::common::IoErr::Invalid);
        EXPECT_EQ(bad_len.init(TlsCipherSuiteId::EcdheRsaAes128GcmSha256, TlsRecordProtectionKind::Tls12, key16,
                               key_iv("0001020304"))
                          .error(),
                  fiber::common::IoErr::Invalid);
        EXPECT_FALSE(bad_len.initialized());

        TlsRecordCipher good;
        EXPECT_TRUE(good.init(TlsCipherSuiteId::TlsAes128GcmSha256, TlsRecordProtectionKind::Tls13, key16, iv12)
                            .has_value());
        EXPECT_TRUE(good.initialized());
        EXPECT_EQ(good.suite(), TlsCipherSuiteId::TlsAes128GcmSha256);
        EXPECT_EQ(good.kind(), TlsRecordProtectionKind::Tls13);
        EXPECT_EQ(good.sequence(), 0u);
    });
}

// ------------------------------------------------- §10.1/10.2: fixed vectors

class TlsRecordCipherKat : public ::testing::TestWithParam<SuiteVectors> {};

TEST_P(TlsRecordCipherKat, SealMatchesIndependentConstruction) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        const SuiteVectors &v = GetParam();
        TlsRecordCipher cipher;
        init_cipher(cipher, v);

        std::size_t seq = 0;
        for (const std::size_t plain_len: {std::size_t{0}, std::size_t{1}, std::size_t{500}}) {
            const auto plain = ramp(plain_len, 0x40 + static_cast<std::uint8_t>(seq));
            const TlsContentType type = seq % 2 == 0 ? TlsContentType::ApplicationData : TlsContentType::Handshake;

            const auto expected = v.kind == TlsRecordProtectionKind::Tls13
                                          ? expected_tls13_payload(v.suite, v.key, v.iv, seq, type, plain)
                                          : expected_tls12_payload(v.suite, v.key, v.iv, seq, type, plain);

            std::vector<std::uint8_t> dst(expected.size() + 8, 0xAA); // slop past the end
            const auto r = cipher.seal(type, plain, dst);
            ASSERT_EQ(r.status, TlsRecordCipher::Status::Ok);
            EXPECT_EQ(r.out_len, expected.size());
            EXPECT_EQ(std::vector<std::uint8_t>(dst.begin(), dst.begin() + r.out_len), expected);
            for (std::size_t i = r.out_len; i < dst.size(); ++i) {
                EXPECT_EQ(dst[i], 0xAA); // no overflow past the promised output
            }
            EXPECT_EQ(cipher.sequence(), ++seq);
        }
    });
}

TEST_P(TlsRecordCipherKat, OpenRestoresPlaintextAndType) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        const SuiteVectors &v = GetParam();
        TlsRecordCipher cipher;
        init_cipher(cipher, v);

        const auto plain = ramp(300, 0x77);
        const TlsContentType type = TlsContentType::Alert;
        const auto expected = v.kind == TlsRecordProtectionKind::Tls13
                                      ? expected_tls13_payload(v.suite, v.key, v.iv, 0, type, plain)
                                      : expected_tls12_payload(v.suite, v.key, v.iv, 0, type, plain);

        // In place (1.2 GCM: dst starts past the explicit nonce; 1.2 ChaCha and
        // 1.3 are in place with no prefix). 1.2 has no inner type: the received
        // outer type IS the record type, so it must match the type the record was
        // sealed with; 1.3 always opens with outer 23.
        const bool tls13 = v.kind == TlsRecordProtectionKind::Tls13;
        const TlsContentType outer = tls13 ? TlsContentType::ApplicationData : type;
        const std::size_t nonce_prefix = wire_nonce_len(v);
        std::vector<std::uint8_t> buf(expected);
        auto r = cipher.open(outer, 0x0303, static_cast<std::uint16_t>(expected.size()), buf,
                             std::span<std::uint8_t>(buf.data() + nonce_prefix, expected.size() - nonce_prefix));
        ASSERT_EQ(r.status, TlsRecordCipher::Status::Ok);
        EXPECT_EQ(r.inner_type, type); // 1.3 reports the inner type; 1.2 echoes the outer
        EXPECT_EQ(r.plain_len, plain.size());
        EXPECT_EQ(std::vector<std::uint8_t>(buf.data() + nonce_prefix, buf.data() + nonce_prefix + r.plain_len), plain);

        // Fresh cipher, separate dst.
        TlsRecordCipher second;
        init_cipher(second, v);
        std::vector<std::uint8_t> out(expected.size());
        r = second.open(outer, 0x0303, static_cast<std::uint16_t>(expected.size()), expected, out);
        ASSERT_EQ(r.status, TlsRecordCipher::Status::Ok);
        EXPECT_EQ(r.plain_len, plain.size());
        EXPECT_EQ(std::vector<std::uint8_t>(out.begin(), out.begin() + r.plain_len), plain);
        EXPECT_EQ(second.sequence(), 1u);
    });
}

INSTANTIATE_TEST_SUITE_P(Tls13, TlsRecordCipherKat, ::testing::ValuesIn(tls13_suites()));
INSTANTIATE_TEST_SUITE_P(Tls12, TlsRecordCipherKat, ::testing::ValuesIn(tls12_suites()));

// ------------------------------------------------------------ §10.3: sizes

TEST(TlsRecordCipherSizes, BoundariesPerKind) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        TlsRecordCipher c13;
        init_cipher(c13, tls13_suites()[0]);
        EXPECT_EQ(c13.seal_output_size(0), 17u);
        EXPECT_EQ(c13.seal_output_size(1), 18u);
        EXPECT_EQ(c13.seal_output_size(fiber::tls::kTlsMaxPlaintextSize), fiber::tls::kTlsMaxPlaintextSize + 17);
        // 1.3 open capacity includes the type + padding workspace; the plaintext
        // itself can be smaller (OpenResult::plain_len).
        EXPECT_EQ(c13.open_output_size(17), 1u);
        EXPECT_EQ(c13.open_output_size(17 + 32), 33u);
        EXPECT_EQ(c13.min_ciphertext_size(), 17u);
        EXPECT_EQ(c13.max_ciphertext_size(), fiber::tls::kTlsMaxPlaintextSize + 256u);

        TlsRecordCipher c12;
        init_cipher(c12, tls12_suites()[1]);
        EXPECT_EQ(c12.seal_output_size(0), 24u);
        EXPECT_EQ(c12.seal_output_size(fiber::tls::kTlsMaxPlaintextSize), fiber::tls::kTlsMaxPlaintextSize + 24);
        EXPECT_EQ(c12.open_output_size(24), 0u); // exact for 1.2
        EXPECT_EQ(c12.open_output_size(100), 76u);
        EXPECT_EQ(c12.min_ciphertext_size(), 24u);
        EXPECT_EQ(c12.max_ciphertext_size(), fiber::tls::kTlsMaxPlaintextSize + 2048u);
        EXPECT_EQ(c13.explicit_nonce_len(), 0u);
        EXPECT_EQ(c12.explicit_nonce_len(), 8u); // RFC 5288 GCM form

        // 1.2 ChaCha20 (RFC 7905 §2): no wire nonce — overhead is the tag alone.
        TlsRecordCipher cc12;
        init_cipher(cc12, tls12_suites()[2]);
        EXPECT_EQ(cc12.explicit_nonce_len(), 0u);
        EXPECT_EQ(cc12.seal_output_size(0), 16u);
        EXPECT_EQ(cc12.seal_output_size(fiber::tls::kTlsMaxPlaintextSize), fiber::tls::kTlsMaxPlaintextSize + 16);
        EXPECT_EQ(cc12.open_output_size(16), 0u);
        EXPECT_EQ(cc12.open_output_size(100), 84u);
        EXPECT_EQ(cc12.min_ciphertext_size(), 16u);
        EXPECT_EQ(cc12.max_ciphertext_size(), fiber::tls::kTlsMaxPlaintextSize + 2048u);
    });
}

// ------------------------------------------------------- §10.4: roundtrip

TEST(TlsRecordCipherRoundTrip, SequenceAdvanceAndCiphertextUniqueness) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        for (const SuiteVectors &v: all_suites()) {
            TlsRecordCipher seal_side;
            init_cipher(seal_side, v);
            TlsRecordCipher open_side;
            init_cipher(open_side, v);

            for (const std::size_t plain_len: {std::size_t{0}, std::size_t{1}, fiber::tls::kTlsMaxPlaintextSize}) {
                std::vector<std::uint8_t> prev;
                for (std::uint64_t step = 0; step < 3; ++step) {
                    const std::uint64_t seq = seal_side.sequence(); // the cipher persists across lengths
                    const auto plain = ramp(plain_len, static_cast<std::uint8_t>(0x10 * step));
                    std::vector<std::uint8_t> wire(seal_side.seal_output_size(plain_len));
                    const auto s = seal_side.seal(TlsContentType::ApplicationData, plain, wire);
                    ASSERT_EQ(s.status, TlsRecordCipher::Status::Ok);
                    ASSERT_EQ(seal_side.sequence(), seq + 1);

                    if (plain_len > 0 || seq > 0) {
                        EXPECT_NE(wire, prev); // nonce/seq variation shows up in the ciphertext
                    }
                    prev = wire;

                    if (wire_nonce_len(v) == 8) {
                        // 1.2 GCM: the explicit nonce is BE64(seq) on the wire
                        EXPECT_EQ(wire[0], 0u);
                        EXPECT_EQ(wire[7], static_cast<std::uint8_t>(seq));
                    }

                    std::vector<std::uint8_t> out(open_side.open_output_size(wire.size()));
                    const auto r = open_side.open(TlsContentType::ApplicationData, 0x0303,
                                                  static_cast<std::uint16_t>(wire.size()), wire, out);
                    ASSERT_EQ(r.status, TlsRecordCipher::Status::Ok);
                    ASSERT_EQ(r.plain_len, plain_len);
                    EXPECT_EQ(std::vector<std::uint8_t>(out.begin(), out.begin() + r.plain_len), plain);
                    EXPECT_EQ(open_side.sequence(), seq + 1);
                }
            }
        }
    });
}

TEST(TlsRecordCipherRoundTrip, AuthFailDoesNotAdvanceSequence) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        const SuiteVectors &v = tls13_suites()[0];
        TlsRecordCipher cipher;
        init_cipher(cipher, v);

        auto wire = expected_tls13_payload(v.suite, v.key, v.iv, 0, TlsContentType::ApplicationData, ramp(10, 1));
        wire[3] ^= 0x40; // corrupt
        std::vector<std::uint8_t> out(wire.size());
        const auto r = cipher.open(TlsContentType::ApplicationData, 0x0303, static_cast<std::uint16_t>(wire.size()),
                                   wire, out);
        EXPECT_EQ(r.status, TlsRecordCipher::Status::AuthFail);
        EXPECT_EQ(cipher.sequence(), 0u);
    });
}

// TLS 1.3 trailing-zero semantics: the encrypted inner type sits AFTER the
// plaintext, so plaintext trailing zeros are guarded — they survive intact.
// Only explicit zero padding (which this sender never adds) would be stripped.
TEST(TlsRecordCipherRoundTrip, Tls13PlaintextTrailingZerosSurvive) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        const SuiteVectors &v = tls13_suites()[0];
        TlsRecordCipher seal_side;
        init_cipher(seal_side, v);
        TlsRecordCipher open_side;
        init_cipher(open_side, v);

        const auto plain = std::vector<std::uint8_t>{0x61, 0x62, 0x00, 0x00};
        std::vector<std::uint8_t> wire(seal_side.seal_output_size(plain.size()));
        ASSERT_EQ(seal_side.seal(TlsContentType::ApplicationData, plain, wire).status, TlsRecordCipher::Status::Ok);

        std::vector<std::uint8_t> out(open_side.open_output_size(wire.size()));
        const auto r = open_side.open(TlsContentType::ApplicationData, 0x0303, static_cast<std::uint16_t>(wire.size()),
                                      wire, out);
        ASSERT_EQ(r.status, TlsRecordCipher::Status::Ok);
        EXPECT_EQ(r.inner_type, TlsContentType::ApplicationData);
        EXPECT_EQ(r.plain_len, plain.size()); // zeros intact: the type byte guards them
        EXPECT_EQ(std::vector<std::uint8_t>(out.begin(), out.begin() + r.plain_len), plain);
    });
}

// ------------------------------------------------------- §10.5: aliasing

TEST(TlsRecordCipherAliasing, InPlaceSealTls13WritesTypeIntoTailroom) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        const SuiteVectors &v = tls13_suites()[0];
        TlsRecordCipher cipher;
        init_cipher(cipher, v);
        const auto plain = ramp(16, 0x50);

        // Simulate the engine's fast path: a node holding the plaintext with
        // tailroom for the type byte + tag, guard bytes around it.
        std::vector<std::uint8_t> storage(8 + plain.size() + 17 + 8, 0xEE);
        std::memcpy(storage.data() + 8, plain.data(), plain.size());
        // node view = [8, 8+plain); dst spans [8, 8+plain+17)
        const std::span<std::uint8_t> dst(storage.data() + 8, plain.size() + 17);
        const std::span<const std::uint8_t> plaintext(storage.data() + 8, plain.size());

        const auto s = cipher.seal(TlsContentType::Handshake, plaintext, dst);
        ASSERT_EQ(s.status, TlsRecordCipher::Status::Ok);
        EXPECT_EQ(s.out_len, plain.size() + 17);

        // guards untouched
        for (std::size_t i = 0; i < 8; ++i) {
            EXPECT_EQ(storage[i], 0xEE);
            EXPECT_EQ(storage[8 + plain.size() + 17 + i], 0xEE);
        }
        // identical bytes to a disjoint-dst seal
        TlsRecordCipher again;
        init_cipher(again, v);
        std::vector<std::uint8_t> separate(plain.size() + 17);
        ASSERT_EQ(again.seal(TlsContentType::Handshake, plain, separate).status, TlsRecordCipher::Status::Ok);
        EXPECT_EQ(std::vector<std::uint8_t>(dst.begin(), dst.end()), separate);
    });
}

TEST(TlsRecordCipherAliasing, InPlaceSealTls12UsesHeadroomForNonce) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        const SuiteVectors &v = tls12_suites()[0];
        TlsRecordCipher cipher;
        init_cipher(cipher, v);
        const auto plain = ramp(16, 0x60);

        // Layout: 8 guard | 8 nonce headroom | plaintext | 16 tag | 8 guard.
        std::vector<std::uint8_t> storage(8 + 8 + plain.size() + 16 + 8, 0xEE);
        std::memcpy(storage.data() + 16, plain.data(), plain.size()); // plaintext at +16
        const std::span<std::uint8_t> dst(storage.data() + 8, 8 + plain.size() + 16);
        const std::span<const std::uint8_t> plaintext(storage.data() + 16, plain.size());

        const auto s = cipher.seal(TlsContentType::ApplicationData, plaintext, dst);
        ASSERT_EQ(s.status, TlsRecordCipher::Status::Ok);
        EXPECT_EQ(s.out_len, plain.size() + 24);
        EXPECT_EQ(dst[0], 0); // explicit nonce = BE64(0)
        EXPECT_EQ(dst[7], 0);

        for (std::size_t i = 0; i < 8; ++i) {
            EXPECT_EQ(storage[i], 0xEE); // head guard (before dst)
        }
        EXPECT_EQ(std::vector<std::uint8_t>(storage.end() - 8, storage.end()),
                  std::vector<std::uint8_t>(8, 0xEE)); // tail guard (after tag)

        // identical to a disjoint-dst seal
        TlsRecordCipher again;
        init_cipher(again, v);
        std::vector<std::uint8_t> separate(plain.size() + 24);
        ASSERT_EQ(again.seal(TlsContentType::ApplicationData, plain, separate).status, TlsRecordCipher::Status::Ok);
        EXPECT_EQ(std::vector<std::uint8_t>(dst.begin(), dst.end()), separate);
    });
}

TEST(TlsRecordCipherAliasing, InPlaceOpenLeavesGuardsAndSeparateDstAgrees) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        for (const SuiteVectors &v: tls13_suites()) {
            TlsRecordCipher seal_once;
            init_cipher(seal_once, v);
            TlsRecordCipher in_place;
            init_cipher(in_place, v);
            TlsRecordCipher separate;
            init_cipher(separate, v);

            const auto plain = ramp(24, 0x80);
            std::vector<std::uint8_t> wire(seal_once.seal_output_size(plain.size()));
            ASSERT_EQ(seal_once.seal(TlsContentType::ApplicationData, plain, wire).status, TlsRecordCipher::Status::Ok);

            std::vector<std::uint8_t> buf(8 + wire.size() + 8, 0xEE);
            std::memcpy(buf.data() + 8, wire.data(), wire.size());

            const auto a =
                    in_place.open(TlsContentType::ApplicationData, 0x0303, static_cast<std::uint16_t>(wire.size()),
                                  std::span<const std::uint8_t>(buf.data() + 8, wire.size()),
                                  std::span<std::uint8_t>(buf.data() + 8, wire.size()));
            ASSERT_EQ(a.status, TlsRecordCipher::Status::Ok);
            EXPECT_EQ(std::vector<std::uint8_t>(buf.begin(), buf.begin() + 8), std::vector<std::uint8_t>(8, 0xEE));
            EXPECT_EQ(std::vector<std::uint8_t>(buf.end() - 8, buf.end()), std::vector<std::uint8_t>(8, 0xEE));

            std::vector<std::uint8_t> out(separate.open_output_size(wire.size()));
            const auto b = separate.open(TlsContentType::ApplicationData, 0x0303,
                                         static_cast<std::uint16_t>(wire.size()), wire, out);
            ASSERT_EQ(b.status, TlsRecordCipher::Status::Ok);
            EXPECT_EQ(std::vector<std::uint8_t>(buf.data() + 8, buf.data() + 8 + a.plain_len),
                      std::vector<std::uint8_t>(out.begin(), out.begin() + b.plain_len));
        }
    });
}

TEST(TlsRecordCipherAliasing, DisjointSealDoesNotTouchSource) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        const SuiteVectors &v = tls13_suites()[0];
        TlsRecordCipher cipher;
        init_cipher(cipher, v);
        const auto plain = ramp(20, 0x99);
        std::vector<std::uint8_t> dst(cipher.seal_output_size(plain.size()));
        ASSERT_EQ(cipher.seal(TlsContentType::ApplicationData, plain, dst).status, TlsRecordCipher::Status::Ok);
        EXPECT_EQ(plain, ramp(20, 0x99)); // source span untouched
    });
}

// --------------------------------------------------- §10.6: failure paths

class Tls13Failure : public ::testing::Test {
protected:
    const SuiteVectors &v = tls13_suites()[0];
    TlsRecordCipher cipher;

    void SetUp() override { init_cipher(cipher, v); }

    std::vector<std::uint8_t> wire_of(TlsContentType type, const std::vector<std::uint8_t> &plain) {
        std::vector<std::uint8_t> wire(cipher.seal_output_size(plain.size()));
        EXPECT_EQ(cipher.seal(type, plain, wire).status, TlsRecordCipher::Status::Ok);
        return wire;
    }
};

TEST_F(Tls13Failure, BitFlipEverywhereIsAuthFail) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        auto wire = wire_of(TlsContentType::ApplicationData, ramp(32, 7));
        for (const std::size_t pos: {std::size_t{0}, std::size_t{10}, wire.size() - 1}) {
            TlsRecordCipher opener;
            init_cipher(opener, v);
            std::vector<std::uint8_t> tampered = wire;
            tampered[pos] ^= 0x01;
            std::vector<std::uint8_t> out(opener.open_output_size(tampered.size()));
            const auto r = opener.open(TlsContentType::ApplicationData, 0x0303,
                                       static_cast<std::uint16_t>(tampered.size()), tampered, out);
            EXPECT_EQ(r.status, TlsRecordCipher::Status::AuthFail);
        }
    });
}

TEST_F(Tls13Failure, LengthBoundsAreMalformed) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        std::vector<std::uint8_t> small(64);
        // Below the 1.3 floor (17).
        EXPECT_EQ(cipher.open(TlsContentType::ApplicationData, 0x0303, 16,
                              std::span<const std::uint8_t>(small.data(), 16), small)
                          .status,
                  TlsRecordCipher::Status::Malformed);
        // Above the exact 1.3 cap (2^14 + 256): the length alone is malformed,
        // regardless of what the buffers hold.
        const std::uint16_t over = static_cast<std::uint16_t>(fiber::tls::kTlsMaxPlaintextSize + 257);
        EXPECT_EQ(cipher.open(TlsContentType::ApplicationData, 0x0303, over,
                              std::span<const std::uint8_t>(small.data(), small.size()), small)
                          .status,
                  TlsRecordCipher::Status::Malformed);

        // The exact cap is not rejected pre-decryption (decryption decides).
        const std::size_t cap = fiber::tls::kTlsMaxPlaintextSize + 256;
        std::vector<std::uint8_t> big(cap);
        TlsRecordCipher opener;
        init_cipher(opener, v);
        EXPECT_EQ(opener.open(TlsContentType::ApplicationData, 0x0303, static_cast<std::uint16_t>(cap),
                              std::span<const std::uint8_t>(big.data(), cap), big)
                          .status,
                  TlsRecordCipher::Status::AuthFail);
    });
}

TEST_F(Tls13Failure, OuterTypeMustBeApplicationData) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        auto wire = wire_of(TlsContentType::ApplicationData, ramp(8, 2));
        std::vector<std::uint8_t> out(wire.size());
        EXPECT_EQ(cipher.open(TlsContentType::Handshake, 0x0303, static_cast<std::uint16_t>(wire.size()), wire, out)
                          .status,
                  TlsRecordCipher::Status::Malformed);
    });
}

TEST_F(Tls13Failure, IllegalInnerTypeIsMalformed) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        // Hand-build a record whose decrypted content type is 0x01 (illegal).
        const auto plain = ramp(8, 3);
        std::array<std::uint8_t, 12> nonce{};
        std::memcpy(nonce.data(), v.iv.data(), 12); // seq 0
        const std::size_t out_len = plain.size() + 17;
        const std::array<std::uint8_t, 5> aad{23, 0x03, 0x03, static_cast<std::uint8_t>(out_len >> 8),
                                              static_cast<std::uint8_t>(out_len)};
        std::vector<std::uint8_t> input = plain;
        input.push_back(0x01); // illegal inner type
        EVP_AEAD_CTX ctx;
        EVP_AEAD_CTX_zero(&ctx);
        ASSERT_EQ(EVP_AEAD_CTX_init(&ctx, aead_for(v.suite), v.key.data(), v.key.size(), EVP_AEAD_DEFAULT_TAG_LENGTH,
                                    nullptr),
                  1);
        std::vector<std::uint8_t> wire(out_len);
        size_t written = 0;
        ASSERT_EQ(EVP_AEAD_CTX_seal(&ctx, wire.data(), &written, wire.size(), nonce.data(), 12, input.data(),
                                    input.size(), aad.data(), aad.size()),
                  1);
        EVP_AEAD_CTX_cleanup(&ctx);

        std::vector<std::uint8_t> out(wire.size());
        const auto r = cipher.open(TlsContentType::ApplicationData, 0x0303, static_cast<std::uint16_t>(wire.size()),
                                   wire, out);
        EXPECT_EQ(r.status, TlsRecordCipher::Status::Malformed);
    });
}

TEST_F(Tls13Failure, AllZeroInnerIsMalformed) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        // A plaintext of pure zeros decrypts fine but carries no content type:
        // the AEAD input is exactly the zeros — no trailing type byte at all.
        const std::vector<std::uint8_t> zeros(4, 0);
        std::array<std::uint8_t, 12> nonce{};
        std::memcpy(nonce.data(), v.iv.data(), 12); // seq 0
        const std::size_t out_len = zeros.size() + 16;
        const std::array<std::uint8_t, 5> aad{23, 0x03, 0x03, static_cast<std::uint8_t>(out_len >> 8),
                                              static_cast<std::uint8_t>(out_len)};
        EVP_AEAD_CTX ctx;
        EVP_AEAD_CTX_zero(&ctx);
        ASSERT_EQ(EVP_AEAD_CTX_init(&ctx, aead_for(v.suite), v.key.data(), v.key.size(), EVP_AEAD_DEFAULT_TAG_LENGTH,
                                    nullptr),
                  1);
        std::vector<std::uint8_t> wire(out_len);
        size_t written = 0;
        ASSERT_EQ(EVP_AEAD_CTX_seal(&ctx, wire.data(), &written, wire.size(), nonce.data(), 12, zeros.data(),
                                    zeros.size(), aad.data(), aad.size()),
                  1);
        EVP_AEAD_CTX_cleanup(&ctx);

        std::vector<std::uint8_t> out(wire.size());
        EXPECT_EQ(
                cipher.open(TlsContentType::ApplicationData, 0x0303, static_cast<std::uint16_t>(wire.size()), wire, out)
                        .status,
                TlsRecordCipher::Status::Malformed);
    });
}

TEST_F(Tls13Failure, AadIsBoundToHeaderFields) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        auto wire = wire_of(TlsContentType::ApplicationData, ramp(16, 4));
        std::vector<std::uint8_t> out(wire.size());
        // Wrong legacy_version in the received header -> AAD mismatch.
        EXPECT_EQ(
                cipher.open(TlsContentType::ApplicationData, 0x0301, static_cast<std::uint16_t>(wire.size()), wire, out)
                        .status,
                TlsRecordCipher::Status::AuthFail);
        // Wrong length in the received header (within bounds) -> AAD mismatch.
        TlsRecordCipher opener;
        init_cipher(opener, v);
        EXPECT_EQ(opener.open(TlsContentType::ApplicationData, 0x0303, static_cast<std::uint16_t>(wire.size() - 1),
                              std::span<const std::uint8_t>(wire.data(), wire.size() - 1), out)
                          .status,
                  TlsRecordCipher::Status::AuthFail);
    });
}

// An authenticated plaintext over the protocol limit is Overflow (the caller
// sends record_overflow): 2^14 in 1.2, 2^14 + 1 for the whole 1.3
// TLSInnerPlaintext — the ciphertext bounds alone admit more.
TEST(TlsRecordCipherOverflow, PlaintextOverLimitIsOverflowInBothOpenForms) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        for (const SuiteVectors &v: all_suites()) {
            const bool tls13 = v.kind == TlsRecordProtectionKind::Tls13;
            const TlsContentType type = TlsContentType::ApplicationData;
            for (const std::size_t plain_len:
                 {fiber::tls::kTlsMaxPlaintextSize, fiber::tls::kTlsMaxPlaintextSize + 1}) {
                const bool over = plain_len > fiber::tls::kTlsMaxPlaintextSize;
                const auto plain = ramp(plain_len, 0x21);
                TlsRecordCipher sealer;
                init_cipher(sealer, v);
                std::vector<std::uint8_t> wire(sealer.seal_output_size(plain.size()));
                ASSERT_EQ(sealer.seal(type, plain, wire).status, TlsRecordCipher::Status::Ok);
                const std::uint16_t length = static_cast<std::uint16_t>(wire.size());
                const auto expected = over ? TlsRecordCipher::Status::Overflow : TlsRecordCipher::Status::Ok;

                TlsRecordCipher opener;
                init_cipher(opener, v);
                std::vector<std::uint8_t> out(opener.open_output_size(length));
                EXPECT_EQ(opener.open(type, 0x0303, length, wire, out).status, expected)
                        << (tls13 ? "1.3" : "1.2") << " open, plain_len=" << plain_len;

                const std::size_t off = wire_nonce_len(v);
                const std::size_t body_len = wire.size() - off - 16;
                TlsRecordCipher scatter;
                init_cipher(scatter, v);
                std::vector<std::uint8_t> dst(scatter.open_output_size(length));
                EXPECT_EQ(scatter.open_scatter(type, 0x0303, length, {wire.data(), off}, {wire.data() + off, body_len},
                                               {wire.data() + off + body_len, 16}, dst)
                                  .status,
                          expected)
                        << (tls13 ? "1.3" : "1.2") << " open_scatter, plain_len=" << plain_len;
            }
        }
    });
}

TEST_F(Tls13Failure, PaddingPastInnerPlaintextLimitIsOverflow) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        // 8 content bytes + type + zero padding to 2^14 + 2 inner bytes: the
        // limit counts the padding (RFC 8446 §5.4), not just the content.
        std::vector<std::uint8_t> input = ramp(8, 3);
        input.push_back(23); // application_data
        input.resize(fiber::tls::kTlsMaxPlaintextSize + 2, 0);
        std::array<std::uint8_t, 12> nonce{};
        std::memcpy(nonce.data(), v.iv.data(), 12); // seq 0
        const std::size_t out_len = input.size() + 16;
        const std::array<std::uint8_t, 5> aad{23, 0x03, 0x03, static_cast<std::uint8_t>(out_len >> 8),
                                              static_cast<std::uint8_t>(out_len)};
        EVP_AEAD_CTX ctx;
        EVP_AEAD_CTX_zero(&ctx);
        ASSERT_EQ(EVP_AEAD_CTX_init(&ctx, aead_for(v.suite), v.key.data(), v.key.size(), EVP_AEAD_DEFAULT_TAG_LENGTH,
                                    nullptr),
                  1);
        std::vector<std::uint8_t> wire(out_len);
        size_t written = 0;
        ASSERT_EQ(EVP_AEAD_CTX_seal(&ctx, wire.data(), &written, wire.size(), nonce.data(), 12, input.data(),
                                    input.size(), aad.data(), aad.size()),
                  1);
        EVP_AEAD_CTX_cleanup(&ctx);

        std::vector<std::uint8_t> out(wire.size());
        EXPECT_EQ(
                cipher.open(TlsContentType::ApplicationData, 0x0303, static_cast<std::uint16_t>(wire.size()), wire, out)
                        .status,
                TlsRecordCipher::Status::Overflow);
    });
}

TEST(Tls12Failure, LengthBoundsAndReplayAreRejected) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        const SuiteVectors &v = tls12_suites()[0];
        TlsRecordCipher cipher;
        init_cipher(cipher, v);
        std::vector<std::uint8_t> out(64);

        EXPECT_EQ(cipher.open(TlsContentType::ApplicationData, 0x0303, 23,
                              std::span<const std::uint8_t>(out.data(), 23), out)
                          .status,
                  TlsRecordCipher::Status::Malformed);
        EXPECT_EQ(cipher.open(TlsContentType::ApplicationData, 0x0303,
                              static_cast<std::uint16_t>(fiber::tls::kTlsMaxPlaintextSize + 2049),
                              std::span<const std::uint8_t>(out.data(), out.size()), out)
                          .status,
                  TlsRecordCipher::Status::Malformed);

        // Replay: an opener instance sees the same record twice — the second
        // open binds a different seq into the AAD and must fail.
        const auto plain = ramp(12, 5);
        std::vector<std::uint8_t> wire(cipher.seal_output_size(plain.size()));
        ASSERT_EQ(cipher.seal(TlsContentType::ApplicationData, plain, wire).status, TlsRecordCipher::Status::Ok);
        TlsRecordCipher opener;
        init_cipher(opener, v);
        std::vector<std::uint8_t> dst(plain.size());
        const auto first = opener.open(TlsContentType::ApplicationData, 0x0303, static_cast<std::uint16_t>(wire.size()),
                                       wire, dst);
        ASSERT_EQ(first.status, TlsRecordCipher::Status::Ok);
        const auto second = opener.open(TlsContentType::ApplicationData, 0x0303,
                                        static_cast<std::uint16_t>(wire.size()), wire, dst);
        EXPECT_EQ(second.status, TlsRecordCipher::Status::AuthFail); // seq replay
    });
}

// ------------------------------------- §10.7: reader -> cipher -> writer

TEST(TlsRecordCipherEndToEnd, Tls13InPlaceChainOrchestration) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        using fiber::mem::IoBuf;
        using fiber::mem::IoBufChain;
        using fiber::mem::IoBufNodePool;
        using fiber::tls::TlsRecordReader;
        using fiber::tls::TlsRecordWriter;

        const SuiteVectors &v = tls13_suites()[0];
        TlsRecordCipher seal_side;
        init_cipher(seal_side, v);
        TlsRecordCipher open_side;
        init_cipher(open_side, v);


        TlsRecordWriter writer;

        // Engine fast path: one node, plaintext committed, tailroom >= 17.
        const auto plain = ramp(100, 0x30);
        IoBuf node = IoBuf::allocate(plain.size() + 64);
        ASSERT_TRUE(node);
        std::memcpy(node.writable_data(), plain.data(), plain.size());
        node.commit(plain.size());

        IoBufChain payload;
        ASSERT_TRUE(payload.append(std::move(node)));

        IoBuf *back = payload.back();
        ASSERT_NE(back, nullptr);
        ASSERT_TRUE(back->unique());
        ASSERT_GE(back->tailroom(), 17u);
        const auto s = seal_side.seal(
                TlsContentType::ApplicationData, std::span<const std::uint8_t>(back->readable_data(), back->readable()),
                std::span<std::uint8_t>(back->readable_data(), back->readable() + back->tailroom()));
        ASSERT_EQ(s.status, TlsRecordCipher::Status::Ok);
        payload.commit_tailroom(s.out_len - payload.readable_bytes());

        IoBufChain framed;
        ASSERT_TRUE(writer.write(TlsContentType::ApplicationData, std::move(payload), framed).has_value());

        // Receive side: reader -> in-place open -> trim_end.
        TlsRecordReader reader;
        ASSERT_TRUE(reader.feed(std::move(framed)));
        auto r = reader.next();
        ASSERT_EQ(r.status, TlsRecordReader::Result::Status::Ok);
        ASSERT_EQ(r.record.type, TlsContentType::ApplicationData);
        fiber::mem::IoBuf *front = r.record.payload.first_readable();
        ASSERT_NE(front, nullptr);
        ASSERT_EQ(front->readable(), r.record.length);

        const auto o = open_side.open(r.record.type, r.record.legacy_version, r.record.length,
                                      std::span<const std::uint8_t>(front->readable_data(), front->readable()),
                                      std::span<std::uint8_t>(front->readable_data(), front->readable()));
        ASSERT_EQ(o.status, TlsRecordCipher::Status::Ok);
        ASSERT_EQ(o.inner_type, TlsContentType::ApplicationData);
        r.record.payload.trim_end(r.record.length - o.plain_len);

        EXPECT_EQ(r.record.payload.readable_bytes(), plain.size());
        std::vector<std::uint8_t> received;
        struct iovec spans[4];
        int count = r.record.payload.fill_write_iov(spans, 4);
        for (int i = 0; i < count; ++i) {
            const auto *begin = static_cast<const std::uint8_t *>(spans[i].iov_base);
            received.insert(received.end(), begin, begin + spans[i].iov_len);
        }
        EXPECT_EQ(received, plain);
    });
}

TEST(TlsRecordCipherEndToEnd, Tls12FreshNodeOrchestration) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        using fiber::mem::IoBuf;
        using fiber::mem::IoBufChain;
        using fiber::mem::IoBufNodePool;
        using fiber::tls::TlsRecordReader;
        using fiber::tls::TlsRecordWriter;

        const SuiteVectors &v = tls12_suites()[0];
        TlsRecordCipher seal_side;
        init_cipher(seal_side, v);
        TlsRecordCipher open_side;
        init_cipher(open_side, v);


        TlsRecordWriter writer;

        // 1.2 disjoint-dst path: fresh node of plain+24, the EVP moves the bytes.
        const auto plain = ramp(80, 0x50);
        IoBuf node = IoBuf::allocate(seal_side.seal_output_size(plain.size()));
        ASSERT_TRUE(node);
        const auto s = seal_side.seal(TlsContentType::ApplicationData, plain,
                                      std::span<std::uint8_t>(node.writable_data(), node.writable()));
        ASSERT_EQ(s.status, TlsRecordCipher::Status::Ok);
        node.commit(s.out_len);

        IoBufChain payload;
        ASSERT_TRUE(payload.append(std::move(node)));
        IoBufChain framed;
        ASSERT_TRUE(writer.write(TlsContentType::ApplicationData, std::move(payload), framed).has_value());

        TlsRecordReader reader;
        ASSERT_TRUE(reader.feed(std::move(framed)));
        auto r = reader.next();
        ASSERT_EQ(r.status, TlsRecordReader::Result::Status::Ok);
        fiber::mem::IoBuf *front = r.record.payload.first_readable();
        ASSERT_NE(front, nullptr);
        ASSERT_EQ(front->readable(), r.record.length);

        // Open in place: ciphertext = the full payload (with nonce prefix for
        // GCM), the plaintext lands past the nonce; the chain view is adjusted
        // afterwards.
        const std::uint8_t *wire = front->readable_data();
        const std::size_t expl = open_side.explicit_nonce_len();
        const std::uint16_t body_len = static_cast<std::uint16_t>(r.record.length - 16 - expl);
        const auto o = open_side.open(TlsContentType::ApplicationData, 0x0303, r.record.length,
                                      std::span<const std::uint8_t>(wire, r.record.length),
                                      std::span<std::uint8_t>(const_cast<std::uint8_t *>(wire) + expl, body_len));
        ASSERT_EQ(o.status, TlsRecordCipher::Status::Ok);
        EXPECT_EQ(o.inner_type, TlsContentType::ApplicationData);
        r.record.payload.consume(expl); // drop the explicit nonce (GCM only; 0 for ChaCha)
        r.record.payload.trim_end(r.record.length - expl - o.plain_len); // then the tag

        EXPECT_EQ(r.record.payload.readable_bytes(), plain.size());
        std::vector<std::uint8_t> received;
        struct iovec spans[4];
        int count = r.record.payload.fill_write_iov(spans, 4);
        for (int i = 0; i < count; ++i) {
            const auto *begin = static_cast<const std::uint8_t *>(spans[i].iov_base);
            received.insert(received.end(), begin, begin + spans[i].iov_len);
        }
        EXPECT_EQ(received, plain);
    });
}

// ------------------------------------- scatter forms (chain-friendly)

const std::vector<SuiteVectors> &all_suites() {
    static const std::vector<SuiteVectors> suites = [] {
        std::vector<SuiteVectors> out = tls13_suites();
        const std::vector<SuiteVectors> &t12 = tls12_suites();
        out.insert(out.end(), t12.begin(), t12.end());
        return out;
    }();
    return suites;
}

TEST(TlsRecordCipherScatter, SealScatterMatchesSeal) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        for (const SuiteVectors &v: all_suites()) {
            const auto plain = ramp(77, 0x60);
            const bool tls13 = v.kind == TlsRecordProtectionKind::Tls13;
            const TlsContentType type = tls13 ? TlsContentType::ApplicationData : TlsContentType::Handshake;
            const std::size_t off = wire_nonce_len(v); // 8 only for 1.2 GCM

            TlsRecordCipher reference;
            init_cipher(reference, v);
            std::vector<std::uint8_t> wire(reference.seal_output_size(plain.size()));
            ASSERT_EQ(reference.seal(type, plain, wire).status, TlsRecordCipher::Status::Ok);

            // Disjoint outputs: three separate buffers (no prefix for 1.3 and
            // 1.2 ChaCha — neither puts nonce bytes on the wire).
            TlsRecordCipher scatter;
            init_cipher(scatter, v);
            std::vector<std::uint8_t> ct(plain.size());
            std::vector<std::uint8_t> prefix(8);
            std::vector<std::uint8_t> tag(17);
            const std::span<std::uint8_t> prefix_out =
                    off > 0 ? std::span<std::uint8_t>(prefix) : std::span<std::uint8_t>{};
            const auto r = scatter.seal_scatter(type, plain, ct, prefix_out, tag);
            ASSERT_EQ(r.status, TlsRecordCipher::Status::Ok);
            ASSERT_EQ(r.out_len, wire.size());
            EXPECT_EQ(ct, std::vector<std::uint8_t>(wire.begin() + off, wire.begin() + off + plain.size()));
            EXPECT_EQ(std::vector<std::uint8_t>(prefix.begin(), prefix.begin() + off),
                      std::vector<std::uint8_t>(wire.begin(), wire.begin() + off)); // 1.2-GCM nonce prefix
            EXPECT_EQ(std::vector<std::uint8_t>(tag.begin(), tag.begin() + r.out_len - off - plain.size()),
                      std::vector<std::uint8_t>(wire.begin() + off + plain.size(), wire.end()));

            // In place: dst_ct over a staged copy of the plaintext.
            TlsRecordCipher in_place;
            init_cipher(in_place, v);
            std::vector<std::uint8_t> staged(plain.size() + 1);
            std::memcpy(staged.data(), plain.data(), plain.size());
            staged[plain.size()] = 0xAA; // guard: never written (no tailroom touch)
            const auto r2 =
                    in_place.seal_scatter(type, std::span<const std::uint8_t>(staged.data(), plain.size()),
                                          std::span<std::uint8_t>(staged.data(), plain.size()), prefix_out, tag);
            ASSERT_EQ(r2.status, TlsRecordCipher::Status::Ok);
            EXPECT_EQ(std::vector<std::uint8_t>(staged.begin(), staged.begin() + plain.size()),
                      std::vector<std::uint8_t>(wire.begin() + off, wire.begin() + off + plain.size()));
            EXPECT_EQ(staged.back(), 0xAA);
        }
    });
}

TEST(TlsRecordCipherScatter, OpenScatterRestoresPlaintext) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        for (const SuiteVectors &v: all_suites()) {
            const auto plain = ramp(53, 0x70);
            const bool tls13 = v.kind == TlsRecordProtectionKind::Tls13;
            const TlsContentType type = tls13 ? TlsContentType::ApplicationData : TlsContentType::Handshake;

            TlsRecordCipher sealer;
            init_cipher(sealer, v);
            std::vector<std::uint8_t> wire(sealer.seal_output_size(plain.size()));
            ASSERT_EQ(sealer.seal(type, plain, wire).status, TlsRecordCipher::Status::Ok);
            const std::uint16_t length = static_cast<std::uint16_t>(wire.size());
            const std::size_t off = wire_nonce_len(v); // 8 only for 1.2 GCM
            const std::size_t body_len = wire.size() - off - 16;
            const std::span<const std::uint8_t> nonce{wire.data(), off};
            const std::span<const std::uint8_t> body{wire.data() + off, body_len};
            const std::span<const std::uint8_t> tag{wire.data() + off + body_len, 16};

            // Disjoint dst: the plaintext lands at dst.data() for BOTH kinds (no
            // 1.2 +8 offset — the nonce is not part of dst).
            TlsRecordCipher opener;
            init_cipher(opener, v);
            std::vector<std::uint8_t> dst(opener.open_output_size(length)); // 1.3: -16, includes the type workspace
            const auto r = opener.open_scatter(type, 0x0303, length, nonce, body, tag, dst);
            ASSERT_EQ(r.status, TlsRecordCipher::Status::Ok);
            EXPECT_EQ(r.inner_type, type);
            EXPECT_EQ(r.plain_len, plain.size());
            // 1.3 dst carries one extra type-workspace byte past the plaintext.
            EXPECT_EQ(std::vector<std::uint8_t>(dst.begin(), dst.begin() + plain.size()), plain);

            // In place: dst over a staged copy of the body; the tag span stays in
            // the wire (a different buffer — the cross-node shape).
            TlsRecordCipher in_place;
            init_cipher(in_place, v);
            std::vector<std::uint8_t> staged(wire.begin() + off, wire.end());
            const auto r2 = in_place.open_scatter(type, 0x0303, length, nonce,
                                                  std::span<const std::uint8_t>(staged.data(), body_len),
                                                  std::span<const std::uint8_t>(staged.data() + body_len, 16),
                                                  std::span<std::uint8_t>(staged.data(), body_len));
            ASSERT_EQ(r2.status, TlsRecordCipher::Status::Ok);
            EXPECT_EQ(r2.plain_len, plain.size());
            EXPECT_EQ(std::vector<std::uint8_t>(staged.begin(), staged.begin() + plain.size()), plain);
        }
    });
}

TEST(TlsRecordCipherScatter, EmptyPlaintextCrossFormRoundTrip) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        for (const SuiteVectors &v: all_suites()) {
            const bool tls13 = v.kind == TlsRecordProtectionKind::Tls13;
            const TlsContentType type = TlsContentType::Alert;

            // Scatter seal of an empty plaintext, regular open restores.
            const std::size_t off = wire_nonce_len(v); // 8 only for 1.2 GCM
            TlsRecordCipher scatter;
            init_cipher(scatter, v);
            std::vector<std::uint8_t> prefix(8);
            std::vector<std::uint8_t> tag(17);
            const std::span<std::uint8_t> prefix_out =
                    off > 0 ? std::span<std::uint8_t>(prefix) : std::span<std::uint8_t>{};
            std::vector<std::uint8_t> wire(scatter.seal_output_size(0));
            const auto r = scatter.seal_scatter(type, {}, std::span<std::uint8_t>(wire.data(), 0), prefix_out, tag);
            ASSERT_EQ(r.status, TlsRecordCipher::Status::Ok);
            const std::size_t tag_len = tls13 ? 17 : 16;
            std::memcpy(wire.data() + off, tag.data(), tag_len);
            if (off > 0) {
                std::memcpy(wire.data(), prefix.data(), off);
            }

            TlsRecordCipher opener;
            init_cipher(opener, v);
            const std::size_t open_off = off; // 1.2-GCM in-place dst sits past the nonce
            const auto o = opener.open(tls13 ? TlsContentType::ApplicationData : type, 0x0303,
                                       static_cast<std::uint16_t>(wire.size()), wire,
                                       std::span<std::uint8_t>(wire.data() + open_off, wire.size() - open_off));
            ASSERT_EQ(o.status, TlsRecordCipher::Status::Ok);
            EXPECT_EQ(o.inner_type, type);
            EXPECT_EQ(o.plain_len, 0u);

            // And the mirror: regular seal, scatter open. Fresh opener — the one
            // above already consumed sequence 0.
            TlsRecordCipher sealer;
            init_cipher(sealer, v);
            TlsRecordCipher opener2;
            init_cipher(opener2, v);
            std::vector<std::uint8_t> wire2(sealer.seal_output_size(0));
            ASSERT_EQ(sealer.seal(type, {}, wire2).status, TlsRecordCipher::Status::Ok);
            const std::size_t body_len = wire2.size() - off - 16;
            const auto o2 = opener2.open_scatter(tls13 ? TlsContentType::ApplicationData : type, 0x0303,
                                                 static_cast<std::uint16_t>(wire2.size()),
                                                 std::span<const std::uint8_t>(wire2.data(), off),
                                                 std::span<const std::uint8_t>(wire2.data() + off, body_len),
                                                 std::span<const std::uint8_t>(wire2.data() + off + body_len, 16),
                                                 std::span<std::uint8_t>(wire2.data(), wire2.size()));
            ASSERT_EQ(o2.status, TlsRecordCipher::Status::Ok);
            EXPECT_EQ(o2.plain_len, 0u);
            EXPECT_EQ(o2.inner_type, type);
        }
    });
}

TEST(TlsRecordCipherScatter, AuthFailAndMalformed) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        const SuiteVectors &v = tls13_suites()[0];
        const auto plain = ramp(40, 0x80);

        TlsRecordCipher sealer;
        init_cipher(sealer, v);
        std::vector<std::uint8_t> wire(sealer.seal_output_size(plain.size()));
        ASSERT_EQ(sealer.seal(TlsContentType::ApplicationData, plain, wire).status, TlsRecordCipher::Status::Ok);
        const std::uint16_t length = static_cast<std::uint16_t>(wire.size());
        const std::size_t body_len = wire.size() - 16;

        TlsRecordCipher opener;
        init_cipher(opener, v);
        std::vector<std::uint8_t> dst(opener.open_output_size(length)); // 1.3: -16, type workspace included
        std::vector<std::uint8_t> bad = wire;
        bad.back() ^= 0x01; // tag bit
        EXPECT_EQ(opener.open_scatter(TlsContentType::ApplicationData, 0x0303, length, {},
                                      std::span<const std::uint8_t>(bad.data(), body_len),
                                      std::span<const std::uint8_t>(bad.data() + body_len, 16), dst)
                          .status,
                  TlsRecordCipher::Status::AuthFail);
        bad = wire;
        bad[10] ^= 0x01; // body bit
        EXPECT_EQ(opener.open_scatter(TlsContentType::ApplicationData, 0x0303, length, {},
                                      std::span<const std::uint8_t>(bad.data(), body_len),
                                      std::span<const std::uint8_t>(bad.data() + body_len, 16), dst)
                          .status,
                  TlsRecordCipher::Status::AuthFail);
        EXPECT_EQ(opener.sequence(), 0u); // failures never advance

        // Out-of-range lengths are Malformed BEFORE any buffer contract assert
        // (the tiny spans below must not trip the capacity checks).
        EXPECT_EQ(opener.open_scatter(TlsContentType::ApplicationData, 0x0303, 16, {}, {}, {}, dst).status,
                  TlsRecordCipher::Status::Malformed);
        EXPECT_EQ(opener.open_scatter(TlsContentType::ApplicationData, 0x0303,
                                      static_cast<std::uint16_t>(fiber::tls::kTlsMaxPlaintextSize + 257), {}, {}, {},
                                      dst)
                          .status,
                  TlsRecordCipher::Status::Malformed);
    });
}

// ---------------------------------------------------------------- move

// TlsConnectedState carries both traffic ciphers out of the handshake engine
// by move: the sequence number must travel with the instance (record-
// protection continuity), and the moved-from instance must be inert (fresh
// state, safe destructor, re-initializable per the single-shot init contract).
TEST(TlsRecordCipherMove, StealsSequenceAndNeutralizesSource) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        for (const SuiteVectors &v: tls13_suites()) {
            TlsRecordCipher cipher;
            init_cipher(cipher, v);
            const auto first = ramp(32, 0x40);
            std::vector<std::uint8_t> wire(cipher.seal_output_size(first.size()));
            ASSERT_EQ(cipher.seal(TlsContentType::Handshake, first, wire).status, TlsRecordCipher::Status::Ok);
            ASSERT_EQ(cipher.sequence(), 1u);

            // Move-construct: state travels, source goes inert.
            TlsRecordCipher moved(std::move(cipher));
            EXPECT_TRUE(moved.initialized());
            EXPECT_EQ(moved.sequence(), 1u);
            EXPECT_EQ(moved.suite(), v.suite);
            EXPECT_FALSE(cipher.initialized());
            EXPECT_EQ(cipher.sequence(), 0u);

            // The moved instance continues the epoch: its record at seq=1 must
            // match the hand-built expected wire — proof the key bytes traveled.
            const auto second = ramp(48, 0x80);
            std::vector<std::uint8_t> wire2(moved.seal_output_size(second.size()));
            ASSERT_EQ(moved.seal(TlsContentType::ApplicationData, second, wire2).status, TlsRecordCipher::Status::Ok);
            EXPECT_EQ(wire2, expected_tls13_payload(v.suite, v.key, v.iv, 1, TlsContentType::ApplicationData, second));

            // Move-assign over an initialized target: target cleans up, then takes
            // the source's place.
            TlsRecordCipher target;
            init_cipher(target, tls13_suites()[1]);
            ASSERT_EQ(target.sequence(), 0u);
            target = std::move(moved);
            EXPECT_TRUE(target.initialized());
            EXPECT_EQ(target.sequence(), 2u);
            EXPECT_FALSE(moved.initialized());

            // The moved-from instances are re-initializable (single-shot init
            // contract applies to the fresh state) and destroy safely at scope end.
            init_cipher(cipher, v);
            EXPECT_TRUE(cipher.initialized());
        }
    });
}

TEST(TlsRecordCipherMove, Tls12ExplicitNonceContinuesAcrossMove) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        const SuiteVectors &v = tls12_suites()[0];
        TlsRecordCipher cipher;
        init_cipher(cipher, v);
        const auto plain = ramp(20, 0x33);
        std::vector<std::uint8_t> wire(cipher.seal_output_size(plain.size()));
        ASSERT_EQ(cipher.seal(TlsContentType::ApplicationData, plain, wire).status, TlsRecordCipher::Status::Ok);

        TlsRecordCipher moved(std::move(cipher));
        const auto plain2 = ramp(20, 0x44);
        std::vector<std::uint8_t> wire2(moved.seal_output_size(plain2.size()));
        ASSERT_EQ(moved.seal(TlsContentType::ApplicationData, plain2, wire2).status, TlsRecordCipher::Status::Ok);
        // 1.2's explicit nonce IS the sequence number on the wire.
        EXPECT_EQ(wire2, expected_tls12_payload(v.suite, v.key, v.iv, 1, TlsContentType::ApplicationData, plain2));
    });
}

} // namespace
