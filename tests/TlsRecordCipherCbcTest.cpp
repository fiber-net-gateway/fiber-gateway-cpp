#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include <openssl/aes.h>
#include <openssl/digest.h>
#include <openssl/hmac.h>

#include <fiber/tls/record/TlsRecord.h>
#include <fiber/tls/record/TlsRecordCipher.h>

// TLS 1.2 CBC record protection (feature/tls/11) against an INDEPENDENT
// construction: expected records are assembled by hand per RFC 5246
// §6.2.3.2 — HMAC over seq || type || version || length || plaintext, then
// padding whose every byte equals the padding length, then AES-CBC under the
// record's IV — with the raw AES and HMAC one-shots, sharing no code path
// with the BoringSSL TLS CBC AEADs the cipher drives.

namespace {

using fiber::tls::kTlsMaxCiphertextOverhead12;
using fiber::tls::kTlsMaxPlaintextSize;
using fiber::tls::TlsCipherSuiteId;
using fiber::tls::TlsContentType;
using fiber::tls::TlsRecordCipher;
using fiber::tls::TlsRecordDirection;
using fiber::tls::TlsRecordProtectionKind;

constexpr std::uint16_t kTls12 = 0x0303;

struct CbcSuite {
    TlsCipherSuiteId suite;
    const EVP_MD *(*md)();
    std::size_t mac_len;
    std::size_t key_len;
};

const std::array<CbcSuite, 3> &cbc_suites() {
    static const std::array<CbcSuite, 3> suites{{
            {TlsCipherSuiteId::EcdheRsaAes128CbcSha, EVP_sha1, 20, 16},
            {TlsCipherSuiteId::EcdheRsaAes256CbcSha, EVP_sha1, 20, 32},
            {TlsCipherSuiteId::EcdheRsaAes128CbcSha256, EVP_sha256, 32, 16},
    }};
    return suites;
}

std::vector<std::uint8_t> ramp(std::size_t len, std::uint8_t seed) {
    std::vector<std::uint8_t> out(len);
    for (std::size_t i = 0; i < len; ++i) {
        out[i] = static_cast<std::uint8_t>(seed + i);
    }
    return out;
}

void store_be64(std::uint8_t *dst, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        dst[i] = static_cast<std::uint8_t>(value);
        value >>= 8;
    }
}

struct CbcKeys {
    std::vector<std::uint8_t> mac_key;
    std::vector<std::uint8_t> enc_key;

    // The record cipher's key: MAC key || encryption key.
    [[nodiscard]] std::vector<std::uint8_t> merged() const {
        std::vector<std::uint8_t> out = mac_key;
        out.insert(out.end(), enc_key.begin(), enc_key.end());
        return out;
    }
};

CbcKeys keys_for(const CbcSuite &s) { return {ramp(s.mac_len, 0x11), ramp(s.key_len, 0x5A)}; }

TlsRecordCipher make_cipher(const CbcSuite &s, const CbcKeys &k, TlsRecordDirection direction) {
    TlsRecordCipher cipher;
    const auto key = k.merged();
    EXPECT_TRUE(cipher.init(s.suite, TlsRecordProtectionKind::Tls12, direction, key, {}).has_value());
    return cipher;
}

std::vector<std::uint8_t> ref_mac(const CbcSuite &s, const CbcKeys &k, std::uint64_t seq, TlsContentType type,
                                  std::uint16_t version, std::span<const std::uint8_t> plain) {
    std::vector<std::uint8_t> input(13 + plain.size());
    store_be64(input.data(), seq);
    input[8] = static_cast<std::uint8_t>(type);
    input[9] = static_cast<std::uint8_t>(version >> 8);
    input[10] = static_cast<std::uint8_t>(version);
    input[11] = static_cast<std::uint8_t>(plain.size() >> 8);
    input[12] = static_cast<std::uint8_t>(plain.size());
    if (!plain.empty()) {
        std::memcpy(input.data() + 13, plain.data(), plain.size());
    }
    std::vector<std::uint8_t> mac(EVP_MAX_MD_SIZE);
    unsigned mac_len = 0;
    EXPECT_NE(nullptr,
              HMAC(s.md(), k.mac_key.data(), k.mac_key.size(), input.data(), input.size(), mac.data(), &mac_len));
    mac.resize(mac_len);
    return mac;
}

struct RefSealOptions {
    // Replaces the minimal canonical padding (its length must keep the body
    // whole blocks).
    std::optional<std::vector<std::uint8_t>> padding;
    bool corrupt_mac = false;
};

// IV || AES-CBC(plaintext || MAC || padding).
std::vector<std::uint8_t> ref_seal(const CbcSuite &s, const CbcKeys &k, std::span<const std::uint8_t> iv,
                                   std::uint64_t seq, TlsContentType type, std::span<const std::uint8_t> plain,
                                   const RefSealOptions &opt = {}) {
    std::vector<std::uint8_t> mac = ref_mac(s, k, seq, type, kTls12, plain);
    if (opt.corrupt_mac) {
        mac[0] ^= 0x01;
    }
    std::vector<std::uint8_t> body(plain.begin(), plain.end());
    body.insert(body.end(), mac.begin(), mac.end());
    if (opt.padding.has_value()) {
        body.insert(body.end(), opt.padding->begin(), opt.padding->end());
    } else {
        const std::size_t pad = 16 - body.size() % 16; // 1..16 bytes, each = pad - 1
        body.insert(body.end(), pad, static_cast<std::uint8_t>(pad - 1));
    }
    EXPECT_EQ(0u, body.size() % 16);

    AES_KEY aes;
    EXPECT_EQ(0, AES_set_encrypt_key(k.enc_key.data(), static_cast<unsigned>(k.enc_key.size() * 8), &aes));
    std::array<std::uint8_t, 16> chain_iv{};
    std::memcpy(chain_iv.data(), iv.data(), 16);
    std::vector<std::uint8_t> out(16 + body.size());
    std::memcpy(out.data(), iv.data(), 16);
    AES_cbc_encrypt(body.data(), out.data() + 16, body.size(), &aes, chain_iv.data(), AES_ENCRYPT);
    return out;
}

// Decrypts and checks a record per RFC 5246 §6.2.3.2; the plaintext when the
// padding and the MAC both verify.
std::optional<std::vector<std::uint8_t>> ref_open(const CbcSuite &s, const CbcKeys &k, std::uint64_t seq,
                                                  TlsContentType type, std::span<const std::uint8_t> record) {
    if (record.size() < 32 || (record.size() - 16) % 16 != 0) {
        return std::nullopt;
    }
    AES_KEY aes;
    EXPECT_EQ(0, AES_set_decrypt_key(k.enc_key.data(), static_cast<unsigned>(k.enc_key.size() * 8), &aes));
    std::array<std::uint8_t, 16> chain_iv{};
    std::memcpy(chain_iv.data(), record.data(), 16);
    std::vector<std::uint8_t> body(record.size() - 16);
    AES_cbc_encrypt(record.data() + 16, body.data(), body.size(), &aes, chain_iv.data(), AES_DECRYPT);

    const std::size_t pad = body.back();
    if (pad + 1 + s.mac_len > body.size()) {
        return std::nullopt;
    }
    for (std::size_t i = body.size() - pad - 1; i < body.size(); ++i) {
        if (body[i] != pad) {
            return std::nullopt;
        }
    }
    const std::size_t data_len = body.size() - pad - 1 - s.mac_len;
    const std::vector<std::uint8_t> data(body.begin(), body.begin() + static_cast<std::ptrdiff_t>(data_len));
    const std::vector<std::uint8_t> mac = ref_mac(s, k, seq, type, kTls12, data);
    if (std::memcmp(mac.data(), body.data() + data_len, s.mac_len) != 0) {
        return std::nullopt;
    }
    return data;
}

TlsRecordCipher::OpenResult open_record(TlsRecordCipher &cipher, TlsContentType type,
                                        std::span<const std::uint8_t> record, std::vector<std::uint8_t> &out,
                                        std::uint16_t version = kTls12) {
    out.assign(record.size(), 0);
    return cipher.open(type, version, static_cast<std::uint16_t>(record.size()), record, out);
}

std::vector<std::uint8_t> seal_record(TlsRecordCipher &cipher, TlsContentType type,
                                      std::span<const std::uint8_t> plain) {
    std::vector<std::uint8_t> out(cipher.seal_output_size(plain.size()));
    const auto sealed = cipher.seal(type, plain, out);
    EXPECT_EQ(TlsRecordCipher::Status::Ok, sealed.status);
    EXPECT_EQ(out.size(), sealed.out_len);
    return out;
}

constexpr std::array<std::size_t, 11> kSizes{0, 1, 15, 16, 17, 31, 32, 47, 255, 1000, kTlsMaxPlaintextSize};

// ---------------------------------------------------------------- queries

TEST(TlsRecordCipherCbc, QueriesDescribeTheCbcConstruction) {
    for (const CbcSuite &s: cbc_suites()) {
        const CbcKeys k = keys_for(s);
        TlsRecordCipher cipher = make_cipher(s, k, TlsRecordDirection::Seal);
        EXPECT_TRUE(cipher.is_cbc());
        EXPECT_EQ(0u, cipher.detached_tag_len());
        EXPECT_EQ(16u, cipher.explicit_nonce_len());
        EXPECT_EQ(TlsRecordDirection::Seal, cipher.direction());
        // IV + one block-padded MAC-and-padding body: 48 (SHA-1) / 64 (SHA-256).
        EXPECT_EQ(s.mac_len == 20 ? 48u : 64u, cipher.min_ciphertext_size());
        EXPECT_EQ(kTlsMaxPlaintextSize + kTlsMaxCiphertextOverhead12, cipher.max_ciphertext_size());
        EXPECT_EQ(cipher.min_ciphertext_size(), cipher.seal_output_size(0));
        EXPECT_EQ(48u, cipher.open_output_size(64));
    }

    TlsRecordCipher aead;
    ASSERT_TRUE(aead.init(TlsCipherSuiteId::EcdheRsaAes128GcmSha256, TlsRecordProtectionKind::Tls12,
                          TlsRecordDirection::Seal, ramp(16, 1), ramp(4, 2))
                        .has_value());
    EXPECT_FALSE(aead.is_cbc());
    EXPECT_EQ(16u, aead.detached_tag_len());
}

// ---------------------------------------------------------------- known answers

TEST(TlsRecordCipherCbc, OpenMatchesIndependentConstruction) {
    for (const CbcSuite &s: cbc_suites()) {
        const CbcKeys k = keys_for(s);
        TlsRecordCipher opener = make_cipher(s, k, TlsRecordDirection::Open);
        std::uint64_t seq = 0;
        for (const std::size_t n: kSizes) {
            const TlsContentType type = seq % 2 == 0 ? TlsContentType::ApplicationData : TlsContentType::Handshake;
            const auto plain = ramp(n, static_cast<std::uint8_t>(seq));
            const auto iv = ramp(16, static_cast<std::uint8_t>(0x90 + seq));
            const auto record = ref_seal(s, k, iv, seq, type, plain);

            std::vector<std::uint8_t> out;
            const auto opened = open_record(opener, type, record, out);
            ASSERT_EQ(TlsRecordCipher::Status::Ok, opened.status) << "n=" << n;
            EXPECT_EQ(type, opened.inner_type);
            ASSERT_EQ(n, opened.plain_len);
            EXPECT_EQ(0, n == 0 ? 0 : std::memcmp(out.data(), plain.data(), n));
            ++seq;
            EXPECT_EQ(seq, opener.sequence());
        }
    }
}

TEST(TlsRecordCipherCbc, SealDecryptsIndependently) {
    for (const CbcSuite &s: cbc_suites()) {
        const CbcKeys k = keys_for(s);
        TlsRecordCipher sealer = make_cipher(s, k, TlsRecordDirection::Seal);
        std::uint64_t seq = 0;
        for (const std::size_t n: kSizes) {
            const TlsContentType type = seq % 2 == 0 ? TlsContentType::Handshake : TlsContentType::ApplicationData;
            const auto plain = ramp(n, static_cast<std::uint8_t>(0x40 + seq));
            const auto record = seal_record(sealer, type, plain);

            // IV || whole blocks holding plaintext || MAC || 1..16 padding bytes.
            ASSERT_EQ(0u, (record.size() - 16) % 16);
            const std::size_t overhead = record.size() - 16 - n;
            EXPECT_GE(overhead, s.mac_len + 1);
            EXPECT_LE(overhead, s.mac_len + 16);

            const auto recovered = ref_open(s, k, seq, type, record);
            ASSERT_TRUE(recovered.has_value()) << "n=" << n;
            EXPECT_EQ(plain, *recovered);
            ++seq;
        }
    }
}

TEST(TlsRecordCipherCbc, SealOutputSizeIsExactAndEveryIvIsFresh) {
    for (const CbcSuite &s: cbc_suites()) {
        const CbcKeys k = keys_for(s);
        TlsRecordCipher sealer = make_cipher(s, k, TlsRecordDirection::Seal);
        std::vector<std::uint8_t> previous_iv;
        for (std::size_t n = 0; n <= 64; ++n) {
            const auto plain = ramp(n, 0x21);
            const std::size_t expected = 16 + (n + s.mac_len + 16) / 16 * 16;
            EXPECT_EQ(expected, sealer.seal_output_size(n));
            const auto record = seal_record(sealer, TlsContentType::ApplicationData, plain);
            ASSERT_EQ(expected, record.size());
            const std::vector<std::uint8_t> iv(record.begin(), record.begin() + 16);
            EXPECT_NE(previous_iv, iv); // a fresh random IV per record (RFC 5246 §6.2.3.2)
            previous_iv = iv;
        }
    }
}

// RFC 5246 §6.2.3.2 allows up to 255 padding bytes; a peer choosing more than
// the minimum (to hide lengths) must be accepted.
TEST(TlsRecordCipherCbc, LongerValidPaddingIsAccepted) {
    for (const CbcSuite &s: cbc_suites()) {
        const CbcKeys k = keys_for(s);
        TlsRecordCipher opener = make_cipher(s, k, TlsRecordDirection::Open);
        const auto plain = ramp(10, 0x33);
        const std::size_t minimal = 16 - (plain.size() + s.mac_len) % 16;
        const std::size_t pad_bytes = minimal + 64;
        const std::vector<std::uint8_t> padding(pad_bytes, static_cast<std::uint8_t>(pad_bytes - 1));
        const auto record =
                ref_seal(s, k, ramp(16, 7), 0, TlsContentType::ApplicationData, plain, {.padding = padding});

        std::vector<std::uint8_t> out;
        const auto opened = open_record(opener, TlsContentType::ApplicationData, record, out);
        ASSERT_EQ(TlsRecordCipher::Status::Ok, opened.status);
        ASSERT_EQ(plain.size(), opened.plain_len);
        EXPECT_EQ(0, std::memcmp(out.data(), plain.data(), plain.size()));
    }
}

// ---------------------------------------------------------------- in place and scatter

TEST(TlsRecordCipherCbc, InPlaceSealAndOpen) {
    for (const CbcSuite &s: cbc_suites()) {
        const CbcKeys k = keys_for(s);
        TlsRecordCipher sealer = make_cipher(s, k, TlsRecordDirection::Seal);
        TlsRecordCipher opener = make_cipher(s, k, TlsRecordDirection::Open);
        const auto plain = ramp(100, 0x61);

        // seal: the plaintext sits right after the 16-byte IV slot.
        std::vector<std::uint8_t> buf(sealer.seal_output_size(plain.size()));
        std::memcpy(buf.data() + 16, plain.data(), plain.size());
        const auto sealed = sealer.seal(TlsContentType::ApplicationData, {buf.data() + 16, plain.size()}, buf);
        ASSERT_EQ(TlsRecordCipher::Status::Ok, sealed.status);
        ASSERT_EQ(buf.size(), sealed.out_len);
        const auto recovered = ref_open(s, k, 0, TlsContentType::ApplicationData, buf);
        ASSERT_TRUE(recovered.has_value());
        EXPECT_EQ(plain, *recovered);

        // open: the plaintext lands right after the IV, over the ciphertext.
        const auto length = static_cast<std::uint16_t>(buf.size());
        const auto opened =
                opener.open(TlsContentType::ApplicationData, kTls12, length, buf, {buf.data() + 16, buf.size() - 16});
        ASSERT_EQ(TlsRecordCipher::Status::Ok, opened.status);
        ASSERT_EQ(plain.size(), opened.plain_len);
        EXPECT_EQ(0, std::memcmp(buf.data() + 16, plain.data(), plain.size()));
    }
}

TEST(TlsRecordCipherCbc, ScatterFormsCrossOpen) {
    for (const CbcSuite &s: cbc_suites()) {
        const CbcKeys k = keys_for(s);
        TlsRecordCipher sealer = make_cipher(s, k, TlsRecordDirection::Seal);
        TlsRecordCipher opener = make_cipher(s, k, TlsRecordDirection::Open);

        // seal_scatter: IV to its own span, ciphertext in place over the
        // plaintext, the MAC tail || padding to the tag span.
        const auto plain = ramp(77, 0x70);
        std::vector<std::uint8_t> ct = plain;
        std::array<std::uint8_t, 16> prefix{};
        std::array<std::uint8_t, 48> tag{};
        const auto sealed = sealer.seal_scatter(TlsContentType::Handshake, plain, ct, prefix, tag);
        ASSERT_EQ(TlsRecordCipher::Status::Ok, sealed.status);
        ASSERT_EQ(sealer.seal_output_size(plain.size()), sealed.out_len);
        std::vector<std::uint8_t> wire(prefix.begin(), prefix.end());
        wire.insert(wire.end(), ct.begin(), ct.end());
        wire.insert(wire.end(), tag.begin(), tag.begin() + static_cast<std::ptrdiff_t>(sealed.out_len - 16 - 77));
        ASSERT_EQ(sealed.out_len, wire.size());
        const auto recovered = ref_open(s, k, 0, TlsContentType::Handshake, wire);
        ASSERT_TRUE(recovered.has_value());
        EXPECT_EQ(plain, *recovered);

        // open_scatter: the IV and the body as separate spans, no tag span.
        std::vector<std::uint8_t> dst(wire.size() - 16);
        const auto opened =
                opener.open_scatter(TlsContentType::Handshake, kTls12, static_cast<std::uint16_t>(wire.size()),
                                    {wire.data(), 16}, {wire.data() + 16, wire.size() - 16}, {}, dst);
        ASSERT_EQ(TlsRecordCipher::Status::Ok, opened.status);
        ASSERT_EQ(plain.size(), opened.plain_len);
        EXPECT_EQ(0, std::memcmp(dst.data(), plain.data(), plain.size()));
    }
}

// ---------------------------------------------------------------- failures

TEST(TlsRecordCipherCbc, TamperingIsAuthFail) {
    for (const CbcSuite &s: cbc_suites()) {
        const CbcKeys k = keys_for(s);
        const auto plain = ramp(12, 0x44); // 12 + MAC keeps the padding choices below block-aligned
        const auto iv = ramp(16, 0x99);
        const auto good = ref_seal(s, k, iv, 0, TlsContentType::ApplicationData, plain);

        std::vector<std::vector<std::uint8_t>> bad;
        for (const std::size_t at: {std::size_t{0}, std::size_t{16}, good.size() - 17, good.size() - 1}) {
            auto flipped = good; // IV, first body byte, MAC-bearing block, padding byte
            flipped[at] ^= 0x01;
            bad.push_back(flipped);
        }
        bad.push_back(ref_seal(s, k, iv, 0, TlsContentType::ApplicationData, plain, {.corrupt_mac = true}));
        // Padding bytes that disagree with the padding length.
        const std::size_t pad = 16 - (plain.size() + s.mac_len) % 16;
        std::vector<std::uint8_t> uneven(pad, static_cast<std::uint8_t>(pad - 1));
        uneven[0] ^= 0x01;
        bad.push_back(ref_seal(s, k, iv, 0, TlsContentType::ApplicationData, plain, {.padding = uneven}));
        // A padding length claiming more bytes than the record holds.
        bad.push_back(ref_seal(s, k, iv, 0, TlsContentType::ApplicationData, plain,
                               {.padding = std::vector<std::uint8_t>(pad, 0xFE)}));

        for (std::size_t i = 0; i < bad.size(); ++i) {
            TlsRecordCipher opener = make_cipher(s, k, TlsRecordDirection::Open);
            std::vector<std::uint8_t> out;
            EXPECT_EQ(TlsRecordCipher::Status::AuthFail,
                      open_record(opener, TlsContentType::ApplicationData, bad[i], out).status)
                    << "case " << i;
            EXPECT_EQ(0u, opener.sequence());
        }

        // The type and version are bound through the MAC.
        TlsRecordCipher type_opener = make_cipher(s, k, TlsRecordDirection::Open);
        std::vector<std::uint8_t> out;
        EXPECT_EQ(TlsRecordCipher::Status::AuthFail,
                  open_record(type_opener, TlsContentType::Handshake, good, out).status);
        TlsRecordCipher version_opener = make_cipher(s, k, TlsRecordDirection::Open);
        EXPECT_EQ(TlsRecordCipher::Status::AuthFail,
                  open_record(version_opener, TlsContentType::ApplicationData, good, out, 0x0301).status);
        TlsRecordCipher ok_opener = make_cipher(s, k, TlsRecordDirection::Open);
        EXPECT_EQ(TlsRecordCipher::Status::Ok,
                  open_record(ok_opener, TlsContentType::ApplicationData, good, out).status);
    }
}

TEST(TlsRecordCipherCbc, PublicLengthViolationsAreMalformedBeforeCrypto) {
    for (const CbcSuite &s: cbc_suites()) {
        const CbcKeys k = keys_for(s);
        TlsRecordCipher opener = make_cipher(s, k, TlsRecordDirection::Open);
        const auto good = ref_seal(s, k, ramp(16, 3), 0, TlsContentType::ApplicationData, ramp(40, 9));
        std::vector<std::uint8_t> big(kTlsMaxPlaintextSize + kTlsMaxCiphertextOverhead12 + 32, 0);
        std::memcpy(big.data(), good.data(), good.size());
        std::vector<std::uint8_t> out(big.size());

        const auto misaligned = static_cast<std::uint16_t>(good.size() - 1);
        const auto too_short = static_cast<std::uint16_t>(opener.min_ciphertext_size() - 16);
        const auto too_long = static_cast<std::uint16_t>(opener.max_ciphertext_size() + 16);
        for (const std::uint16_t length: {misaligned, too_short, too_long}) {
            const auto opened = opener.open(TlsContentType::ApplicationData, kTls12, length, big, out);
            EXPECT_EQ(TlsRecordCipher::Status::Malformed, opened.status) << "length " << length;
            EXPECT_EQ(0u, opener.sequence());
        }
        EXPECT_EQ(TlsRecordCipher::Status::Ok, open_record(opener, TlsContentType::ApplicationData, good, out).status);
    }
}

TEST(TlsRecordCipherCbc, AuthenticatedOversizePlaintextIsOverflow) {
    for (const CbcSuite &s: cbc_suites()) {
        const CbcKeys k = keys_for(s);
        const auto iv = ramp(16, 0x0F);
        std::vector<std::uint8_t> out;

        TlsRecordCipher at_limit = make_cipher(s, k, TlsRecordDirection::Open);
        const auto limit = ref_seal(s, k, iv, 0, TlsContentType::ApplicationData, ramp(kTlsMaxPlaintextSize, 1));
        EXPECT_EQ(TlsRecordCipher::Status::Ok,
                  open_record(at_limit, TlsContentType::ApplicationData, limit, out).status);

        TlsRecordCipher over = make_cipher(s, k, TlsRecordDirection::Open);
        const auto oversized =
                ref_seal(s, k, iv, 0, TlsContentType::ApplicationData, ramp(kTlsMaxPlaintextSize + 1, 1));
        ASSERT_LE(oversized.size(), over.max_ciphertext_size());
        EXPECT_EQ(TlsRecordCipher::Status::Overflow,
                  open_record(over, TlsContentType::ApplicationData, oversized, out).status);
    }
}

TEST(TlsRecordCipherCbc, SequenceNumberBindsEachRecord) {
    for (const CbcSuite &s: cbc_suites()) {
        const CbcKeys k = keys_for(s);
        const auto record = ref_seal(s, k, ramp(16, 5), 0, TlsContentType::ApplicationData, ramp(20, 5));
        TlsRecordCipher opener = make_cipher(s, k, TlsRecordDirection::Open);
        std::vector<std::uint8_t> out;
        EXPECT_EQ(TlsRecordCipher::Status::Ok,
                  open_record(opener, TlsContentType::ApplicationData, record, out).status);
        // Replayed at seq 1: the MAC covers seq 0.
        EXPECT_EQ(TlsRecordCipher::Status::AuthFail,
                  open_record(opener, TlsContentType::ApplicationData, record, out).status);
    }
}

// ---------------------------------------------------------------- lifecycle

TEST(TlsRecordCipherCbc, MoveCarriesSequenceAndOwnership) {
    for (const CbcSuite &s: cbc_suites()) {
        const CbcKeys k = keys_for(s);
        TlsRecordCipher a = make_cipher(s, k, TlsRecordDirection::Seal);
        const auto r0 = seal_record(a, TlsContentType::ApplicationData, ramp(30, 0));
        TlsRecordCipher b(std::move(a));
        EXPECT_FALSE(a.initialized());
        const auto r1 = seal_record(b, TlsContentType::ApplicationData, ramp(31, 1));
        TlsRecordCipher c;
        c = std::move(b);
        EXPECT_FALSE(b.initialized());
        EXPECT_TRUE(c.is_cbc());
        const auto r2 = seal_record(c, TlsContentType::ApplicationData, ramp(32, 2));
        EXPECT_EQ(3u, c.sequence());

        TlsRecordCipher opener = make_cipher(s, k, TlsRecordDirection::Open);
        std::vector<std::uint8_t> out;
        EXPECT_EQ(TlsRecordCipher::Status::Ok, open_record(opener, TlsContentType::ApplicationData, r0, out).status);
        TlsRecordCipher moved_opener(std::move(opener));
        EXPECT_EQ(TlsRecordCipher::Status::Ok,
                  open_record(moved_opener, TlsContentType::ApplicationData, r1, out).status);
        EXPECT_EQ(TlsRecordCipher::Status::Ok,
                  open_record(moved_opener, TlsContentType::ApplicationData, r2, out).status);
        EXPECT_EQ(3u, moved_opener.sequence());
    }
}

TEST(TlsRecordCipherCbc, InitRejectsMismatchedMaterial) {
    const CbcSuite &s = cbc_suites()[0];
    const CbcKeys k = keys_for(s);
    const auto merged = k.merged();

    TlsRecordCipher wrong_kind;
    EXPECT_FALSE(
            wrong_kind.init(s.suite, TlsRecordProtectionKind::Tls13, TlsRecordDirection::Seal, merged, {}).has_value());
    TlsRecordCipher no_mac_key;
    EXPECT_FALSE(no_mac_key.init(s.suite, TlsRecordProtectionKind::Tls12, TlsRecordDirection::Seal, k.enc_key, {})
                         .has_value());
    TlsRecordCipher with_iv;
    EXPECT_FALSE(with_iv.init(s.suite, TlsRecordProtectionKind::Tls12, TlsRecordDirection::Seal, merged, ramp(4, 0))
                         .has_value());
    TlsRecordCipher good;
    EXPECT_TRUE(good.init(s.suite, TlsRecordProtectionKind::Tls12, TlsRecordDirection::Open, merged, {}).has_value());
}

// The TLS CBC AEADs are direction-bound; using an instance the other way is
// an engine bug.
TEST(TlsRecordCipherCbcDeathTest, WrongDirectionAsserts) {
    const CbcSuite &s = cbc_suites()[0];
    const CbcKeys k = keys_for(s);
    const auto record = ref_seal(s, k, ramp(16, 1), 0, TlsContentType::ApplicationData, ramp(8, 1));
    EXPECT_DEATH(
            {
                TlsRecordCipher sealer = make_cipher(s, k, TlsRecordDirection::Seal);
                std::vector<std::uint8_t> out;
                (void) open_record(sealer, TlsContentType::ApplicationData, record, out);
            },
            "FIBER_ASSERT failed");
    EXPECT_DEATH(
            {
                TlsRecordCipher opener = make_cipher(s, k, TlsRecordDirection::Open);
                (void) seal_record(opener, TlsContentType::ApplicationData, ramp(8, 1));
            },
            "FIBER_ASSERT failed");
}

} // namespace
