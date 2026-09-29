#ifndef FIBER_TLS_CERTIFICATE_TEST_SUPPORT_H
#define FIBER_TLS_CERTIFICATE_TEST_SUPPORT_H

#include <gtest/gtest.h>
#include <openssl/mem.h>
#include <openssl/obj.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <cstdint>
#include <string>
#include <vector>

#include "TlsCertFixtures.h"

namespace fiber::tls::certtest {

enum class KeyUsage { Absent, Signature, Encipherment, Both, Malformed, Duplicate };
inline constexpr KeyUsage kKeyUsages[] = {KeyUsage::Absent, KeyUsage::Signature, KeyUsage::Encipherment,
                                          KeyUsage::Both,   KeyUsage::Malformed, KeyUsage::Duplicate};

inline bssl::UniquePtr<X509> leaf() {
    bssl::UniquePtr<BIO> bio(BIO_new_mem_buf(certfix::kLeafRsaPem, -1));
    return bssl::UniquePtr<X509>(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
}

inline void set_key_usage(X509 *cert, KeyUsage usage) {
    int index;
    while ((index = X509_get_ext_by_NID(cert, NID_key_usage, -1)) >= 0) {
        X509_EXTENSION_free(X509_delete_ext(cert, index));
    }
    if (usage == KeyUsage::Absent) {
        return;
    }
    // DER BIT STRING: unused-bit count, then digitalSignature (0x80) and/or
    // keyEncipherment (0x20). The malformed case truncates the value byte.
    const unsigned char bits = usage == KeyUsage::Signature ? 0x80 : usage == KeyUsage::Encipherment ? 0x20 : 0xa0;
    const unsigned char encoded[] = {0x03, 0x02, static_cast<unsigned char>(bits == 0x80 ? 7 : 5), bits};
    bssl::UniquePtr<ASN1_OCTET_STRING> data(ASN1_OCTET_STRING_new());
    ASSERT_EQ(1, ASN1_OCTET_STRING_set(data.get(), encoded, usage == KeyUsage::Malformed ? 3 : 4));
    bssl::UniquePtr<X509_EXTENSION> ext(X509_EXTENSION_create_by_NID(nullptr, NID_key_usage, 1, data.get()));
    ASSERT_NE(nullptr, ext);
    ASSERT_EQ(1, X509_add_ext(cert, ext.get(), -1));
    if (usage == KeyUsage::Duplicate) {
        ASSERT_EQ(1, X509_add_ext(cert, ext.get(), -1));
    }
}

// All RSA fixtures share this key, including the issuing intermediate.
inline void sign(X509 *cert) {
    bssl::UniquePtr<BIO> bio(BIO_new_mem_buf(certfix::kRsa2048KeyPem, -1));
    bssl::UniquePtr<EVP_PKEY> key(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, key);
    ASSERT_GT(X509_sign(cert, key.get(), EVP_sha256()), 0);
}

inline std::vector<std::uint8_t> der(X509 *cert) {
    const int len = i2d_X509(cert, nullptr);
    EXPECT_GT(len, 0);
    if (len <= 0) {
        return {};
    }
    std::vector<std::uint8_t> bytes(len);
    auto *cursor = bytes.data();
    EXPECT_EQ(len, i2d_X509(cert, &cursor));
    return bytes;
}

inline std::string pem(X509 *cert) {
    bssl::UniquePtr<BIO> bio(BIO_new(BIO_s_mem()));
    EXPECT_EQ(1, PEM_write_bio_X509(bio.get(), cert));
    char *data = nullptr;
    const long len = BIO_get_mem_data(bio.get(), &data);
    return {data, static_cast<std::size_t>(len)};
}

inline void set_dsa_public_key(X509 *cert) {
    // A valid DSA SPKI: parsing succeeds, but DSA is not a supported TLS key.
    static constexpr char kDsaPublicKey[] = R"pem(-----BEGIN PUBLIC KEY-----
MIIBvjCCATMGByqGSM44BAEwggEmAoGBAJKbRNZqQia9zqGnE9u5Hf72bJayThc5
DjPdNVesHR/26hFC4fx3JxRXJLnDuJaxxzKtvV7PYhcpugnDqc1GYTc/6+kKOepS
2V4bN7hgfQifk+n+bCMf/xxNjpXwL/d3L6Pa0/N+DE6eV3wnpfcFCjzsKiJatu0a
P9W0cnhUEzjFAh0A8kY8fcR9RZR8x9sV6gNvU0y6PEb7WsrKP41luQKBgA3TD0pc
4oivUzsfPaPkpqmD0jXVforVbT2n3be7pzC740X9IouA8OT8VGiQ6EL21yM8Sjq8
2JvpeTGKu6rx9o1hNhGpM5ZcB4SznN+rXaYBCWojwj4aEQgCiwqR4FUdmiYXKJkc
Poeied4cly4ZZT60ZF2WnWD6a+92PwR719aCA4GEAAKBgCFkOL2OH1kMnd9/N9ho
17cIbNVW0wnw3qUFH4v9JcDvqL/hZzuo3bfwd7nWFeJch3hYwfjMRp7Ily3T0Dwc
QtDdJXXcbcZfu8m4bvOqR9ZPcYGr1E3zYiFZTxtg1T1BG4asLdxRpfJ3WcfGKcSQ
VAT47dti6N34h1fAmA6sejH3
-----END PUBLIC KEY-----
)pem";
    bssl::UniquePtr<BIO> bio(BIO_new_mem_buf(kDsaPublicKey, -1));
    bssl::UniquePtr<EVP_PKEY> key(PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, key);
    ASSERT_EQ(1, X509_set_pubkey(cert, key.get()));
}

inline void set_rsa_pss_public_key(X509 *cert) {
    X509_PUBKEY *pub = X509_get_X509_PUBKEY(cert);
    const unsigned char *encoded = nullptr;
    int len = 0;
    ASSERT_EQ(1, X509_PUBKEY_get0_param(nullptr, &encoded, &len, nullptr, pub));
    auto *copy = static_cast<unsigned char *>(OPENSSL_memdup(encoded, len));
    ASSERT_NE(nullptr, copy);
    // Keep the RSA modulus/exponent but constrain the SPKI to RSA-PSS.
    ASSERT_EQ(1, X509_PUBKEY_set0_param(pub, OBJ_dup(OBJ_nid2obj(NID_rsassaPss)), V_ASN1_UNDEF, nullptr, copy, len));
}

} // namespace fiber::tls::certtest

#endif // FIBER_TLS_CERTIFICATE_TEST_SUPPORT_H
