#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <fiber/common/IoError.h>
#include <fiber/tls/handshake/TlsExtensionCodec.h>
#include <fiber/tls/handshake/TlsHandshakeCodec.h>
#include <fiber/tls/handshake/TlsHandshakeMessage.h>

namespace {

using fiber::tls::kTlsHelloRetryRandom;
using fiber::tls::tls_client_hello_size;
using fiber::tls::tls_decode_certificate_12;
using fiber::tls::tls_decode_certificate_13;
using fiber::tls::tls_decode_certificate_request_12;
using fiber::tls::tls_decode_certificate_request_13;
using fiber::tls::tls_decode_certificate_verify;
using fiber::tls::tls_decode_client_hello;
using fiber::tls::tls_decode_client_key_exchange;
using fiber::tls::tls_decode_encrypted_extensions;
using fiber::tls::tls_decode_finished;
using fiber::tls::tls_decode_server_hello;
using fiber::tls::tls_decode_server_key_exchange;
using fiber::tls::tls_encode_certificate_12;
using fiber::tls::tls_encode_certificate_13;
using fiber::tls::tls_encode_certificate_request_12;
using fiber::tls::tls_encode_certificate_request_13;
using fiber::tls::tls_encode_certificate_verify;
using fiber::tls::tls_encode_client_hello;
using fiber::tls::tls_encode_client_key_exchange;
using fiber::tls::tls_encode_encrypted_extensions;
using fiber::tls::tls_encode_finished;
using fiber::tls::tls_encode_handshake_message;
using fiber::tls::tls_encode_new_session_ticket_12;
using fiber::tls::tls_encode_new_session_ticket_13;
using fiber::tls::tls_encode_server_hello;
using fiber::tls::tls_encode_server_key_exchange;
using fiber::tls::tls_is_hello_retry_request;
using fiber::tls::tls_psk_binder_at;
using fiber::tls::tls_psk_identity_at;
using fiber::tls::TlsCertificate12;
using fiber::tls::TlsCertificate13;
using fiber::tls::TlsCertificateRequest12;
using fiber::tls::TlsCertificateRequest13;
using fiber::tls::TlsCertificateVerify;
using fiber::tls::TlsClientHello;
using fiber::tls::TlsClientHelloEncoded;
using fiber::tls::TlsClientHelloInput;
using fiber::tls::TlsClientKeyExchange;
using fiber::tls::TlsEncryptedExtensions;
using fiber::tls::TlsEncryptedExtensionsInput;
using fiber::tls::TlsFinished;
using fiber::tls::TlsHandshakeType;
using fiber::tls::TlsNewSessionTicket13Input;
using fiber::tls::TlsPskIdentityView;
using fiber::tls::TlsServerHello;
using fiber::tls::TlsServerHelloInput;
using fiber::tls::TlsServerKeyExchange;
using fiber::tls::TlsServerKeyExchangeInput;

void put_u8(std::vector<std::uint8_t> &out, std::uint8_t value) { out.push_back(value); }

void put_u16(std::vector<std::uint8_t> &out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
}

void put_u24(std::vector<std::uint8_t> &out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

std::span<const std::uint8_t> as_bytes(std::initializer_list<std::uint8_t> values) {
    return {values.begin(), values.size()};
}

std::vector<std::uint8_t> concat(std::initializer_list<std::span<const std::uint8_t>> parts) {
    std::vector<std::uint8_t> out;
    for (const auto part: parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

std::vector<std::uint8_t> ext_bytes(std::uint16_t type, std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> out;
    put_u16(out, type);
    put_u16(out, static_cast<std::uint16_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

std::vector<std::uint8_t> ext(std::uint16_t type, std::initializer_list<std::uint8_t> data) {
    return ext_bytes(type, as_bytes(data));
}

// ---- ServerHello helpers ----

// SH-variant supported_versions: a bare 2-byte value (no list wrapper).
std::vector<std::uint8_t> sv_sh_ext(std::uint16_t version) {
    std::vector<std::uint8_t> data;
    put_u16(data, version);
    return ext_bytes(43, data);
}

// SH/HRR-variant key_share: a bare KeyShareEntry (no list wrapper — that is
// ClientHello-only, RFC 8446 §4.2.8); an empty key emits the HRR selected_group
// form — just the 2-byte group, no key length.
std::vector<std::uint8_t> server_key_share_ext(std::uint16_t group, std::span<const std::uint8_t> key) {
    std::vector<std::uint8_t> data;
    put_u16(data, group);
    if (!key.empty()) {
        put_u16(data, static_cast<std::uint16_t>(key.size()));
        data.insert(data.end(), key.begin(), key.end());
    }
    return ext_bytes(51, data);
}

std::vector<std::uint8_t> cookie_ext(std::span<const std::uint8_t> cookie) {
    std::vector<std::uint8_t> data;
    put_u16(data, static_cast<std::uint16_t>(cookie.size()));
    data.insert(data.end(), cookie.begin(), cookie.end());
    return ext_bytes(44, data);
}

std::vector<std::uint8_t> alpn_server_ext(std::string_view name) {
    std::vector<std::uint8_t> list;
    list.push_back(static_cast<std::uint8_t>(name.size()));
    list.insert(list.end(), name.begin(), name.end());
    std::vector<std::uint8_t> data;
    put_u16(data, static_cast<std::uint16_t>(list.size()));
    data.insert(data.end(), list.begin(), list.end());
    return ext_bytes(16, data);
}

std::vector<std::uint8_t> build_server_hello(std::span<const std::uint8_t> random,
                                             std::span<const std::uint8_t> session_id, std::uint16_t suite,
                                             std::span<const std::uint8_t> extensions_entries) {
    std::vector<std::uint8_t> body;
    put_u16(body, 0x0303);
    body.insert(body.end(), random.begin(), random.end());
    put_u8(body, static_cast<std::uint8_t>(session_id.size()));
    body.insert(body.end(), session_id.begin(), session_id.end());
    put_u16(body, suite);
    put_u8(body, 0); // compression null
    put_u16(body, static_cast<std::uint16_t>(extensions_entries.size()));
    body.insert(body.end(), extensions_entries.begin(), extensions_entries.end());
    return body;
}

const std::vector<std::uint8_t> kRandom32(32, 0x5A);
const std::vector<std::uint8_t> kSessionId{0x01, 0x02, 0x03, 0x04};
const std::vector<std::uint8_t> kServerShare{0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38};

std::vector<std::uint8_t> plain_sh_exts() {
    return concat({sv_sh_ext(0x0304), server_key_share_ext(0x001D, kServerShare), ext(23, {}), ext(0xFF01, {0x00}),
                   ext(4242, {0x99})}); // EMS, empty RI, unknown
}

std::vector<std::uint8_t> plain_sh_body() { return build_server_hello(kRandom32, kSessionId, 0x1301, plain_sh_exts()); }

// ---- certificate / request helpers ----

std::vector<std::uint8_t> cert_entry(std::span<const std::uint8_t> der, std::span<const std::uint8_t> entry_exts) {
    std::vector<std::uint8_t> out;
    put_u24(out, static_cast<std::uint32_t>(der.size()));
    out.insert(out.end(), der.begin(), der.end());
    put_u16(out, static_cast<std::uint16_t>(entry_exts.size()));
    out.insert(out.end(), entry_exts.begin(), entry_exts.end());
    return out;
}

std::vector<std::uint8_t> cert13_body(std::span<const std::uint8_t> ctx,
                                      std::initializer_list<std::span<const std::uint8_t>> entries) {
    std::vector<std::uint8_t> list;
    for (const auto entry: entries) {
        list.insert(list.end(), entry.begin(), entry.end());
    }
    std::vector<std::uint8_t> body;
    put_u8(body, static_cast<std::uint8_t>(ctx.size()));
    body.insert(body.end(), ctx.begin(), ctx.end());
    put_u24(body, static_cast<std::uint32_t>(list.size()));
    body.insert(body.end(), list.begin(), list.end());
    return body;
}

// `count` copies of one entry (1.3 empty-context form, or the bare 1.2 list
// when `tls13` is false) — the chain-count cap cases.
std::vector<std::uint8_t> cert_body_repeat(bool tls13, std::span<const std::uint8_t> entry, std::size_t count) {
    std::vector<std::uint8_t> list;
    for (std::size_t i = 0; i < count; ++i) {
        list.insert(list.end(), entry.begin(), entry.end());
    }
    std::vector<std::uint8_t> body;
    if (tls13) {
        put_u8(body, 0);
    }
    put_u24(body, static_cast<std::uint32_t>(list.size()));
    body.insert(body.end(), list.begin(), list.end());
    return body;
}

std::vector<std::uint8_t> cert12_entry(std::span<const std::uint8_t> der) {
    std::vector<std::uint8_t> out;
    put_u24(out, static_cast<std::uint32_t>(der.size()));
    out.insert(out.end(), der.begin(), der.end());
    return out;
}

std::vector<std::uint8_t> cert12_body(std::initializer_list<std::span<const std::uint8_t>> entries) {
    std::vector<std::uint8_t> list;
    for (const auto entry: entries) {
        list.insert(list.end(), entry.begin(), entry.end());
    }
    std::vector<std::uint8_t> body;
    put_u24(body, static_cast<std::uint32_t>(list.size()));
    body.insert(body.end(), list.begin(), list.end());
    return body;
}

std::vector<std::uint8_t> cr13_body(std::span<const std::uint8_t> ctx,
                                    std::span<const std::uint8_t> extensions_entries) {
    std::vector<std::uint8_t> body;
    put_u8(body, static_cast<std::uint8_t>(ctx.size()));
    body.insert(body.end(), ctx.begin(), ctx.end());
    put_u16(body, static_cast<std::uint16_t>(extensions_entries.size()));
    body.insert(body.end(), extensions_entries.begin(), extensions_entries.end());
    return body;
}

std::vector<std::uint8_t> sigalgs_ext(std::initializer_list<std::uint16_t> schemes) {
    std::vector<std::uint8_t> data;
    put_u16(data, static_cast<std::uint16_t>(schemes.size() * 2));
    for (const std::uint16_t scheme: schemes) {
        put_u16(data, scheme);
    }
    return ext_bytes(13, data);
}

// ---- decode: ServerHello ----

TEST(ServerHelloDecode, FullParseExtractsEveryField) {
    const std::vector<std::uint8_t> body = build_server_hello(
            kRandom32, kSessionId, 0x1301,
            concat({sv_sh_ext(0x0304), server_key_share_ext(0x001D, kServerShare), alpn_server_ext("h2"), ext(23, {}),
                    ext(0xFF01, {0x00}), ext(41, {0x00, 0x01})}));

    TlsServerHello hello;
    ASSERT_TRUE(tls_decode_server_hello(body.data(), body.size(), hello).has_value());
    EXPECT_EQ(hello.legacy_version, 0x0303);
    EXPECT_EQ(hello.random.data(), body.data() + 2); // spans borrow the body
    EXPECT_EQ(hello.session_id.size(), 4u);
    EXPECT_EQ(hello.cipher_suite, 0x1301);
    EXPECT_EQ(hello.compression_method, 0);
    EXPECT_TRUE(hello.has_supported_version);
    EXPECT_EQ(hello.supported_version, 0x0304);
    EXPECT_TRUE(hello.has_key_share);
    EXPECT_EQ(hello.key_share_group, 0x001Du);
    EXPECT_EQ(hello.key_share.size(), kServerShare.size());
    EXPECT_EQ(0, std::memcmp(hello.key_share.data(), kServerShare.data(), kServerShare.size()));
    EXPECT_TRUE(hello.has_alpn);
    EXPECT_EQ(hello.alpn, "h2");
    EXPECT_TRUE(hello.has_extended_master_secret);
    EXPECT_TRUE(hello.has_renegotiation_info);
    EXPECT_EQ(hello.renegotiation_info.size(), 1u); // content kept for the engine's empty-check
    EXPECT_TRUE(hello.has_selected_identity);
    EXPECT_EQ(hello.selected_identity, 1u);
    EXPECT_FALSE(hello.has_cookie);

    EXPECT_FALSE(tls_is_hello_retry_request(hello.random));
}

TEST(ServerHelloDecode, HelloRetryRandomIdentifiesHrr) {
    std::vector<std::uint8_t> hrr_random(kTlsHelloRetryRandom.begin(), kTlsHelloRetryRandom.end());
    EXPECT_TRUE(tls_is_hello_retry_request(hrr_random));

    hrr_random[31] ^= 0x01; // one-bit miss
    EXPECT_FALSE(tls_is_hello_retry_request(hrr_random));
    EXPECT_FALSE(tls_is_hello_retry_request({hrr_random.data(), 31}));
}

TEST(ServerHelloDecode, HrrFormKeyShareHasEmptyExchange) {
    const std::vector<std::uint8_t> body = build_server_hello(
            {kTlsHelloRetryRandom.begin(), kTlsHelloRetryRandom.end()}, kSessionId, 0x1301,
            concat({sv_sh_ext(0x0304), server_key_share_ext(0x0017, {}), cookie_ext(as_bytes({0xC1, 0xC2}))}));
    ASSERT_TRUE(tls_is_hello_retry_request({body.data() + 2, 32}));

    TlsServerHello hello;
    ASSERT_TRUE(tls_decode_server_hello(body.data(), body.size(), hello).has_value());
    EXPECT_TRUE(hello.has_key_share);
    EXPECT_EQ(hello.key_share_group, 0x0017u);
    EXPECT_TRUE(hello.key_share.empty()); // selected_group form
    EXPECT_TRUE(hello.has_cookie);
    EXPECT_EQ(hello.cookie.size(), 2u);
}

TEST(ServerHelloDecode, ExtensionlessSh12Decodes) {
    const std::vector<std::uint8_t> body = build_server_hello(kRandom32, {}, 0xC02F, {});
    TlsServerHello hello;
    ASSERT_TRUE(tls_decode_server_hello(body.data(), body.size(), hello).has_value());
    EXPECT_EQ(hello.legacy_version, 0x0303);
    EXPECT_TRUE(hello.session_id.empty());
    EXPECT_EQ(hello.cipher_suite, 0xC02Fu);
    EXPECT_FALSE(hello.has_supported_version);
    EXPECT_FALSE(hello.has_key_share);
    EXPECT_TRUE(hello.extensions_block.empty());
}

TEST(ServerHelloDecode, MalformedTable) {
    TlsServerHello out;
    const auto bad = [&](const std::vector<std::uint8_t> &body) {
        EXPECT_FALSE(tls_decode_server_hello(body.data(), body.size(), out).has_value());
    };

    const auto truncated_exts = plain_sh_body();
    bad(std::vector<std::uint8_t>(truncated_exts.begin(), truncated_exts.end() - 5)); // truncated mid-extension
    bad(build_server_hello({kRandom32.data(), 31}, kSessionId, 0x1301, plain_sh_exts())); // random 31 bytes
    {
        auto body = plain_sh_body();
        body[34] = 33; // session_id length 33 > 32 cap
        bad(body);
    }
    { // extension block length overruns the body
        auto body = plain_sh_body();
        const auto exts = plain_sh_exts();
        const std::size_t block_len_at = body.size() - exts.size() - 2;
        body[block_len_at] = static_cast<std::uint8_t>((exts.size() + 1) >> 8);
        body[block_len_at + 1] = static_cast<std::uint8_t>((exts.size() + 1) & 0xFF);
        bad(body);
    }
    bad(build_server_hello(kRandom32, kSessionId, 0x1301,
                           concat({sv_sh_ext(0x0304), sv_sh_ext(0x0303)}))); // duplicate extension
    bad(build_server_hello(kRandom32, kSessionId, 0x1301,
                           ext(43, {0x03, 0x04, 0x03}))); // supported_versions payload != 2
    bad(build_server_hello(kRandom32, kSessionId, 0x1301,
                           concat({server_key_share_ext(0x001D, kServerShare),
                                   server_key_share_ext(0x0017, kServerShare)}))); // duplicate key_share
    {
        // key_share entry length lies (claims one byte more than the data holds)
        std::vector<std::uint8_t> data;
        put_u16(data, 0x001D);
        put_u16(data, static_cast<std::uint16_t>(kServerShare.size() + 1));
        data.insert(data.end(), kServerShare.begin(), kServerShare.end());
        bad(build_server_hello(kRandom32, kSessionId, 0x1301, ext_bytes(51, data)));
    }
    bad(build_server_hello(kRandom32, kSessionId, 0x1301,
                           ext(44, {0x00, 0x00}))); // cookie: 2-byte len, non-empty required
    bad(build_server_hello(kRandom32, kSessionId, 0x1301, ext(41, {0x00, 0x01, 0x00}))); // pre_shared_key payload != 2
    bad(build_server_hello(kRandom32, kSessionId, 0x1301, ext(23, {0x01}))); // EMS must be empty
    bad(build_server_hello(kRandom32, kSessionId, 0x1301, alpn_server_ext(""))); // empty ALPN name
    {
        // two selected protocols
        std::vector<std::uint8_t> list;
        list.push_back(2);
        list.insert(list.end(), {'h', '2'});
        list.push_back(2);
        list.insert(list.end(), {'h', '3'});
        std::vector<std::uint8_t> data;
        put_u16(data, static_cast<std::uint16_t>(list.size()));
        data.insert(data.end(), list.begin(), list.end());
        bad(build_server_hello(kRandom32, kSessionId, 0x1301, ext_bytes(16, data)));
    }
    { // trailing bytes after the extension block
        auto body = plain_sh_body();
        body.push_back(0x00);
        bad(body);
    }
    { // failure leaves `out` untouched
        TlsServerHello sentinel;
        sentinel.cipher_suite = 0xBEEF;
        const auto whole = plain_sh_body();
        const auto truncated = std::vector<std::uint8_t>(whole.begin(), whole.begin() + 40);
        EXPECT_FALSE(tls_decode_server_hello(truncated.data(), truncated.size(), sentinel).has_value());
        EXPECT_EQ(sentinel.cipher_suite, 0xBEEFu);
    }
}

// ---- decode: EncryptedExtensions ----

std::vector<std::uint8_t> ee_body(std::span<const std::uint8_t> entries) {
    std::vector<std::uint8_t> body;
    put_u16(body, static_cast<std::uint16_t>(entries.size()));
    body.insert(body.end(), entries.begin(), entries.end());
    return body;
}

TEST(EncryptedExtensionsDecode, ExtractsAlpnAndEarlyData) {
    const auto body = ee_body(concat({alpn_server_ext("h3"), ext(42, {}), ext(4242, {0x01, 0x02})}));
    TlsEncryptedExtensions out;
    ASSERT_TRUE(tls_decode_encrypted_extensions(body.data(), body.size(), out).has_value());
    EXPECT_TRUE(out.has_alpn);
    EXPECT_EQ(out.alpn, "h3");
    EXPECT_TRUE(out.has_early_data);
    EXPECT_EQ(out.extensions_block.size(), body.size() - 2); // block spans the entries
}

TEST(EncryptedExtensionsDecode, MalformedTable) {
    TlsEncryptedExtensions out;
    const auto bad = [&](const std::vector<std::uint8_t> &body) {
        EXPECT_FALSE(tls_decode_encrypted_extensions(body.data(), body.size(), out).has_value());
    };

    bad({}); // missing block length
    {
        auto body = ee_body(concat({alpn_server_ext("h2"), ext(16, {0x00, 0x01, 'a'})})); // duplicate ALPN ext
        bad(body);
    }
    bad(ee_body(ext(42, {0x01}))); // early_data must be empty
    bad(ee_body(alpn_server_ext(""))); // empty selected name
    {
        auto body = ee_body(alpn_server_ext("h2"));
        body.push_back(0); // trailing byte
        bad(body);
    }
    { // block length overruns
        auto body = ee_body(alpn_server_ext("h2"));
        body[0] = 0x00;
        body[1] = 0xFF;
        bad(body);
    }
}

// ---- decode: Certificate (both forms) ----

const std::vector<std::uint8_t> kDerA{0x30, 0x82, 0x01, 0x01, 0xAA};
const std::vector<std::uint8_t> kDerB{0x30, 0x82, 0x02, 0x02, 0xBB, 0xCC};

TEST(CertificateDecode, Tls13FullParse) {
    const auto body =
            cert13_body({}, {cert_entry(kDerA, {}), cert_entry(kDerB, as_bytes({0x00, 0x04, 0x00, 0x02, 0x00, 0x00}))});
    TlsCertificate13 out;
    ASSERT_TRUE(tls_decode_certificate_13(body.data(), body.size(), out).has_value());
    EXPECT_TRUE(out.certificate_request_context.empty());
    EXPECT_EQ(out.cert_count, 2u);
    EXPECT_EQ(out.certs[0].size(), kDerA.size());
    EXPECT_EQ(0, std::memcmp(out.certs[0].data(), kDerA.data(), kDerA.size()));
    EXPECT_EQ(out.certs[1].size(), kDerB.size());
    EXPECT_EQ(out.certs[1].data(), body.data() + 1 + 3 + (3 + kDerA.size() + 2) + 3); // borrows body
}

TEST(CertificateDecode, Tls13EmptyListIsStructurallyValid) {
    const auto body = cert13_body({}, {});
    TlsCertificate13 out;
    ASSERT_TRUE(tls_decode_certificate_13(body.data(), body.size(), out).has_value());
    EXPECT_EQ(out.cert_count, 0u); // rejecting an empty server chain is the engine's rule
}

TEST(CertificateDecode, Tls12FullParse) {
    const auto body = cert12_body({cert12_entry(kDerA), cert12_entry(kDerB)});
    TlsCertificate12 out;
    ASSERT_TRUE(tls_decode_certificate_12(body.data(), body.size(), out).has_value());
    EXPECT_EQ(out.cert_count, 2u);
    EXPECT_EQ(out.certs[1].size(), kDerB.size());
}

TEST(CertificateDecode, MalformedTable) {
    {
        TlsCertificate13 out;
        const auto bad = [&](const std::vector<std::uint8_t> &body) {
            EXPECT_FALSE(tls_decode_certificate_13(body.data(), body.size(), out).has_value());
        };
        bad(cert13_body({}, {cert_entry({} /* zero-length DER */, {})}));
        {
            // list length overruns
            auto body = cert13_body({}, {cert_entry(kDerA, {})});
            body[1] = 0x0F;
            bad(body);
        }
        {
            // entry extension vector overruns
            auto entry = cert_entry(kDerA, {});
            entry.back() = 0x10;
            entry.push_back(0x00);
            bad(cert13_body({}, {entry}));
        }
        {
            // context length overruns
            auto body = cert13_body({}, {cert_entry(kDerA, {})});
            body[0] = 0x10;
            bad(body);
        }
        {
            // trailing bytes after the list
            auto body = cert13_body({}, {cert_entry(kDerA, {})});
            body.push_back(0x00);
            bad(body);
        }
        {
            // chain-count cap: kMaxEntries decodes, one more is rejected
            const auto entry = cert_entry(kDerA, {});
            const auto at_cap = cert_body_repeat(true, entry, TlsCertificate13::kMaxEntries);
            ASSERT_TRUE(tls_decode_certificate_13(at_cap.data(), at_cap.size(), out).has_value());
            EXPECT_EQ(out.cert_count, TlsCertificate13::kMaxEntries);
            bad(cert_body_repeat(true, entry, TlsCertificate13::kMaxEntries + 1));
        }
    }
    {
        TlsCertificate12 out;
        const std::vector<std::uint8_t> short_len{0x00, 0x00};
        EXPECT_FALSE(tls_decode_certificate_12(short_len.data(), short_len.size(), out)
                             .has_value()); // truncated list length
        auto body = cert12_body({cert12_entry(kDerB)});
        body.push_back(0x00); // trailing byte
        EXPECT_FALSE(tls_decode_certificate_12(body.data(), body.size(), out).has_value());

        // chain-count cap: kMaxEntries decodes, one more is rejected
        const auto entry = cert12_entry(kDerB);
        const auto at_cap = cert_body_repeat(false, entry, TlsCertificate12::kMaxEntries);
        ASSERT_TRUE(tls_decode_certificate_12(at_cap.data(), at_cap.size(), out).has_value());
        EXPECT_EQ(out.cert_count, TlsCertificate12::kMaxEntries);
        const auto over_cap = cert_body_repeat(false, entry, TlsCertificate12::kMaxEntries + 1);
        EXPECT_FALSE(tls_decode_certificate_12(over_cap.data(), over_cap.size(), out).has_value());
    }
}

// ---- decode: CertificateRequest (both forms) ----

TEST(CertificateRequestDecode, Tls13RequiresSignatureAlgorithms) {
    const auto body = cr13_body(as_bytes({0x61}), concat({sigalgs_ext({0x0403, 0x0804}), ext(4242, {})}));
    TlsCertificateRequest13 out;
    ASSERT_TRUE(tls_decode_certificate_request_13(body.data(), body.size(), out).has_value());
    EXPECT_EQ(out.certificate_request_context, "a");
    EXPECT_TRUE(out.has_signature_algorithms);
    EXPECT_EQ(out.signature_algorithms.size(), 4u); // raw u16 list
    EXPECT_EQ(out.signature_algorithms.data(), body.data() + 2 + 2 + 6); // borrows body
}

TEST(CertificateRequestDecode, Tls13WithoutSigalgsFails) {
    const auto body = cr13_body({}, ext(4242, {}));
    TlsCertificateRequest13 out;
    EXPECT_FALSE(tls_decode_certificate_request_13(body.data(), body.size(), out).has_value());
}

TEST(CertificateRequestDecode, Tls12PositionalVectors) {
    std::vector<std::uint8_t> body;
    put_u8(body, 1);
    put_u8(body, 0x40); // certificate_types: rsa_sign
    put_u16(body, 4);
    put_u16(body, 0x0403);
    put_u16(body, 0x0804); // signature_algorithms
    put_u16(body, 3);
    body.push_back(0x0A);
    body.push_back(0x0B);
    body.push_back(0x0C); // certificate_authorities blob

    TlsCertificateRequest12 out;
    ASSERT_TRUE(tls_decode_certificate_request_12(body.data(), body.size(), out).has_value());
    EXPECT_EQ(out.certificate_types.size(), 1u);
    EXPECT_EQ(out.certificate_types.front(), 0x40);
    EXPECT_TRUE(out.has_signature_algorithms);
    EXPECT_EQ(out.signature_algorithms.size(), 4u);
    EXPECT_EQ(out.certificate_authorities.size(), 3u);
}

TEST(CertificateRequestDecode, MalformedTable) {
    {
        TlsCertificateRequest13 out;
        EXPECT_FALSE(tls_decode_certificate_request_13(nullptr, 0, out).has_value()); // no context byte
        auto dup = cr13_body({}, concat({sigalgs_ext({0x0403}), sigalgs_ext({0x0804})}));
        EXPECT_FALSE(tls_decode_certificate_request_13(dup.data(), dup.size(), out).has_value());
        auto odd = cr13_body({}, ext(13, {0x00, 0x01, 0x04})); // odd-length scheme list
        EXPECT_FALSE(tls_decode_certificate_request_13(odd.data(), odd.size(), out).has_value());
    }
    {
        TlsCertificateRequest12 out;
        const std::vector<std::uint8_t> empty_types{0x00};
        EXPECT_FALSE(tls_decode_certificate_request_12(empty_types.data(), empty_types.size(), out)
                             .has_value()); // types list empty
        std::vector<std::uint8_t> trailing;
        put_u8(trailing, 1);
        put_u8(trailing, 0x40);
        put_u16(trailing, 2);
        put_u16(trailing, 0x0403);
        trailing.push_back(0x00); // trailing byte after positional vectors
        EXPECT_FALSE(tls_decode_certificate_request_12(trailing.data(), trailing.size(), out).has_value());
    }
}

// ---- decode: CertificateVerify / Finished / ServerKeyExchange ----

TEST(CertificateVerifyAndFinishedDecode, ParseAndMalformedTable) {
    {
        TlsCertificateVerify out;
        std::vector<std::uint8_t> body;
        put_u16(body, 0x0403);
        put_u16(body, 4);
        body.insert(body.end(), {0xDE, 0xAD, 0xBE, 0xEF});
        ASSERT_TRUE(tls_decode_certificate_verify(body.data(), body.size(), out).has_value());
        EXPECT_EQ(out.algorithm, 0x0403u);
        EXPECT_EQ(out.signature.size(), 4u);
        EXPECT_EQ(out.signature.data(), body.data() + 4);

        const auto bad = [&](std::vector<std::uint8_t> mangled) {
            EXPECT_FALSE(tls_decode_certificate_verify(mangled.data(), mangled.size(), out).has_value());
        };
        bad(std::vector<std::uint8_t>(body.begin(), body.end() - 1)); // truncated signature
        bad({0x04, 0x03, 0x00, 0x00}); // zero-length signature
        bad({0x04, 0x03, 0x00, 0x04, 0xDE, 0xAD, 0xBE, 0xEF, 0x00}); // trailing byte
        bad({0x04}); // truncated header
    }
    {
        TlsFinished out;
        const std::vector<std::uint8_t> vd(32, 0x77);
        ASSERT_TRUE(tls_decode_finished(vd.data(), vd.size(), out).has_value());
        EXPECT_EQ(out.verify_data.size(), 32u);
        const std::vector<std::uint8_t> empty;
        EXPECT_FALSE(tls_decode_finished(empty.data(), 0, out).has_value());
    }
}

std::vector<std::uint8_t> ske_body(std::uint8_t curve_type, std::uint16_t group, std::span<const std::uint8_t> point,
                                   std::uint16_t scheme, std::span<const std::uint8_t> sig) {
    std::vector<std::uint8_t> body;
    put_u8(body, curve_type);
    put_u16(body, group);
    put_u8(body, static_cast<std::uint8_t>(point.size()));
    body.insert(body.end(), point.begin(), point.end());
    put_u16(body, scheme);
    put_u16(body, static_cast<std::uint16_t>(sig.size()));
    body.insert(body.end(), sig.begin(), sig.end());
    return body;
}

TEST(ServerKeyExchangeDecode, NamedCurveOnly) {
    const std::vector<std::uint8_t> point(32, 0x42);
    const std::vector<std::uint8_t> sig(64, 0x43);
    const auto body = ske_body(3, 0x001D, point, 0x0804, sig);

    TlsServerKeyExchange out;
    ASSERT_TRUE(tls_decode_server_key_exchange(body.data(), body.size(), out).has_value());
    EXPECT_EQ(out.curve_type, 3u);
    EXPECT_EQ(out.named_group, 0x001Du);
    EXPECT_EQ(out.public_key.size(), 32u);
    EXPECT_EQ(out.algorithm, 0x0804u);
    EXPECT_EQ(out.signature.size(), 64u);
    EXPECT_EQ(out.public_key.data(), body.data() + 4); // borrows body

    const auto bad = [&](const std::vector<std::uint8_t> &mangled) {
        EXPECT_FALSE(tls_decode_server_key_exchange(mangled.data(), mangled.size(), out).has_value());
    };
    bad(ske_body(1 /* explicit_curve */, 0x001D, point, 0x0804, sig));
    bad(ske_body(3, 0x001D, {} /* empty point */, 0x0804, sig));
    bad(ske_body(3, 0x001D, point, 0x0804, {} /* empty signature */));
    bad(std::vector<std::uint8_t>(body.begin(), body.end() - 1)); // truncated signature
    {
        auto trailing = body;
        trailing.push_back(0x00);
        bad(trailing);
    }
    bad({0x03, 0x00}); // truncated after curve type
}

// ---- encode: ClientHello ----

const std::vector<std::uint16_t> kSuites{0x1301, 0x1302, 0xC02F};
const std::vector<std::uint16_t> kGroups{0x001D, 0x0017};
const std::vector<std::uint16_t> kSigalgs{0x0403, 0x0503, 0x0804};
const std::vector<std::uint8_t> kClientShare(32, 0x11);
const std::string_view kAlpn[] = {"h2", "http/1.1"};
const std::vector<std::uint8_t> kTicket{0x71, 0x72, 0x73};

TlsClientHelloInput full_input() {
    TlsClientHelloInput in;
    in.random = kRandom32;
    in.session_id = kSessionId;
    in.cipher_suites = kSuites;
    in.supported_groups = kGroups;
    in.signature_algorithms = kSigalgs;
    in.key_share_group = 0x001D;
    in.key_share = kClientShare;
    in.sni_host = "example.com";
    in.alpn = kAlpn;
    in.session_ticket = kTicket;
    in.has_psk = true;
    in.psk_identity = kTicket;
    in.psk_obfuscated_ticket_age = 0x11223344;
    in.psk_binder_len = 32;
    in.early_data = true;
    return in;
}

class ClientHelloEncodeTest : public ::testing::Test {
protected:
    std::vector<std::uint8_t> buffer_;

    [[nodiscard]] fiber::common::IoResult<TlsClientHelloEncoded> encode(const TlsClientHelloInput &in) {
        const auto size = tls_client_hello_size(in);
        if (!size.has_value()) {
            return std::unexpected(size.error());
        }
        buffer_.assign(size.value() + 8, 0xCC);
        const auto encoded = tls_encode_client_hello(in, {buffer_.data(), size.value()});
        if (encoded.has_value()) {
            for (std::size_t i = size.value(); i < buffer_.size(); ++i) {
                EXPECT_EQ(buffer_[i], 0xCC) << "overwrite past the promised length at " << i; // tail poison intact
            }
            buffer_.resize(size.value());
        }
        return encoded;
    }
};

TEST_F(ClientHelloEncodeTest, RoundTripsThroughTheDecoder) {
    const auto in = full_input();
    const auto encoded = encode(in);
    ASSERT_TRUE(encoded.has_value());

    TlsClientHello hello;
    ASSERT_TRUE(tls_decode_client_hello(buffer_.data() + 4, encoded.value().len - 4, hello).has_value());
    EXPECT_EQ(hello.legacy_version, 0x0303);
    EXPECT_EQ(hello.random.size(), 32u);
    EXPECT_EQ(hello.session_id.size(), 4u);
    EXPECT_EQ(hello.cipher_suites.size(), 6u);
    EXPECT_TRUE(hello.has_server_name);
    EXPECT_EQ(hello.server_name, "example.com");
    EXPECT_TRUE(hello.has_supported_groups);
    EXPECT_EQ(hello.supported_groups.size(), 4u);
    EXPECT_TRUE(hello.has_signature_algorithms);
    EXPECT_EQ(hello.signature_algorithms.size(), 6u);
    EXPECT_TRUE(hello.has_key_share);
    EXPECT_EQ(hello.key_share_entries.size(), 4 + kClientShare.size());
    fiber::tls::TlsKeyShareView share;
    ASSERT_TRUE(fiber::tls::tls_find_client_key_share(hello.key_share_entries, fiber::tls::TlsNamedGroup::X25519, share)
                        .has_value());
    EXPECT_EQ(share.key_exchange.size(), kClientShare.size());
    EXPECT_TRUE(hello.has_alpn);
    EXPECT_EQ(hello.alpn_list.size(), 12u); // "h2" (1+2) + "http/1.1" (1+8)
    EXPECT_TRUE(hello.has_session_ticket);
    EXPECT_EQ(hello.session_ticket.size(), 3u);
    EXPECT_TRUE(hello.has_extended_master_secret);
    EXPECT_TRUE(hello.has_renegotiation_info);
    EXPECT_TRUE(hello.has_psk_key_exchange_modes);
    EXPECT_TRUE(hello.has_early_data);
    EXPECT_TRUE(hello.has_pre_shared_key);
    EXPECT_EQ(hello.psk_identity_count, 1u);
    EXPECT_EQ(hello.psk_binder_count, 1u);

    // supported_versions: fixed [0x0304, 0x0303] offer
    EXPECT_TRUE(hello.has_supported_versions);
    EXPECT_EQ(hello.supported_versions.size(), 4u);
    EXPECT_EQ(hello.supported_versions[0], 0x03);
    EXPECT_EQ(hello.supported_versions[1], 0x04);
    EXPECT_EQ(hello.supported_versions[2], 0x03);
    EXPECT_EQ(hello.supported_versions[3], 0x03);
}

TEST_F(ClientHelloEncodeTest, BinderBlockIsZeroedAndAnchored) {
    const auto in = full_input();
    const auto encoded = encode(in);
    ASSERT_TRUE(encoded.has_value());
    const auto &e = encoded.value();

    // binders vector: be16(33) || 0x20 || 32 zero bytes at the very end
    EXPECT_EQ(e.len, buffer_.size());
    EXPECT_EQ(e.binder_block_offset, e.len - 2 - 1 - in.psk_binder_len);
    EXPECT_EQ(buffer_[e.binder_block_offset], 0x00);
    EXPECT_EQ(buffer_[e.binder_block_offset + 1], 33);
    EXPECT_EQ(buffer_[e.binder_block_offset + 2], in.psk_binder_len);
    for (std::size_t i = 0; i < in.psk_binder_len; ++i) {
        EXPECT_EQ(buffer_[e.binder_block_offset + 3 + i], 0) << "binder byte " << i;
    }

    // header: type 1 + be24(body len)
    EXPECT_EQ(buffer_[0], 1);
    EXPECT_EQ(buffer_[1], 0x00);
    EXPECT_EQ(static_cast<std::uint32_t>(buffer_[2]) << 8 | buffer_[3], e.len - 4);

    // the decoder's body-relative truncation anchor matches (message-relative - 4)
    TlsClientHello hello;
    ASSERT_TRUE(tls_decode_client_hello(buffer_.data() + 4, e.len - 4, hello).has_value());
    EXPECT_EQ(hello.psk_binder_block_offset + 4, e.binder_block_offset);
}

TEST_F(ClientHelloEncodeTest, MinimalInputOmitsOptionalExtensions) {
    TlsClientHelloInput in;
    in.random = kRandom32;
    in.cipher_suites = kSuites;
    in.supported_groups = kGroups;
    in.signature_algorithms = kSigalgs;
    in.key_share_group = 0x001D;
    in.key_share = kClientShare;

    const auto encoded = encode(in);
    ASSERT_TRUE(encoded.has_value());
    TlsClientHello hello;
    ASSERT_TRUE(tls_decode_client_hello(buffer_.data() + 4, encoded.value().len - 4, hello).has_value());
    EXPECT_FALSE(hello.has_server_name);
    EXPECT_FALSE(hello.has_alpn);
    EXPECT_FALSE(hello.has_session_ticket);
    EXPECT_FALSE(hello.has_pre_shared_key);
    EXPECT_FALSE(hello.has_early_data);
    EXPECT_TRUE(hello.has_key_share);
    EXPECT_TRUE(hello.has_extended_master_secret); // offered by default
    EXPECT_TRUE(hello.has_renegotiation_info);
}

TEST_F(ClientHelloEncodeTest, CookieEchoIsLengthPrefixed) {
    // CH2 after an HRR carries the cookie extension (RFC 8446 §4.2.2:
    // opaque cookie<1..2^16-1>, i.e. a 2-byte length prefix inside the
    // extension payload) between ALPN and supported_groups.
    static constexpr std::uint8_t kCookie[] = {0xde, 0xad, 0xbe, 0xef, 0x01};
    auto in = full_input();
    in.cookie = kCookie;

    const auto without = encode(full_input());
    const auto with = encode(in);
    ASSERT_TRUE(without.has_value());
    ASSERT_TRUE(with.has_value());
    EXPECT_EQ(with.value().len - without.value().len, 4u + 2u + sizeof(kCookie));

    // Extension materializes as 00 2C | be16(payload=2+5) | be16(len) | bytes
    // right before the supported_groups extension (00 0A).
    const std::array<std::uint8_t, 8> kWantHead{0x00, 0x2C, 0x00, 0x07, 0x00, 0x05, 0xde, 0xad};
    const auto at = std::search(buffer_.begin(), buffer_.end(), kWantHead.begin(), kWantHead.end());
    ASSERT_NE(at, buffer_.end());
    const auto after = at + static_cast<std::ptrdiff_t>(kWantHead.size());
    EXPECT_EQ(after[0], 0xbe);
    EXPECT_EQ(after[1], 0xef);
    EXPECT_EQ(after[2], 0x01); // cookie tail byte
    EXPECT_EQ(after[3], 0x00); // supported_groups ext type hi
    EXPECT_EQ(after[4], 0x0A); // supported_groups ext type lo
}

TEST_F(ClientHelloEncodeTest, RejectsContractViolations) {
    const auto base = [] {
        TlsClientHelloInput in;
        in.random = kRandom32;
        in.cipher_suites = kSuites;
        in.supported_groups = kGroups;
        in.signature_algorithms = kSigalgs;
        in.key_share = kClientShare;
        return in;
    };

    auto size_mismatch = base();
    size_mismatch.random = {kRandom32.data(), 31};
    EXPECT_FALSE(tls_client_hello_size(size_mismatch).has_value());

    auto no_suites = base();
    no_suites.cipher_suites = {};
    EXPECT_FALSE(tls_client_hello_size(no_suites).has_value());

    auto ed_without_psk = base();
    ed_without_psk.early_data = true;
    EXPECT_FALSE(tls_client_hello_size(ed_without_psk).has_value());

    auto bad_binder_len = base();
    bad_binder_len.has_psk = true;
    bad_binder_len.psk_binder_len = 24;
    EXPECT_FALSE(tls_client_hello_size(bad_binder_len).has_value());

    // scratch one byte short fails; exact size succeeds
    const auto size = tls_client_hello_size(base());
    ASSERT_TRUE(size.has_value());
    std::vector<std::uint8_t> scratch(size.value() - 1, 0);
    EXPECT_FALSE(tls_encode_client_hello(base(), scratch).has_value());
    scratch.push_back(0);
    EXPECT_TRUE(tls_encode_client_hello(base(), scratch).has_value());
}

// ---- encode: client flight messages ----

TEST(FlightEncode, CertificateBothFormsRoundTrip) {
    const std::vector<std::uint8_t> der_a(10, 0x61);
    const std::vector<std::uint8_t> der_b(20, 0x62);
    const std::span<const std::uint8_t> certs[] = {der_a, der_b};

    // 1.3 with an echoed context
    std::vector<std::uint8_t> scratch(64, 0);
    const auto len13 =
            tls_encode_certificate_13({reinterpret_cast<const std::uint8_t *>("\x07ctx"), 4}, certs, scratch);
    ASSERT_TRUE(len13.has_value());
    TlsCertificate13 cert13;
    ASSERT_TRUE(tls_decode_certificate_13(scratch.data() + 4, len13.value() - 4, cert13).has_value());
    EXPECT_EQ(cert13.certificate_request_context, std::string_view("\x07ctx", 4));
    EXPECT_EQ(cert13.cert_count, 2u);
    EXPECT_EQ(cert13.certs[0].size(), 10u);
    EXPECT_EQ(cert13.certs[1].size(), 20u);

    // 1.2 bare list
    const auto len12 = tls_encode_certificate_12(certs, scratch);
    ASSERT_TRUE(len12.has_value());
    TlsCertificate12 cert12;
    ASSERT_TRUE(tls_decode_certificate_12(scratch.data() + 4, len12.value() - 4, cert12).has_value());
    EXPECT_EQ(cert12.cert_count, 2u);
    EXPECT_EQ(cert12.certs[1].size(), 20u);

    // cap: kMaxEntries + 1 entries rejected
    const std::vector<std::span<const std::uint8_t>> many(TlsCertificate13::kMaxEntries + 1, der_a);
    EXPECT_FALSE(tls_encode_certificate_12(many, scratch).has_value());
    EXPECT_FALSE(tls_encode_certificate_13({}, many, scratch).has_value());

    // empty DER entry rejected
    const std::span<const std::uint8_t> with_empty[] = {der_a, {}};
    EXPECT_FALSE(tls_encode_certificate_12(with_empty, scratch).has_value());

    // scratch too small
    std::vector<std::uint8_t> tiny(4, 0);
    EXPECT_FALSE(tls_encode_certificate_12({certs}, tiny).has_value());
}

TEST(FlightEncode, CertificateVerifyAndFinishedRoundTrip) {
    const std::vector<std::uint8_t> sig{0x11, 0x22, 0x33};
    std::vector<std::uint8_t> scratch(64, 0);
    const auto cv_len = tls_encode_certificate_verify(0x0804, sig, scratch);
    ASSERT_TRUE(cv_len.has_value());
    TlsCertificateVerify cv;
    ASSERT_TRUE(tls_decode_certificate_verify(scratch.data() + 4, cv_len.value() - 4, cv).has_value());
    EXPECT_EQ(cv.algorithm, 0x0804u);
    EXPECT_EQ(cv.signature.size(), 3u);
    EXPECT_EQ(scratch[0], static_cast<std::uint8_t>(TlsHandshakeType::CertificateVerify));

    const std::vector<std::uint8_t> verify_data(32, 0x99);
    const auto fin_len = tls_encode_finished(verify_data, scratch);
    ASSERT_TRUE(fin_len.has_value());
    TlsFinished fin;
    ASSERT_TRUE(tls_decode_finished(scratch.data() + 4, fin_len.value() - 4, fin).has_value());
    EXPECT_EQ(fin.verify_data.size(), 32u);
    EXPECT_EQ(scratch[0], static_cast<std::uint8_t>(TlsHandshakeType::Finished));

    EXPECT_FALSE(tls_encode_certificate_verify(0x0804, {}, scratch).has_value());
    EXPECT_FALSE(tls_encode_finished({}, scratch).has_value());
}

TEST(FlightEncode, GenericMessageAndEndOfEarlyData) {
    std::vector<std::uint8_t> scratch(8, 0);
    const auto len = tls_encode_handshake_message(TlsHandshakeType::EndOfEarlyData, {}, scratch);
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(len.value(), 4u);
    EXPECT_EQ(scratch[0], static_cast<std::uint8_t>(TlsHandshakeType::EndOfEarlyData));
    EXPECT_EQ(scratch[1], 0);
    EXPECT_EQ(scratch[2], 0);
    EXPECT_EQ(scratch[3], 0);

    const std::vector<std::uint8_t> body{0xAA, 0xBB};
    const auto len2 = tls_encode_handshake_message(TlsHandshakeType::ClientKeyExchange, body, {scratch.data(), 3});
    EXPECT_FALSE(len2.has_value()); // scratch too small
    const auto len3 = tls_encode_handshake_message(TlsHandshakeType::ClientKeyExchange, body, scratch);
    ASSERT_TRUE(len3.has_value());
    EXPECT_EQ(len3.value(), 6u);
    EXPECT_EQ(scratch[4], 0xAA);
    EXPECT_EQ(scratch[5], 0xBB);
}

// RFC 4492 §5.7: body = u8(point_len) || point — no curve wrapper (the curve
// came in the server's SKE), message header type ClientKeyExchange.
TEST(FlightEncode, ClientKeyExchange12PointForms) {
    std::vector<std::uint8_t> scratch(300, 0);

    // X25519-shaped share: 32 bytes.
    std::vector<std::uint8_t> point(32);
    for (std::size_t i = 0; i < point.size(); ++i) {
        point[i] = static_cast<std::uint8_t>(i);
    }
    const auto len = tls_encode_client_key_exchange(point, scratch);
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(len.value(), 4u + 1u + 32u);
    EXPECT_EQ(scratch[0], static_cast<std::uint8_t>(TlsHandshakeType::ClientKeyExchange));
    EXPECT_EQ(scratch[3], 33); // be24 body length
    EXPECT_EQ(scratch[4], 32); // u8 point length
    EXPECT_EQ(0, std::memcmp(scratch.data() + 5, point.data(), point.size()));

    // P-256-shaped share: 65 uncompressed bytes.
    point.assign(65, 0xAB);
    const auto len65 = tls_encode_client_key_exchange(point, scratch);
    ASSERT_TRUE(len65.has_value());
    EXPECT_EQ(len65.value(), 4u + 1u + 65u);
    EXPECT_EQ(scratch[4], 65);

    // Contract violations: empty point, >255-byte point, tight scratch.
    EXPECT_FALSE(tls_encode_client_key_exchange({}, scratch).has_value());
    const std::vector<std::uint8_t> huge(256, 0x01);
    EXPECT_FALSE(tls_encode_client_key_exchange(huge, scratch).has_value());
    EXPECT_FALSE(tls_encode_client_key_exchange(point, {scratch.data(), 4u + 1u + 65u - 1}).has_value());
}

// ---- 07 server flight encoders ----

std::vector<std::uint16_t> extension_order(std::span<const std::uint8_t> block) {
    std::vector<std::uint16_t> order;
    fiber::tls::TlsExtensionCursor cursor(block);
    fiber::tls::TlsExtensionView view;
    while (true) {
        const auto has = cursor.next(view);
        if (!has.has_value() || !has.value()) {
            break;
        }
        order.push_back(view.type);
    }
    return order;
}

TEST(ServerFlightEncode, ServerHello13RoundTrip) {
    std::vector<std::uint8_t> scratch(512, 0);
    const std::vector<std::uint8_t> random(32, 0x5A);
    const std::vector<std::uint8_t> session_id(32, 0x33);
    const std::vector<std::uint8_t> share(32, 0x77);

    TlsServerHelloInput in{};
    in.random = random;
    in.session_id = session_id;
    in.cipher_suite = 0x1301;
    in.tls13 = true;
    in.key_share_group = 0x001D;
    in.key_share = share;
    in.selected_identity = true;
    in.identity = 0;

    const auto len = tls_encode_server_hello(in, scratch);
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(scratch[0], static_cast<std::uint8_t>(TlsHandshakeType::ServerHello));
    // legacy_version 0x0303 + compression null live at fixed offsets.
    EXPECT_EQ(scratch[4], 0x03);
    EXPECT_EQ(scratch[5], 0x03);
    EXPECT_EQ(scratch[38], static_cast<std::uint8_t>(session_id.size()));
    EXPECT_EQ(scratch[39 + session_id.size()], 0x13);
    EXPECT_EQ(scratch[40 + session_id.size()], 0x01);
    EXPECT_EQ(scratch[41 + session_id.size()], 0); // compression

    TlsServerHello sh{};
    ASSERT_TRUE(tls_decode_server_hello(scratch.data() + 4, len.value() - 4, sh).has_value());
    EXPECT_EQ(sh.legacy_version, 0x0303);
    EXPECT_EQ(0, std::memcmp(sh.random.data(), random.data(), 32));
    ASSERT_EQ(sh.session_id.size(), 32u);
    EXPECT_EQ(0, std::memcmp(sh.session_id.data(), session_id.data(), 32));
    EXPECT_EQ(sh.cipher_suite, 0x1301);
    EXPECT_EQ(sh.compression_method, 0);
    EXPECT_TRUE(sh.has_supported_version);
    EXPECT_EQ(sh.supported_version, 0x0304);
    EXPECT_TRUE(sh.has_key_share);
    EXPECT_EQ(sh.key_share_group, 0x001D);
    ASSERT_EQ(sh.key_share.size(), 32u);
    EXPECT_EQ(0, std::memcmp(sh.key_share.data(), share.data(), 32));
    EXPECT_TRUE(sh.has_selected_identity);
    EXPECT_EQ(sh.selected_identity, 0);
    // RFC 8446 §4.2.11: pre_shared_key must be the LAST extension.
    const auto order = extension_order(sh.extensions_block);
    EXPECT_EQ(order, (std::vector<std::uint16_t>{51, 43, 41}));

    // Without PSK: key_share -> supported_versions only.
    in.selected_identity = false;
    const auto len2 = tls_encode_server_hello(in, scratch);
    ASSERT_TRUE(len2.has_value());
    ASSERT_TRUE(tls_decode_server_hello(scratch.data() + 4, len2.value() - 4, sh).has_value());
    EXPECT_FALSE(sh.has_selected_identity);
    EXPECT_EQ(extension_order(sh.extensions_block), (std::vector<std::uint16_t>{51, 43}));
}

TEST(ServerFlightEncode, ServerHelloHrrForm) {
    std::vector<std::uint8_t> scratch(256, 0);
    // The material must outlive `in`: TlsServerHelloInput borrows spans.
    const std::vector<std::uint8_t> session_id(32, 0xEE);
    TlsServerHelloInput in{};
    in.random = std::span<const std::uint8_t>{kTlsHelloRetryRandom.data(), kTlsHelloRetryRandom.size()};
    in.session_id = session_id;
    in.cipher_suite = 0x1302;
    in.tls13 = true;
    in.key_share_group = 0x0017; // selected_group; empty key_share = HRR form

    const auto len = tls_encode_server_hello(in, scratch);
    ASSERT_TRUE(len.has_value());
    TlsServerHello sh{};
    ASSERT_TRUE(tls_decode_server_hello(scratch.data() + 4, len.value() - 4, sh).has_value());
    EXPECT_TRUE(tls_is_hello_retry_request(sh.random));
    EXPECT_TRUE(sh.has_supported_version);
    EXPECT_EQ(sh.supported_version, 0x0304);
    EXPECT_TRUE(sh.has_key_share);
    EXPECT_EQ(sh.key_share_group, 0x0017);
    EXPECT_TRUE(sh.key_share.empty()); // selected_group form: no key bytes
    EXPECT_EQ(extension_order(sh.extensions_block), (std::vector<std::uint16_t>{51, 43}));
}

TEST(ServerFlightEncode, ServerHello12RoundTrip) {
    std::vector<std::uint8_t> scratch(512, 0);
    const std::vector<std::uint8_t> random(32, 0x9C);
    const std::vector<std::uint8_t> session_id(16, 0x11);

    TlsServerHelloInput in{};
    in.random = random;
    in.session_id = session_id;
    in.cipher_suite = 0xC02F;
    in.tls13 = false;
    in.extended_master_secret = true;
    in.renegotiation_info = true;
    in.alpn = "h2";
    in.session_ticket = true;

    const auto len = tls_encode_server_hello(in, scratch);
    ASSERT_TRUE(len.has_value());
    TlsServerHello sh{};
    ASSERT_TRUE(tls_decode_server_hello(scratch.data() + 4, len.value() - 4, sh).has_value());
    EXPECT_EQ(sh.cipher_suite, 0xC02F);
    EXPECT_FALSE(sh.has_supported_version);
    EXPECT_FALSE(sh.has_key_share);
    EXPECT_TRUE(sh.has_extended_master_secret);
    EXPECT_TRUE(sh.has_renegotiation_info);
    ASSERT_EQ(sh.renegotiation_info.size(), 1u); // empty renegotiated_connection
    EXPECT_EQ(sh.renegotiation_info[0], 0);
    EXPECT_TRUE(sh.has_alpn);
    EXPECT_EQ(sh.alpn, "h2");
    // extension order per the BoringSSL kExtensions walk: ems, ri, alpn, ticket
    EXPECT_EQ(extension_order(sh.extensions_block), (std::vector<std::uint16_t>{23, 0xFF01, 16, 35}));

    // Minimal 1.2 SH: renegotiation_info only.
    in.extended_master_secret = false;
    in.alpn = {};
    in.session_ticket = false;
    const auto len2 = tls_encode_server_hello(in, scratch);
    ASSERT_TRUE(len2.has_value());
    ASSERT_TRUE(tls_decode_server_hello(scratch.data() + 4, len2.value() - 4, sh).has_value());
    EXPECT_EQ(extension_order(sh.extensions_block), (std::vector<std::uint16_t>{0xFF01}));
}

TEST(ServerFlightEncode, ServerHelloRejectsBadInput) {
    std::vector<std::uint8_t> scratch(512, 0);
    // The material must outlive `in`: TlsServerHelloInput borrows spans.
    const std::vector<std::uint8_t> random31(31, 0x01); // wrong size
    const std::vector<std::uint8_t> random32(32, 0x01);
    const std::vector<std::uint8_t> session_id33(33, 0x02); // > 32
    TlsServerHelloInput in{};
    in.random = random31;
    in.session_id = {};
    in.cipher_suite = 0x1301;
    in.tls13 = true;
    EXPECT_FALSE(tls_encode_server_hello(in, scratch).has_value());

    in.random = random32;
    in.session_id = session_id33;
    EXPECT_FALSE(tls_encode_server_hello(in, scratch).has_value());

    in.session_id = {};
    const auto len = tls_encode_server_hello(in, scratch);
    ASSERT_TRUE(len.has_value());
    EXPECT_FALSE(tls_encode_server_hello(in, {scratch.data(), len.value() - 1}).has_value()); // tight scratch
}

TEST(ServerFlightEncode, EncryptedExtensionsRoundTrip) {
    std::vector<std::uint8_t> scratch(256, 0);

    TlsEncryptedExtensionsInput in{};
    in.acknowledge_server_name = true;
    in.alpn = "h2";
    in.early_data = true;

    const auto len = tls_encode_encrypted_extensions(in, scratch);
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(scratch[0], static_cast<std::uint8_t>(TlsHandshakeType::EncryptedExtensions));

    TlsEncryptedExtensions ee{};
    ASSERT_TRUE(tls_decode_encrypted_extensions(scratch.data() + 4, len.value() - 4, ee).has_value());
    EXPECT_TRUE(ee.has_alpn);
    EXPECT_EQ(ee.alpn, "h2");
    EXPECT_TRUE(ee.has_early_data);
    // server_name ack: an EMPTY extension (RFC 8446 §4.2.1 — no ServerNameList;
    // BoringSSL rejects any payload), before alpn and early_data.
    ASSERT_GE(ee.extensions_block.size(), 4u);
    EXPECT_EQ(ee.extensions_block[0], 0x00); // type low byte (0)
    EXPECT_EQ(ee.extensions_block[1], 0x00);
    EXPECT_EQ(ee.extensions_block[2], 0x00); // payload length 0
    EXPECT_EQ(ee.extensions_block[3], 0x00);
    EXPECT_EQ(extension_order(ee.extensions_block), (std::vector<std::uint16_t>{0, 16, 42}));

    // A server_name ack WITH a payload (the RFC 6066 1.2 list form) fails
    // decode — malformed at 1.3.
    std::vector<std::uint8_t> bad = {0x08, 0x00, 0x00, 0x08, // header, body len 8
                                     0x00, 0x06, // extension block length 6
                                     0x00, 0x00, 0x00, 0x02, // server_name, payload len 2
                                     0x00, 0x00}; // the 1.2 empty ServerNameList
    TlsEncryptedExtensions bad_ee{};
    EXPECT_FALSE(tls_decode_encrypted_extensions(bad.data() + 4, bad.size() - 4, bad_ee).has_value());

    // Empty EE is valid: u16 zero extension block only.
    const auto bare = tls_encode_encrypted_extensions(TlsEncryptedExtensionsInput{}, scratch);
    ASSERT_TRUE(bare.has_value());
    EXPECT_EQ(bare.value(), 4u + 2u);
}

TEST(ServerFlightEncode, CertificateRequestRoundTrip) {
    std::vector<std::uint8_t> scratch(512, 0);
    const std::vector<std::uint16_t> schemes{0x0403, 0x0804, 0x0401};

    // 1.3: context length 0 + signature_algorithms-only extension block.
    const auto len13 = tls_encode_certificate_request_13(schemes, scratch);
    ASSERT_TRUE(len13.has_value());
    EXPECT_EQ(scratch[0], static_cast<std::uint8_t>(TlsHandshakeType::CertificateRequest));
    TlsCertificateRequest13 req13{};
    ASSERT_TRUE(tls_decode_certificate_request_13(scratch.data() + 4, len13.value() - 4, req13).has_value());
    EXPECT_TRUE(req13.certificate_request_context.empty());
    EXPECT_TRUE(req13.has_signature_algorithms);
    ASSERT_EQ(req13.signature_algorithms.size(), 6u);
    EXPECT_EQ(req13.signature_algorithms[0], 0x04);
    EXPECT_EQ(req13.signature_algorithms[1], 0x03);
    EXPECT_EQ(extension_order(req13.extensions_block), (std::vector<std::uint16_t>{13}));

    // 1.2: types{1, 64} + sigalgs vector + empty authorities (u16 length 0).
    const auto len12 = tls_encode_certificate_request_12(schemes, scratch);
    ASSERT_TRUE(len12.has_value());
    TlsCertificateRequest12 req12{};
    ASSERT_TRUE(tls_decode_certificate_request_12(scratch.data() + 4, len12.value() - 4, req12).has_value());
    ASSERT_EQ(req12.certificate_types.size(), 2u);
    EXPECT_EQ(req12.certificate_types[0], 1);
    EXPECT_EQ(req12.certificate_types[1], 64);
    EXPECT_TRUE(req12.has_signature_algorithms);
    ASSERT_EQ(req12.signature_algorithms.size(), 6u);
    EXPECT_TRUE(req12.certificate_authorities.empty());
    // The trailing empty vector is present on the wire: u16 0 length.
    EXPECT_EQ(len12.value(), 4u + 1u + 2u + 2u + 6u + 2u);

    // Contract violations.
    EXPECT_FALSE(tls_encode_certificate_request_13({}, scratch).has_value());
    EXPECT_FALSE(tls_encode_certificate_request_12({}, scratch).has_value());
}

TEST(ServerFlightEncode, ServerKeyExchangeRoundTrip) {
    std::vector<std::uint8_t> scratch(512, 0);
    const std::vector<std::uint8_t> point(65, 0x04);
    const std::vector<std::uint8_t> sig{0x01, 0x02, 0x03, 0x04};

    TlsServerKeyExchangeInput in{};
    in.named_group = 0x0017;
    in.public_key = point;
    in.scheme = 0x0403;
    in.signature = sig;

    const auto len = tls_encode_server_key_exchange(in, scratch);
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(scratch[0], static_cast<std::uint8_t>(TlsHandshakeType::ServerKeyExchange));

    TlsServerKeyExchange ske{};
    ASSERT_TRUE(tls_decode_server_key_exchange(scratch.data() + 4, len.value() - 4, ske).has_value());
    EXPECT_EQ(ske.curve_type, 3);
    EXPECT_EQ(ske.named_group, 0x0017);
    ASSERT_EQ(ske.public_key.size(), point.size());
    EXPECT_EQ(0, std::memcmp(ske.public_key.data(), point.data(), point.size()));
    EXPECT_EQ(ske.algorithm, 0x0403);
    EXPECT_EQ(ske.signature.size(), sig.size());
    EXPECT_EQ(0, std::memcmp(ske.signature.data(), sig.data(), sig.size()));

    in.public_key = {};
    EXPECT_FALSE(tls_encode_server_key_exchange(in, scratch).has_value());
    in.public_key = point;
    in.signature = {};
    EXPECT_FALSE(tls_encode_server_key_exchange(in, scratch).has_value());
}

void put_u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

TEST(ServerFlightEncode, NewSessionTicket13) {
    std::vector<std::uint8_t> scratch(512, 0);
    const std::vector<std::uint8_t> ticket{0xC0, 0xFF, 0xEE};

    TlsNewSessionTicket13Input in{};
    in.lifetime_s = 7200;
    in.ticket_age_add = 0x11223344;
    in.ticket_nonce = 0;
    in.ticket = ticket;
    in.max_early_data = 14336;

    const auto len = tls_encode_new_session_ticket_13(in, scratch);
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(scratch[0], static_cast<std::uint8_t>(TlsHandshakeType::NewSessionTicket));

    // Expected body assembled independently: lifetime, age_add, nonce
    // (u8 len 1 + 1 byte), ticket (u16 len), extensions{early_data u32}.
    std::vector<std::uint8_t> expected;
    put_u32(expected, 7200);
    put_u32(expected, 0x11223344);
    expected.push_back(1);
    expected.push_back(0);
    put_u16(expected, static_cast<std::uint16_t>(ticket.size()));
    expected.insert(expected.end(), ticket.begin(), ticket.end());
    put_u16(expected, 8); // extension block length
    put_u16(expected, 42); // early_data
    put_u16(expected, 4);
    put_u32(expected, 14336);

    ASSERT_EQ(len.value(), 4u + expected.size());
    EXPECT_EQ(0, std::memcmp(scratch.data() + 4, expected.data(), expected.size()));

    // max_early_data == 0 => empty extension block, no early_data entry.
    in.max_early_data = 0;
    const auto len2 = tls_encode_new_session_ticket_13(in, scratch);
    ASSERT_TRUE(len2.has_value());
    ASSERT_GE(len2.value(), 4u + 13u);
    const std::size_t body2 = len2.value() - 4;
    EXPECT_EQ(scratch[4 + body2 - 1], 0); // ext block len low byte 0
    EXPECT_EQ(scratch[4 + body2 - 2], 0);

    // Contract violations: empty ticket, oversized ticket (>0xFFFF).
    in.ticket = {};
    EXPECT_FALSE(tls_encode_new_session_ticket_13(in, scratch).has_value());
}

TEST(ServerFlightEncode, NewSessionTicket12) {
    std::vector<std::uint8_t> scratch(512, 0);
    const std::vector<std::uint8_t> ticket{0xAA, 0xBB, 0xCC, 0xDD, 0xEE};

    const auto len = tls_encode_new_session_ticket_12(86400, ticket, scratch);
    ASSERT_TRUE(len.has_value());

    std::vector<std::uint8_t> expected;
    put_u32(expected, 86400);
    put_u16(expected, static_cast<std::uint16_t>(ticket.size()));
    expected.insert(expected.end(), ticket.begin(), ticket.end());
    ASSERT_EQ(len.value(), 4u + expected.size());
    EXPECT_EQ(0, std::memcmp(scratch.data() + 4, expected.data(), expected.size()));

    EXPECT_FALSE(tls_encode_new_session_ticket_12(100, {}, scratch).has_value());
}

TEST(ClientFlightDecode, ClientKeyExchange12) {
    // Round-trip against our own encoder.
    std::vector<std::uint8_t> scratch(300, 0);
    const std::vector<std::uint8_t> point(65, 0x04);
    const auto len = tls_encode_client_key_exchange(point, scratch);
    ASSERT_TRUE(len.has_value());

    TlsClientKeyExchange cke{};
    ASSERT_TRUE(tls_decode_client_key_exchange(scratch.data() + 4, len.value() - 4, cke).has_value());
    ASSERT_EQ(cke.public_key.size(), point.size());
    EXPECT_EQ(0, std::memcmp(cke.public_key.data(), point.data(), point.size()));

    // Truncated body, zero-length point, trailing bytes all fail.
    EXPECT_FALSE(tls_decode_client_key_exchange(scratch.data() + 4, 0, cke).has_value());
    const std::uint8_t zero_len[] = {0x00};
    EXPECT_FALSE(tls_decode_client_key_exchange(zero_len, sizeof(zero_len), cke).has_value());
    const std::uint8_t trailing[] = {0x01, 0xAB, 0xCD};
    EXPECT_FALSE(tls_decode_client_key_exchange(trailing, sizeof(trailing), cke).has_value());
}

TEST(PskWalk, IdentityAndBinderAtIndex) {
    // identities: two entries — {0xA1} age 100, {0xB1, 0xB2} age 0xFFFFFFF0.
    std::vector<std::uint8_t> identities;
    put_u16(identities, 1);
    identities.push_back(0xA1);
    put_u32(identities, 100);
    put_u16(identities, 2);
    identities.push_back(0xB1);
    identities.push_back(0xB2);
    put_u32(identities, 0xFFFFFFF0);

    TlsPskIdentityView view{};
    const auto first = tls_psk_identity_at(identities, 0, view);
    ASSERT_TRUE(first.has_value());
    ASSERT_EQ(view.identity.size(), 1u);
    EXPECT_EQ(view.identity[0], 0xA1);
    EXPECT_EQ(view.obfuscated_ticket_age, 100u);

    const auto second = tls_psk_identity_at(identities, 1, view);
    ASSERT_TRUE(second.has_value());
    ASSERT_EQ(view.identity.size(), 2u);
    EXPECT_EQ(view.identity[0], 0xB1);
    EXPECT_EQ(view.obfuscated_ticket_age, 0xFFFFFFF0u);

    const auto third = tls_psk_identity_at(identities, 2, view);
    ASSERT_TRUE(third.has_value());
    EXPECT_FALSE(third.value()); // out of range: engaged false

    // Truncated age fails the walk.
    const std::uint8_t truncated[] = {0x00, 0x01, 0xA1, 0x00};
    EXPECT_FALSE(tls_psk_identity_at(truncated, 0, view).has_value());

    // binders: two entries — 32-byte and 48-byte (the two suite hash lengths).
    std::vector<std::uint8_t> binders;
    binders.push_back(32);
    binders.insert(binders.end(), 32, 0x11);
    binders.push_back(48);
    binders.insert(binders.end(), 48, 0x22);

    std::span<const std::uint8_t> binder{};
    const auto b0 = tls_psk_binder_at(binders, 0, binder);
    ASSERT_TRUE(b0.has_value());
    ASSERT_TRUE(b0.value());
    ASSERT_EQ(binder.size(), 32u);
    EXPECT_EQ(binder[0], 0x11);

    const auto b1 = tls_psk_binder_at(binders, 1, binder);
    ASSERT_TRUE(b1.has_value());
    ASSERT_TRUE(b1.value());
    ASSERT_EQ(binder.size(), 48u);
    EXPECT_EQ(binder[0], 0x22);

    const auto b2 = tls_psk_binder_at(binders, 2, binder);
    ASSERT_TRUE(b2.has_value());
    EXPECT_FALSE(b2.value());

    const std::uint8_t bad_binder[] = {5, 0x01};
    EXPECT_FALSE(tls_psk_binder_at(bad_binder, 0, binder).has_value());
}

} // namespace
