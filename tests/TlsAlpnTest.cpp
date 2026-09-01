#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "http/TlsAlpn.h"

namespace {

TEST(TlsAlpnTest, NormalizeHttp1AlpnPrefersHttp11AndDropsH2) {
    fiber::net::TlsClientConnectionOptions options;
    options.alpn = {"h2", "", "acme/1", "http/1.1", "custom"};

    fiber::http::normalize_http1_alpn(options);

    const std::vector<std::string> expected = {"http/1.1", "acme/1", "custom"};
    EXPECT_EQ(options.alpn, expected);
}

TEST(TlsAlpnTest, NormalizeHttp1AlpnAddsHttp11WhenMissing) {
    fiber::net::TlsClientConnectionOptions options;
    options.alpn.clear();

    fiber::http::normalize_http1_alpn(options);

    const std::vector<std::string> expected = {"http/1.1"};
    EXPECT_EQ(options.alpn, expected);
}

TEST(TlsAlpnTest, NormalizeHttpServerAlpnPrefersH2ThenHttp11) {
    fiber::net::TlsServerConnectionOptions options;
    options.alpn = {"custom", "http/1.1", "h2", "", "custom"};

    fiber::http::normalize_http_server_alpn(options);

    const std::vector<std::string> expected = {"h2", "http/1.1", "custom"};
    EXPECT_EQ(options.alpn, expected);
}

TEST(TlsAlpnTest, NormalizeHttpServerAlpnAddsSupportedDefaultsWhenMissing) {
    fiber::net::TlsServerConnectionOptions options;
    options.alpn = {"acme/1"};

    fiber::http::normalize_http_server_alpn(options);

    const std::vector<std::string> expected = {"h2", "http/1.1", "acme/1"};
    EXPECT_EQ(options.alpn, expected);
}

TEST(TlsAlpnTest, NormalizeHttp3AlpnPrefersH3AndDropsTcpProtocols) {
    fiber::net::TlsClientConnectionOptions options;
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
