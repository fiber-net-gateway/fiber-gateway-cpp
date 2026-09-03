#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "http/TlsAlpn.h"

namespace {

TEST(TlsAlpnTest, Http1ClientOwnsHttp11Alpn) {
    fiber::http::HttpClientTlsOptions options;
    options.server_name = "routing.example.test";
    options.verify_name = "certificate.example.test";

    auto param = fiber::http::make_http1_client_tls_param(options);

    const std::vector<std::string> expected = {"http/1.1"};
    EXPECT_EQ(param.alpn, expected);
    EXPECT_EQ(param.server_name, options.server_name);
    EXPECT_EQ(param.verify_name, options.verify_name);
}

TEST(TlsAlpnTest, Http2ClientOwnsH2Alpn) {
    fiber::http::HttpClientTlsOptions options;

    auto param = fiber::http::make_http2_client_tls_param(options);

    const std::vector<std::string> expected = {"h2"};
    EXPECT_EQ(param.alpn, expected);
}

TEST(TlsAlpnTest, NormalizeHttpServerAlpnPrefersH2ThenHttp11) {
    static constexpr std::string_view input[] = {"custom", "http/1.1", "h2", "", "custom"};
    fiber::net::TlsServerParam options;
    options.alpn = input;

    fiber::net::TlsAlpnList alpn;
    fiber::http::normalize_http_server_alpn(options, alpn);

    const std::vector<std::string_view> expected = {"h2", "http/1.1", "custom"};
    const std::vector<std::string_view> actual(options.alpn.begin(), options.alpn.end());
    EXPECT_EQ(actual, expected);
    EXPECT_EQ(options.alpn.data(), alpn.view().data());
}

TEST(TlsAlpnTest, NormalizeHttpServerAlpnAddsSupportedDefaultsWhenMissing) {
    static constexpr std::string_view input[] = {"acme/1"};
    fiber::net::TlsServerParam options;
    options.alpn = input;

    fiber::net::TlsAlpnList alpn;
    fiber::http::normalize_http_server_alpn(options, alpn);

    const std::vector<std::string_view> expected = {"h2", "http/1.1", "acme/1"};
    const std::vector<std::string_view> actual(options.alpn.begin(), options.alpn.end());
    EXPECT_EQ(actual, expected);
}

TEST(TlsAlpnTest, NormalizeHttp3ServerAlpnPrefersH3AndDropsTcpProtocols) {
    static constexpr std::string_view input[] = {"http/1.1", "h3", "custom", "h2", "", "custom"};
    fiber::net::TlsServerParam options;
    options.alpn = input;

    fiber::net::TlsAlpnList alpn;
    fiber::http::normalize_http3_alpn(options, alpn);

    const std::vector<std::string_view> expected = {"h3", "custom"};
    const std::vector<std::string_view> actual(options.alpn.begin(), options.alpn.end());
    EXPECT_EQ(actual, expected);
}

TEST(TlsAlpnTest, AlpnProtocolsViewContainsOfferedProtocols) {
    const std::uint8_t encoded[] = {
            0x00, 0x0c, 0x02, 'h', '2', 0x08, 'h', 't', 't', 'p', '/', '1', '.', '1',
    };

    fiber::net::TlsAlpnProtocolsView offered(encoded, sizeof(encoded));

    EXPECT_TRUE(offered.contains("h2"));
    EXPECT_TRUE(offered.contains("http/1.1"));
    EXPECT_FALSE(offered.contains("acme/1"));
}

} // namespace
