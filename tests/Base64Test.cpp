// KAT and round-trip tests for the Huffman-style base64 two-phase primitives
// (feature/script_noexcept_audit.md ID 1/2). RFC 4648 §4 vectors are the oracle.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <fiber/common/util/Base64.h>

using fiber::util::base64_decode_into;
using fiber::util::base64_decoded_size;
using fiber::util::base64_encode_into;
using fiber::util::base64_encoded_size;

namespace {

// RFC 4648 §4 test vectors.
struct Vector {
    std::string_view plain;
    std::string_view encoded;
};

constexpr Vector kVectors[] = {
        {.plain = "", .encoded = ""},
        {.plain = "f", .encoded = "Zg=="},
        {.plain = "fo", .encoded = "Zm8="},
        {.plain = "foo", .encoded = "Zm9v"},
        {.plain = "foob", .encoded = "Zm9vYg=="},
        {.plain = "fooba", .encoded = "Zm9vYmE="},
        {.plain = "foobar", .encoded = "Zm9vYmFy"},
};

// Pseudo-random but deterministic payload covering all residues mod 3.
std::vector<std::uint8_t> payload(std::size_t len) {
    std::vector<std::uint8_t> bytes(len);
    std::uint32_t x = 0x9E3779B9U;
    for (std::size_t i = 0; i < len; ++i) {
        x = x * 1664525U + 1013904223U;
        bytes[i] = static_cast<std::uint8_t>(x >> 24);
    }
    return bytes;
}

std::string encode(const std::uint8_t *data, std::size_t len) {
    std::string out(base64_encoded_size(len), '\0');
    if (len > 0) {
        base64_encode_into(data, len, out.data());
    }
    return out;
}

} // namespace

TEST(Base64Test, EncodedSizeFormula) {
    for (std::size_t len = 0; len <= 8; ++len) {
        EXPECT_EQ(base64_encoded_size(len), ((len + 2) / 3) * 4);
    }
    EXPECT_EQ(base64_encoded_size(0), 0U);
    EXPECT_EQ(base64_encoded_size(1), 4U);
    EXPECT_EQ(base64_encoded_size(2), 4U);
    EXPECT_EQ(base64_encoded_size(3), 4U);
    EXPECT_EQ(base64_encoded_size(4), 8U);
}

TEST(Base64Test, EncodeIntoKatVectors) {
    for (const Vector &v: kVectors) {
        const auto *const plain = reinterpret_cast<const std::uint8_t *>(v.plain.data());
        EXPECT_EQ(encode(plain, v.plain.size()), v.encoded);
    }
}

TEST(Base64Test, DecodeIntoKatVectors) {
    for (const Vector &v: kVectors) {
        bool ok = false;
        const std::size_t len = base64_decoded_size(v.encoded, ok);
        ASSERT_TRUE(ok) << "encoded=" << v.encoded;
        ASSERT_EQ(len, v.plain.size()) << "encoded=" << v.encoded;

        std::string decoded(len, '\0');
        ASSERT_TRUE(base64_decode_into(v.encoded, reinterpret_cast<std::uint8_t *>(decoded.data())));
        EXPECT_EQ(decoded, v.plain);
    }
}

TEST(Base64Test, RoundTripAllLengths) {
    for (std::size_t len = 0; len <= 70; ++len) {
        const std::vector<std::uint8_t> bytes = payload(len);

        const std::string encoded = encode(bytes.data(), len);
        EXPECT_EQ(encoded.size(), base64_encoded_size(len)) << "len=" << len;

        bool ok = false;
        const std::size_t dec_len = base64_decoded_size(encoded, ok);
        ASSERT_TRUE(ok) << "len=" << len;
        ASSERT_EQ(dec_len, len) << "len=" << len;

        std::vector<std::uint8_t> decoded(dec_len);
        ASSERT_TRUE(base64_decode_into(encoded, decoded.data())) << "len=" << len;
        EXPECT_EQ(decoded, bytes) << "len=" << len;
    }
}

TEST(Base64Test, DecodedSizeRejectsStructuralErrors) {
    struct Case {
        std::string_view in;
        bool ok;
        std::size_t len;
    };
    const Case cases[] = {
            {.in = "", .ok = true, .len = 0},         {.in = "QQ==", .ok = true, .len = 1},
            {.in = "QUI=", .ok = true, .len = 2},     {.in = "QUJD", .ok = true, .len = 3},
            {.in = "QUJDRA==", .ok = true, .len = 4}, {.in = "A", .ok = false, .len = 0}, // length % 4
            {.in = "ABCD ", .ok = false, .len = 0}, // length % 4
            {.in = "A===", .ok = false, .len = 0}, // overlong padding
            {.in = "====", .ok = false, .len = 0}, // overlong padding
            {.in = "AB=C", .ok = false, .len = 0}, // '=' mid-stream
            {.in = "=ABC", .ok = false, .len = 0}, // '=' at start
    };
    for (const Case &c: cases) {
        bool ok = true;
        const std::size_t len = base64_decoded_size(c.in, ok);
        EXPECT_EQ(ok, c.ok) << "in=" << c.in;
        if (!c.ok) {
            EXPECT_EQ(len, 0U) << "in=" << c.in;
        } else {
            EXPECT_EQ(len, c.len) << "in=" << c.in;
        }
    }
}

TEST(Base64Test, DecodeIntoRejectsAlphabetErrors) {
    // Structurally valid, alphabet-invalid: decoded_size passes, decode_into fails.
    for (const std::string_view in: {"####", "SGVs bG8", "AB-D", "AB?D"}) {
        bool ok = false;
        (void) base64_decoded_size(in, ok);
        EXPECT_TRUE(ok) << "in=" << in;

        std::string direct(64, '\0');
        EXPECT_FALSE(base64_decode_into(in, reinterpret_cast<std::uint8_t *>(direct.data()))) << "in=" << in;
    }
}

TEST(Base64Test, DecodeIntoStandaloneRejectsStructuralErrors) {
    // decode_into re-validates structure, so it is safe without the size gate.
    for (const std::string_view in: {"A", "A===", "AB=C", "===="}) {
        std::string direct(64, '\0');
        EXPECT_FALSE(base64_decode_into(in, reinterpret_cast<std::uint8_t *>(direct.data()))) << "in=" << in;
    }
}
