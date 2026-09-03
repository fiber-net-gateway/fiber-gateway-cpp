#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include <fiber/net/TlsServerHandshakeConfig.h>
#include "http/TlsAlpn.h"

namespace {

TEST(TlsAlpnTest, Http1ClientOwnsHttp11Alpn) {
    fiber::http::HttpClientTlsOptions options;
    options.server_name = "routing.example.test";
    options.verify_name = "certificate.example.test";

    auto param = fiber::http::make_http1_client_tls_param(options);

    const std::vector<std::string_view> expected = {"http/1.1"};
    const std::vector<std::string_view> actual(param.alpn.begin(), param.alpn.end());
    EXPECT_EQ(actual, expected);
    EXPECT_EQ(param.server_name, options.server_name);
    EXPECT_EQ(param.verify_name, options.verify_name);
}

TEST(TlsAlpnTest, Http2ClientOwnsH2Alpn) {
    fiber::http::HttpClientTlsOptions options;

    auto param = fiber::http::make_http2_client_tls_param(options);

    const std::vector<std::string_view> expected = {"h2"};
    const std::vector<std::string_view> actual(param.alpn.begin(), param.alpn.end());
    EXPECT_EQ(actual, expected);
}

TEST(TlsAlpnTest, Http1ServerAlpnIsHttp11Only) {
    fiber::http::HttpServerTlsOptions options;

    auto param = fiber::http::make_http1_server_tls_param(options);

    const std::vector<std::string_view> expected = {"http/1.1"};
    const std::vector<std::string_view> actual(param.alpn.begin(), param.alpn.end());
    EXPECT_EQ(actual, expected);
}

TEST(TlsAlpnTest, HttpServerAlpnPrefersH2ThenHttp11) {
    fiber::http::HttpServerTlsOptions options;

    auto param = fiber::http::make_http_server_tls_param(options);

    const std::vector<std::string_view> expected = {"h2", "http/1.1"};
    const std::vector<std::string_view> actual(param.alpn.begin(), param.alpn.end());
    EXPECT_EQ(actual, expected);
}

TEST(TlsAlpnTest, Http3ServerAlpnIsH3Only) {
    fiber::http::HttpServerTlsOptions options;

    auto param = fiber::http::make_http3_server_tls_param(options);

    const std::vector<std::string_view> expected = {"h3"};
    const std::vector<std::string_view> actual(param.alpn.begin(), param.alpn.end());
    EXPECT_EQ(actual, expected);
}

TEST(TlsAlpnTest, ServerTlsParamCopiesPolicyFields) {
    fiber::http::HttpServerTlsOptions options;
    options.configure_callback = &fiber::net::configure_tls_with_credential;
    options.configure_ctx = reinterpret_cast<void *>(0x1);
    options.client_certificate_mode = fiber::net::TlsClientCertificateMode::Required;
    options.min_version = 0x0304;
    options.max_version = 0x0304;
    options.enable_early_data = true;

    auto param = fiber::http::make_http_server_tls_param(options);

    EXPECT_EQ(param.configure_callback, options.configure_callback);
    EXPECT_EQ(param.configure_ctx, options.configure_ctx);
    EXPECT_EQ(param.client_certificate_mode, options.client_certificate_mode);
    EXPECT_EQ(param.min_version, options.min_version);
    EXPECT_EQ(param.max_version, options.max_version);
    EXPECT_TRUE(param.enable_early_data);
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
