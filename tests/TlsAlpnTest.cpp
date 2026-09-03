#include <gtest/gtest.h>

#include <string>
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
    fiber::net::TlsServerParam options;
    options.alpn = {"custom", "http/1.1", "h2", "", "custom"};

    fiber::http::normalize_http_server_alpn(options);

    const std::vector<std::string> expected = {"h2", "http/1.1", "custom"};
    EXPECT_EQ(options.alpn, expected);
}

TEST(TlsAlpnTest, NormalizeHttpServerAlpnAddsSupportedDefaultsWhenMissing) {
    fiber::net::TlsServerParam options;
    options.alpn = {"acme/1"};

    fiber::http::normalize_http_server_alpn(options);

    const std::vector<std::string> expected = {"h2", "http/1.1", "acme/1"};
    EXPECT_EQ(options.alpn, expected);
}

TEST(TlsAlpnTest, NormalizeHttp3ServerAlpnPrefersH3AndDropsTcpProtocols) {
    fiber::net::TlsServerParam options;
    options.alpn = {"http/1.1", "h3", "custom", "h2", "", "custom"};

    fiber::http::normalize_http3_alpn(options);

    const std::vector<std::string> expected = {"h3", "custom"};
    EXPECT_EQ(options.alpn, expected);
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
