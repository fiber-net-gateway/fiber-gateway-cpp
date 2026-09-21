#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <fiber/common/IoError.h>
#include <fiber/tls/TlsVersion.h>
#include <fiber/tls/handshake/TlsExtensionCodec.h>
#include <fiber/tls/handshake/TlsHandshakeCodec.h>
#include <fiber/tls/handshake/TlsHandshakeMessage.h>

namespace {

using fiber::common::IoErr;
using fiber::tls::tls_decode_client_hello;
using fiber::tls::tls_decode_handshake_header;
using fiber::tls::tls_find_client_key_share;
using fiber::tls::tls_psk_modes_contains;
using fiber::tls::tls_version_list_contains;
using fiber::tls::TlsClientHello;
using fiber::tls::TlsHandshakeType;
using fiber::tls::TlsNamedGroup;

void put_u8(std::vector<std::uint8_t> &out, std::uint8_t value) { out.push_back(value); }

void put_u16(std::vector<std::uint8_t> &out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
}

// span view of a braced byte list; valid for the enclosing full expression.
std::span<const std::uint8_t> as_bytes(std::initializer_list<std::uint8_t> values) {
    return {values.begin(), values.size()};
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

std::vector<std::uint8_t> sni_ext(std::string_view host) {
    std::vector<std::uint8_t> name;
    name.push_back(0); // host_name
    put_u16(name, static_cast<std::uint16_t>(host.size()));
    name.insert(name.end(), host.begin(), host.end());
    std::vector<std::uint8_t> data;
    put_u16(data, static_cast<std::uint16_t>(name.size()));
    data.insert(data.end(), name.begin(), name.end());
    return ext_bytes(0, data);
}

std::vector<std::uint8_t> supported_versions_ext(std::initializer_list<std::uint16_t> versions) {
    std::vector<std::uint8_t> data;
    data.push_back(static_cast<std::uint8_t>(versions.size() * 2));
    for (const std::uint16_t version: versions) {
        put_u16(data, version);
    }
    return ext_bytes(43, data);
}

std::vector<std::uint8_t> u16_list_ext(std::uint16_t type, std::initializer_list<std::uint16_t> values) {
    std::vector<std::uint8_t> data;
    put_u16(data, static_cast<std::uint16_t>(values.size() * 2));
    for (const std::uint16_t value: values) {
        put_u16(data, value);
    }
    return ext_bytes(type, data);
}

std::vector<std::uint8_t>
key_share_ext(std::initializer_list<std::pair<std::uint16_t, std::vector<std::uint8_t>>> shares) {
    std::vector<std::uint8_t> entries;
    for (const auto &[group, key]: shares) {
        put_u16(entries, group);
        put_u16(entries, static_cast<std::uint16_t>(key.size()));
        entries.insert(entries.end(), key.begin(), key.end());
    }
    std::vector<std::uint8_t> data;
    put_u16(data, static_cast<std::uint16_t>(entries.size()));
    data.insert(data.end(), entries.begin(), entries.end());
    return ext_bytes(51, data);
}

std::vector<std::uint8_t> alpn_ext(std::initializer_list<std::string_view> names) {
    std::vector<std::uint8_t> list;
    for (const std::string_view name: names) {
        list.push_back(static_cast<std::uint8_t>(name.size()));
        list.insert(list.end(), name.begin(), name.end());
    }
    std::vector<std::uint8_t> data;
    put_u16(data, static_cast<std::uint16_t>(list.size()));
    data.insert(data.end(), list.begin(), list.end());
    return ext_bytes(16, data);
}

std::vector<std::uint8_t> psk_modes_ext(std::initializer_list<std::uint8_t> modes) {
    std::vector<std::uint8_t> data;
    data.push_back(static_cast<std::uint8_t>(modes.size()));
    data.insert(data.end(), modes.begin(), modes.end());
    return ext_bytes(45, data);
}

std::vector<std::uint8_t> psk_ext(std::initializer_list<std::pair<std::vector<std::uint8_t>, std::uint32_t>> identities,
                                  std::initializer_list<std::vector<std::uint8_t>> binders) {
    std::vector<std::uint8_t> identity_bytes;
    for (const auto &[identity, age]: identities) {
        put_u16(identity_bytes, static_cast<std::uint16_t>(identity.size()));
        identity_bytes.insert(identity_bytes.end(), identity.begin(), identity.end());
        put_u8(identity_bytes, static_cast<std::uint8_t>(age >> 24));
        put_u8(identity_bytes, static_cast<std::uint8_t>(age >> 16));
        put_u8(identity_bytes, static_cast<std::uint8_t>(age >> 8));
        put_u8(identity_bytes, static_cast<std::uint8_t>(age));
    }
    std::vector<std::uint8_t> binder_bytes;
    for (const std::vector<std::uint8_t> &binder: binders) {
        binder_bytes.push_back(static_cast<std::uint8_t>(binder.size()));
        binder_bytes.insert(binder_bytes.end(), binder.begin(), binder.end());
    }
    std::vector<std::uint8_t> data;
    put_u16(data, static_cast<std::uint16_t>(identity_bytes.size()));
    data.insert(data.end(), identity_bytes.begin(), identity_bytes.end());
    put_u16(data, static_cast<std::uint16_t>(binder_bytes.size()));
    data.insert(data.end(), binder_bytes.begin(), binder_bytes.end());
    return ext_bytes(41, data);
}

std::vector<std::uint8_t> concat(std::initializer_list<std::span<const std::uint8_t>> parts) {
    std::vector<std::uint8_t> out;
    for (const auto part: parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

// Assembles a ClientHello body (bytes after the 4-byte handshake header).
std::vector<std::uint8_t> build_client_hello(std::uint16_t legacy_version, std::span<const std::uint8_t> random,
                                             std::span<const std::uint8_t> session_id,
                                             std::span<const std::uint8_t> cipher_suites,
                                             std::span<const std::uint8_t> compression,
                                             std::span<const std::uint8_t> extensions_entries) {
    std::vector<std::uint8_t> body;
    put_u16(body, legacy_version);
    body.insert(body.end(), random.begin(), random.end());
    put_u8(body, static_cast<std::uint8_t>(session_id.size()));
    body.insert(body.end(), session_id.begin(), session_id.end());
    put_u16(body, static_cast<std::uint16_t>(cipher_suites.size()));
    body.insert(body.end(), cipher_suites.begin(), cipher_suites.end());
    put_u8(body, static_cast<std::uint8_t>(compression.size()));
    body.insert(body.end(), compression.begin(), compression.end());
    if (!extensions_entries.empty()) {
        put_u16(body, static_cast<std::uint16_t>(extensions_entries.size()));
        body.insert(body.end(), extensions_entries.begin(), extensions_entries.end());
    }
    return body;
}

constexpr std::uint8_t kCompressionNull = 0;

class TlsClientHelloDecodeTest : public ::testing::Test {
protected:
    std::vector<std::uint8_t> full_body_;
    std::vector<std::uint8_t> full_extensions_;

    void SetUp() override {
        const std::vector<std::uint8_t> random(32, 0xAB);
        const std::vector<std::uint8_t> session_id{0x01, 0x02, 0x03, 0x04};
        const std::vector<std::uint8_t> cipher_suites{0x13, 0x01, 0x13, 0x02, 0x13, 0x03, 0xC0, 0x2B};

        full_extensions_ = concat({sni_ext("example.com"), supported_versions_ext({0x0304, 0x0303}),
                                   u16_list_ext(10, {0x001D, 0x0017}), // supported_groups
                                   u16_list_ext(13, {0x0403, 0x0503, 0x0804}), // signature_algorithms
                                   u16_list_ext(50, {0x0403}), // signature_algorithms_cert
                                   key_share_ext({{0x001D, {0x21, 0x22, 0x23, 0x24}}}), alpn_ext({"h3", "h2"}),
                                   psk_modes_ext({1}), ext(1234, {0xEE, 0xFF}), // unknown
                                   ext(42, {}), // early_data
                                   ext(35, {0x77, 0x88}), // session_ticket
                                   ext(28, {0x40, 0x01}), // record_size_limit
                                   ext(22, {}), // encrypt_then_mac
                                   ext(23, {}), // extended_master_secret
                                   ext(0xFF01, {0x00}), // renegotiation_info
                                   psk_ext({{{0x0A, 0x0B, 0x0C, 0x0D}, 1000}}, {std::vector<std::uint8_t>(32, 0x33)})});

        full_body_ = build_client_hello(0x0303, random, session_id, cipher_suites, as_bytes({kCompressionNull}),
                                        full_extensions_);
    }

    [[nodiscard]] fiber::common::IoResult<void> decode_full(TlsClientHello &out) const {
        return tls_decode_client_hello(full_body_.data(), full_body_.size(), out);
    }
};

TEST_F(TlsClientHelloDecodeTest, FullParseExtractsEveryField) {
    TlsClientHello hello;
    ASSERT_TRUE(decode_full(hello).has_value());

    EXPECT_EQ(hello.legacy_version, 0x0303);
    EXPECT_EQ(hello.random.size(), 32u);
    EXPECT_EQ(hello.session_id.size(), 4u);
    EXPECT_EQ(hello.cipher_suites.size(), 8u);
    EXPECT_EQ(hello.compression_methods.size(), 1u);
    EXPECT_EQ(hello.compression_methods.front(), kCompressionNull);

    EXPECT_TRUE(hello.has_server_name);
    EXPECT_EQ(hello.server_name, "example.com");
    EXPECT_TRUE(hello.has_supported_versions);
    EXPECT_EQ(hello.supported_versions.size(), 4u);
    EXPECT_TRUE(hello.has_supported_groups);
    EXPECT_EQ(hello.supported_groups.size(), 4u);
    EXPECT_TRUE(hello.has_signature_algorithms);
    EXPECT_EQ(hello.signature_algorithms.size(), 6u);
    EXPECT_TRUE(hello.has_signature_algorithms_cert);
    EXPECT_EQ(hello.signature_algorithms_cert.size(), 2u);
    EXPECT_TRUE(hello.has_key_share);
    EXPECT_TRUE(hello.has_alpn);
    EXPECT_TRUE(hello.has_psk_key_exchange_modes);
    EXPECT_TRUE(hello.has_early_data);
    EXPECT_TRUE(hello.has_session_ticket);
    EXPECT_EQ(hello.session_ticket.size(), 2u);
    EXPECT_TRUE(hello.has_record_size_limit);
    EXPECT_EQ(hello.record_size_limit, 0x4001);
    EXPECT_TRUE(hello.has_encrypt_then_mac);
    EXPECT_TRUE(hello.has_extended_master_secret);
    EXPECT_TRUE(hello.has_renegotiation_info);

    EXPECT_TRUE(hello.has_pre_shared_key);
    EXPECT_EQ(hello.psk_identity_count, 1u);
    EXPECT_EQ(hello.psk_binder_count, 1u);
    EXPECT_EQ(hello.psk_identities.size(), 10u); // u16 len + 4 identity + u32 age
    EXPECT_EQ(hello.psk_binders.size(), 33u); // u8 len + 32 bytes

    EXPECT_TRUE(tls_version_list_contains(hello.supported_versions, fiber::tls::kTlsVersionTls13));
    EXPECT_TRUE(tls_version_list_contains(hello.supported_versions, fiber::tls::kTlsVersionTls12));
    EXPECT_FALSE(tls_version_list_contains(hello.supported_versions, 0x0302));
}

TEST_F(TlsClientHelloDecodeTest, SpansBorrowTheBodyBuffer) {
    TlsClientHello hello;
    ASSERT_TRUE(decode_full(hello).has_value());

    EXPECT_EQ(hello.random.data(), full_body_.data() + 2);
    EXPECT_EQ(hello.session_id.data(), full_body_.data() + 35);
    EXPECT_EQ(hello.cipher_suites.data(), hello.session_id.data() + hello.session_id.size() + 2);
    EXPECT_EQ(hello.extensions_block.data(), full_body_.data() + full_body_.size() - full_extensions_.size());
    // ext hdr(4) + list len(2) + name type(1) + name len(2) into the first ext.
    EXPECT_EQ(hello.server_name.data(),
              reinterpret_cast<const char *>(full_body_.data() + full_body_.size() - full_extensions_.size() + 9));
}

TEST_F(TlsClientHelloDecodeTest, MinimalTls12HelloWithoutExtensions) {
    const std::vector<std::uint8_t> random(32, 0xCD);
    const auto body = build_client_hello(0x0303, random, {}, as_bytes({0xC0, 0x2F}), as_bytes({kCompressionNull}), {});

    TlsClientHello hello;
    ASSERT_TRUE(tls_decode_client_hello(body.data(), body.size(), hello).has_value());
    EXPECT_EQ(hello.legacy_version, 0x0303);
    EXPECT_EQ(hello.session_id.size(), 0u);
    EXPECT_EQ(hello.cipher_suites.size(), 2u);
    EXPECT_EQ(hello.extensions_block.size(), 0u);
    EXPECT_FALSE(hello.has_server_name || hello.has_supported_versions || hello.has_key_share ||
                 hello.has_pre_shared_key);
}

TEST(TlsClientHelloDecode, OutIsUntouchedOnFailure) {
    const std::vector<std::uint8_t> bad{0x03, 0x03}; // random truncated
    TlsClientHello hello;
    hello.legacy_version = 0x1234;
    EXPECT_EQ(tls_decode_client_hello(bad.data(), bad.size(), hello).error(), IoErr::Invalid);
    EXPECT_EQ(hello.legacy_version, 0x1234);
}

TEST_F(TlsClientHelloDecodeTest, EveryTruncationIsRejected) {
    // Body prefix length up to the extension-block length field (exclusive)
    // yields no-extension messages; only that exact boundary may decode.
    const std::size_t no_extension_boundary = full_body_.size() - full_extensions_.size() - 2;
    TlsClientHello hello;
    for (std::size_t len = 0; len < full_body_.size(); ++len) {
        const auto decoded = tls_decode_client_hello(full_body_.data(), len, hello);
        if (len == no_extension_boundary) {
            EXPECT_TRUE(decoded.has_value()) << "prefix of " << len << " should decode extension-less";
        } else {
            EXPECT_FALSE(decoded.has_value()) << "truncated to " << len << " bytes decoded successfully";
        }
    }
    ASSERT_TRUE(decode_full(hello).has_value()); // sanity: full size passes
}

TEST_F(TlsClientHelloDecodeTest, TrailingBytesAfterExtensionBlockRejected) {
    std::vector<std::uint8_t> body = full_body_;
    // Shrink the extension block claim by one byte, leaving a trailing byte.
    const std::size_t block_len_pos = full_body_.size() - full_extensions_.size() - 2;
    body[block_len_pos + 1] -= 1;
    TlsClientHello hello;
    EXPECT_EQ(tls_decode_client_hello(body.data(), body.size(), hello).error(), IoErr::Invalid);
}

TEST_F(TlsClientHelloDecodeTest, SessionIdOver32BytesRejected) {
    const std::vector<std::uint8_t> random(32, 0x00);
    const std::vector<std::uint8_t> long_id(33, 0x11);
    const auto body =
            build_client_hello(0x0303, random, long_id, as_bytes({0x13, 0x01}), as_bytes({kCompressionNull}), {});
    TlsClientHello hello;
    EXPECT_EQ(tls_decode_client_hello(body.data(), body.size(), hello).error(), IoErr::Invalid);
}

TEST_F(TlsClientHelloDecodeTest, SessionIdOf32BytesAccepted) {
    const std::vector<std::uint8_t> random(32, 0x00);
    const std::vector<std::uint8_t> session_id(32, 0x11);
    const auto body =
            build_client_hello(0x0303, random, session_id, as_bytes({0x13, 0x01}), as_bytes({kCompressionNull}), {});
    TlsClientHello hello;
    ASSERT_TRUE(tls_decode_client_hello(body.data(), body.size(), hello).has_value());
    EXPECT_EQ(hello.session_id.size(), 32u);
    EXPECT_EQ(hello.session_id.data(), body.data() + 35);
}

TEST_F(TlsClientHelloDecodeTest, MalformedCipherSuiteListRejected) {
    const std::vector<std::uint8_t> random(32, 0x00);
    TlsClientHello hello;
    for (const std::vector<std::uint8_t> &cipher_suites:
         std::vector<std::vector<std::uint8_t>>{{}, {0x13}, {0x13, 0x01, 0x13}}) {
        const auto body = build_client_hello(0x0303, random, {}, cipher_suites, as_bytes({kCompressionNull}), {});
        EXPECT_EQ(tls_decode_client_hello(body.data(), body.size(), hello).error(), IoErr::Invalid)
                << "cipher list size " << cipher_suites.size();
    }
}

TEST_F(TlsClientHelloDecodeTest, EmptyCompressionListRejected) {
    const std::vector<std::uint8_t> random(32, 0x00);
    const auto body = build_client_hello(0x0303, random, {}, as_bytes({0x13, 0x01}), {}, {});
    TlsClientHello hello;
    EXPECT_EQ(tls_decode_client_hello(body.data(), body.size(), hello).error(), IoErr::Invalid);
}

TEST_F(TlsClientHelloDecodeTest, DuplicateExtensionRejected) {
    const auto unknown = ext(4321, {0x01});
    std::vector<std::uint8_t> twice = unknown;
    twice.insert(twice.end(), unknown.begin(), unknown.end());

    const std::vector<std::uint8_t> random(32, 0x00);
    const auto body =
            build_client_hello(0x0303, random, {}, as_bytes({0x13, 0x01}), as_bytes({kCompressionNull}), twice);
    TlsClientHello hello;
    EXPECT_EQ(tls_decode_client_hello(body.data(), body.size(), hello).error(), IoErr::Invalid);
}

TEST_F(TlsClientHelloDecodeTest, ExtensionCapEnforced) {
    std::vector<std::uint8_t> extensions;
    for (std::uint16_t i = 0; i < 65; ++i) {
        const auto unknown = ext(static_cast<std::uint16_t>(60000 + i), {});
        extensions.insert(extensions.end(), unknown.begin(), unknown.end());
    }
    const std::vector<std::uint8_t> random(32, 0x00);
    const auto body =
            build_client_hello(0x0303, random, {}, as_bytes({0x13, 0x01}), as_bytes({kCompressionNull}), extensions);
    TlsClientHello hello;
    EXPECT_EQ(tls_decode_client_hello(body.data(), body.size(), hello).error(), IoErr::Invalid);
}

TEST_F(TlsClientHelloDecodeTest, EmptyPayloadExtensionCarriesPayloadRejected) {
    const std::vector<std::uint8_t> random(32, 0x00);
    for (const std::uint16_t type: {42u, 22u, 23u}) {
        const auto extensions = ext(type, {0x00});
        const auto body = build_client_hello(0x0303, random, {}, as_bytes({0x13, 0x01}), as_bytes({kCompressionNull}),
                                             extensions);
        TlsClientHello hello;
        EXPECT_EQ(tls_decode_client_hello(body.data(), body.size(), hello).error(), IoErr::Invalid) << "type " << type;
    }
}

TEST_F(TlsClientHelloDecodeTest, RecordSizeLimitMustBeTwoBytes) {
    const std::vector<std::uint8_t> random(32, 0x00);
    TlsClientHello hello;
    for (const std::vector<std::uint8_t> &payload: std::vector<std::vector<std::uint8_t>>{{0x40}, {0x40, 0x01, 0x00}}) {
        const auto extensions = ext_bytes(28, payload);
        const auto body = build_client_hello(0x0303, random, {}, as_bytes({0x13, 0x01}), as_bytes({kCompressionNull}),
                                             extensions);
        EXPECT_EQ(tls_decode_client_hello(body.data(), body.size(), hello).error(), IoErr::Invalid)
                << "payload size " << payload.size();
    }
}

TEST_F(TlsClientHelloDecodeTest, ServerNameListWalk) {
    const std::vector<std::uint8_t> random(32, 0x00);

    // Unknown name type first, then host_name: the host_name is extracted.
    std::vector<std::uint8_t> entries;
    entries.push_back(1); // unknown name type
    put_u16(entries, 3);
    entries.push_back('x');
    entries.push_back('y');
    entries.push_back('z');
    entries.push_back(0); // host_name
    put_u16(entries, 4);
    entries.push_back('h');
    entries.push_back('o');
    entries.push_back('s');
    entries.push_back('t');
    std::vector<std::uint8_t> data;
    put_u16(data, static_cast<std::uint16_t>(entries.size()));
    data.insert(data.end(), entries.begin(), entries.end());

    const auto extensions = ext_bytes(0, data);
    const auto body =
            build_client_hello(0x0303, random, {}, as_bytes({0x13, 0x01}), as_bytes({kCompressionNull}), extensions);
    TlsClientHello hello;
    ASSERT_TRUE(tls_decode_client_hello(body.data(), body.size(), hello).has_value());
    EXPECT_TRUE(hello.has_server_name);
    EXPECT_EQ(hello.server_name, "host");

    // Inflated list_len: rejected.
    data[1] = static_cast<std::uint8_t>(entries.size() + 1);
    const auto bad = ext_bytes(0, data);
    const auto bad_body =
            build_client_hello(0x0303, random, {}, as_bytes({0x13, 0x01}), as_bytes({kCompressionNull}), bad);
    EXPECT_EQ(tls_decode_client_hello(bad_body.data(), bad_body.size(), hello).error(), IoErr::Invalid);
}

TEST_F(TlsClientHelloDecodeTest, SupportedVersionsLengthMismatchRejected) {
    const std::vector<std::uint8_t> random(32, 0x00);
    TlsClientHello hello;

    // Claimed list_len disagrees with the payload.
    auto bad = supported_versions_ext({0x0304, 0x0303});
    bad[4] = 3;
    auto body = build_client_hello(0x0303, random, {}, as_bytes({0x13, 0x01}), as_bytes({kCompressionNull}), bad);
    EXPECT_EQ(tls_decode_client_hello(body.data(), body.size(), hello).error(), IoErr::Invalid);

    // Empty list.
    const auto empty = ext(43, {0x00});
    body = build_client_hello(0x0303, random, {}, as_bytes({0x13, 0x01}), as_bytes({kCompressionNull}), empty);
    EXPECT_EQ(tls_decode_client_hello(body.data(), body.size(), hello).error(), IoErr::Invalid);
}

TEST_F(TlsClientHelloDecodeTest, SupportedGroupsAndSigAlgsOddLengthRejected) {
    const std::vector<std::uint8_t> random(32, 0x00);
    TlsClientHello hello;
    for (const std::uint16_t type: {10u, 13u, 50u}) {
        // Claimed list_len covers only 3 bytes: odd and mismatched.
        const auto bad = ext(type, {0x00, 0x03, 0x00, 0x1D, 0x17});
        const auto body =
                build_client_hello(0x0303, random, {}, as_bytes({0x13, 0x01}), as_bytes({kCompressionNull}), bad);
        EXPECT_EQ(tls_decode_client_hello(body.data(), body.size(), hello).error(), IoErr::Invalid) << "type " << type;
    }
}

TEST_F(TlsClientHelloDecodeTest, KeyShareLookupHelpers) {
    TlsClientHello hello;
    ASSERT_TRUE(decode_full(hello).has_value());

    fiber::tls::TlsKeyShareView share;
    const auto found = tls_find_client_key_share(hello.key_share_entries, TlsNamedGroup::X25519, share);
    ASSERT_TRUE(found.has_value());
    EXPECT_TRUE(found.value());
    EXPECT_EQ(share.group, TlsNamedGroup::X25519);
    EXPECT_EQ(std::vector<std::uint8_t>(share.key_exchange.begin(), share.key_exchange.end()),
              (std::vector<std::uint8_t>{0x21, 0x22, 0x23, 0x24}));

    const auto missing = tls_find_client_key_share(hello.key_share_entries, TlsNamedGroup::Secp256r1, share);
    ASSERT_TRUE(missing.has_value());
    EXPECT_FALSE(missing.value());

    // Zero-length key_exchange rejected during the structural walk.
    const std::vector<std::uint8_t> random(32, 0x00);
    const auto empty_share = key_share_ext({{0x001D, {}}});
    const auto body =
            build_client_hello(0x0303, random, {}, as_bytes({0x13, 0x01}), as_bytes({kCompressionNull}), empty_share);
    EXPECT_EQ(tls_decode_client_hello(body.data(), body.size(), hello).error(), IoErr::Invalid);
}

TEST_F(TlsClientHelloDecodeTest, AlpnCursorWalksDecodedList) {
    TlsClientHello hello;
    ASSERT_TRUE(decode_full(hello).has_value());

    fiber::tls::TlsAlpnCursor cursor(hello.alpn_list);
    std::string_view name;
    ASSERT_TRUE(cursor.next(name).has_value());
    EXPECT_EQ(name, "h3");
    ASSERT_TRUE(cursor.next(name).has_value());
    EXPECT_EQ(name, "h2");
    const auto done = cursor.next(name);
    ASSERT_TRUE(done.has_value());
    EXPECT_FALSE(done.value());

    // Overlong protocol name prefix rejected: ext bytes are
    // type(2) len(2) list_len(2) name_len(1) "h2".
    const std::vector<std::uint8_t> random(32, 0x00);
    auto bad = alpn_ext({"h2"});
    bad[6] = 0x10;
    const auto body = build_client_hello(0x0303, random, {}, as_bytes({0x13, 0x01}), as_bytes({kCompressionNull}), bad);
    EXPECT_EQ(tls_decode_client_hello(body.data(), body.size(), hello).error(), IoErr::Invalid);
}

TEST_F(TlsClientHelloDecodeTest, PskModesContainsAndShape) {
    TlsClientHello hello;
    ASSERT_TRUE(decode_full(hello).has_value());
    EXPECT_TRUE(tls_psk_modes_contains(hello.psk_key_exchange_modes, fiber::tls::kTlsPskModePskDheKe));
    EXPECT_FALSE(tls_psk_modes_contains(hello.psk_key_exchange_modes, fiber::tls::kTlsPskModePskKe));

    // Mode list length byte must cover the rest of the payload.
    const std::vector<std::uint8_t> random(32, 0x00);
    const auto bad = ext(45, {0x01, 0x01, 0x00});
    const auto body = build_client_hello(0x0303, random, {}, as_bytes({0x13, 0x01}), as_bytes({kCompressionNull}), bad);
    EXPECT_EQ(tls_decode_client_hello(body.data(), body.size(), hello).error(), IoErr::Invalid);
}

TEST_F(TlsClientHelloDecodeTest, PskBinderBlockOffsetAnchorsTranscriptTruncation) {
    // Deterministic layout: no session id, one cipher, one compression method,
    // a single pre_shared_key extension. The binders vector length prefix sits
    // at ext_data + 2 + identities.size().
    const std::vector<std::uint8_t> random(32, 0x00);
    const auto extensions = psk_ext({{{0x0A, 0x0B, 0x0C, 0x0D}, 1000}}, {std::vector<std::uint8_t>(32, 0x33)});
    const auto body =
            build_client_hello(0x0303, random, {}, as_bytes({0x13, 0x01}), as_bytes({kCompressionNull}), extensions);

    TlsClientHello hello;
    ASSERT_TRUE(tls_decode_client_hello(body.data(), body.size(), hello).has_value());
    ASSERT_TRUE(hello.has_pre_shared_key);

    const std::size_t ext_block = body.size() - extensions.size();
    const std::size_t ext_data = ext_block + 4;
    const std::size_t expected = ext_data + 2 + hello.psk_identities.size();
    ASSERT_EQ(hello.psk_identities.size(), 10u);
    EXPECT_EQ(hello.psk_binder_block_offset, expected);
    EXPECT_EQ(hello.psk_binder_block_offset, ext_data + 12);
    // The anchored bytes are the binders vector length prefix (1 + 32 = 33).
    EXPECT_EQ(body[expected], 0x00);
    EXPECT_EQ(body[expected + 1], 0x21);
    // Truncated transcript ends at the prefix; the message ends after binders.
    EXPECT_EQ(hello.psk_binder_block_offset + 2 + hello.psk_binders.size(), body.size());
}

TEST_F(TlsClientHelloDecodeTest, PskMustBeTheLastExtension) {
    const std::vector<std::uint8_t> random(32, 0x00);
    const auto psk = psk_ext({{{0x0A}, 0}}, {std::vector<std::uint8_t>(32, 0x33)});
    const auto padding = ext(21, {0x00});
    const auto entries = concat({psk, padding});
    const auto body =
            build_client_hello(0x0303, random, {}, as_bytes({0x13, 0x01}), as_bytes({kCompressionNull}), entries);
    TlsClientHello hello;
    EXPECT_EQ(tls_decode_client_hello(body.data(), body.size(), hello).error(), IoErr::Invalid);
}

TEST_F(TlsClientHelloDecodeTest, PskIdentityBinderCountMismatchRejected) {
    const std::vector<std::uint8_t> random(32, 0x00);
    const auto mismatched = psk_ext({{{0x0A}, 0}, {{0x0B}, 0}}, {std::vector<std::uint8_t>(32, 0x33)});
    const auto body =
            build_client_hello(0x0303, random, {}, as_bytes({0x13, 0x01}), as_bytes({kCompressionNull}), mismatched);
    TlsClientHello hello;
    EXPECT_EQ(tls_decode_client_hello(body.data(), body.size(), hello).error(), IoErr::Invalid);
}

TEST_F(TlsClientHelloDecodeTest, PskBindersMustEndTheMessage) {
    const std::vector<std::uint8_t> random(32, 0x00);
    const auto psk = psk_ext({{{0x0A}, 0}}, {std::vector<std::uint8_t>(32, 0x33)});
    // Append a stray byte inside the extension, past the binders vector, and
    // widen the extension length to cover it.
    std::vector<std::uint8_t> patched = psk;
    patched.push_back(0x00);
    patched[2] = static_cast<std::uint8_t>((patched.size() - 4) >> 8);
    patched[3] = static_cast<std::uint8_t>((patched.size() - 4) & 0xff);

    const auto body =
            build_client_hello(0x0303, random, {}, as_bytes({0x13, 0x01}), as_bytes({kCompressionNull}), patched);
    TlsClientHello hello;
    EXPECT_EQ(tls_decode_client_hello(body.data(), body.size(), hello).error(), IoErr::Invalid);
}

TEST(TlsHandshakeHeaderDecode, DecodesTypeAndLength) {
    const std::uint8_t header[] = {0x01, 0x00, 0x01, 0x02};
    const auto decoded = tls_decode_handshake_header(header, sizeof(header));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->type, TlsHandshakeType::ClientHello);
    EXPECT_EQ(decoded->length, 0x0102u);

    const std::uint8_t truncated[] = {0x01, 0x00, 0x01};
    EXPECT_EQ(tls_decode_handshake_header(truncated, sizeof(truncated)).error(), IoErr::Invalid);
}

} // namespace
