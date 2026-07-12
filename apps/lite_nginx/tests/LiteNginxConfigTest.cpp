#include <gtest/gtest.h>

#include <chrono>
#include <string_view>

#include "config/Config.h"
#include "config/ConfigLoader.h"
#include "config/Lexer.h"

namespace {

using fiber::lite_nginx::config::ConfigLoader;
using fiber::lite_nginx::config::Lexer;
using fiber::lite_nginx::config::PoolSteal;
using fiber::lite_nginx::config::ProxyPassKind;
using fiber::lite_nginx::config::TokenKind;

TEST(LiteNginxConfigTest, LexerHandlesCommentsAndQuotedStrings) {
    Lexer lexer(R"(
        # comment
        proxy_set_header Host "backend internal";
    )",
                "inline.conf");

    auto tokens_result = lexer.tokenize();
    ASSERT_TRUE(tokens_result.has_value()) << tokens_result.error().message;
    const auto &tokens = *tokens_result;

    ASSERT_GE(tokens.size(), 5u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Word);
    EXPECT_EQ(tokens[0].text, "proxy_set_header");
    EXPECT_EQ(tokens[1].text, "Host");
    EXPECT_EQ(tokens[2].kind, TokenKind::String);
    EXPECT_EQ(tokens[2].text, "backend internal");
    EXPECT_EQ(tokens[3].kind, TokenKind::Semicolon);
}

TEST(LiteNginxConfigTest, ParsesStructuredConfig) {
    auto config_result = ConfigLoader::load_from_string(R"(
        worker_processes 4;

        http {
            listen 8080;
            listen 8443 ssl http3;

            connection_pool {
                keepalive_size 32;
                keepalive_timeout 30s;
                steal auto;
                max_idle_total 2048;
                initial_group_capacity 32;
            }

            upstream backend {
                server 127.0.0.1:9001 weight=3;
                server https://baidu.com:443 weight=1;
                server http://127.0.0.1:9002;
                connect_timeout 2s;
            }

            server {
                server_name localhost api.local;
                certificate /tmp/localhost.crt;
                certificate_key /tmp/localhost.key;
                proxy_read_timeout 10s;
                proxy_set_header Host backend.internal;

                location /ready {
                    proxy_pass http://127.0.0.1:9009;
                    proxy_buffering off;
                }

                location /api/ {
                    proxy_pass http://backend;
                    proxy_set_header X-Forwarded-Proto http;
                }
            }
        }
    )",
                                                        "inline.conf");

    ASSERT_TRUE(config_result.has_value()) << config_result.error().message;
    const auto &config = *config_result;

    EXPECT_EQ(config.worker_processes, 4u);
    ASSERT_EQ(config.http.listens.size(), 2u);
    EXPECT_EQ(config.http.listens[0].port, 8080);
    EXPECT_FALSE(config.http.listens[0].tls);
    EXPECT_FALSE(config.http.listens[0].http3);
    EXPECT_EQ(config.http.listens[1].port, 8443);
    EXPECT_TRUE(config.http.listens[1].tls);
    EXPECT_TRUE(config.http.listens[1].http3);
    ASSERT_EQ(config.http.upstreams.size(), 1u);
    EXPECT_EQ(config.http.upstreams[0].name, "backend");
    ASSERT_EQ(config.http.upstreams[0].servers.size(), 3u);
    EXPECT_EQ(config.http.upstreams[0].servers[0].host, "127.0.0.1");
    EXPECT_EQ(config.http.upstreams[0].servers[0].port, 9001);
    EXPECT_EQ(config.http.upstreams[0].servers[0].weight, 3u);
    EXPECT_FALSE(config.http.upstreams[0].servers[0].tls);
    EXPECT_EQ(config.http.upstreams[0].servers[1].host, "baidu.com");
    EXPECT_EQ(config.http.upstreams[0].servers[1].port, 443u);
    EXPECT_EQ(config.http.upstreams[0].servers[1].weight, 1u);
    EXPECT_TRUE(config.http.upstreams[0].servers[1].tls);
    EXPECT_EQ(config.http.upstreams[0].servers[2].host, "127.0.0.1");
    EXPECT_EQ(config.http.upstreams[0].servers[2].port, 9002u);
    EXPECT_FALSE(config.http.upstreams[0].servers[2].tls);
    EXPECT_EQ(config.http.connection_pool.keepalive_size, 32u);
    EXPECT_EQ(config.http.connection_pool.keepalive_timeout, std::chrono::seconds(30));
    EXPECT_EQ(config.http.connection_pool.steal, PoolSteal::Auto);
    EXPECT_EQ(config.http.connection_pool.max_idle_total, 2048u);
    EXPECT_EQ(config.http.connection_pool.initial_group_capacity, 32u);
    EXPECT_EQ(config.http.upstreams[0].connect_timeout, std::chrono::seconds(2));

    ASSERT_EQ(config.http.servers.size(), 1u);
    const auto &server = config.http.servers[0];
    ASSERT_EQ(server.server_names.size(), 2u);
    EXPECT_EQ(server.server_names[0], "localhost");
    EXPECT_EQ(server.server_names[1], "api.local");
    EXPECT_EQ(server.certificate, "/tmp/localhost.crt");
    EXPECT_EQ(server.certificate_key, "/tmp/localhost.key");
    ASSERT_EQ(server.locations.size(), 2u);

    const auto &ready = server.locations[0];
    EXPECT_EQ(ready.pattern, "/ready");
    EXPECT_EQ(ready.proxy_pass.kind, ProxyPassKind::Direct);
    EXPECT_EQ(ready.proxy_pass.host, "127.0.0.1");
    EXPECT_EQ(ready.proxy_pass.port, 9009);

    const auto &api = server.locations[1];
    EXPECT_EQ(api.proxy_pass.kind, ProxyPassKind::NamedUpstream);
    EXPECT_EQ(api.proxy_pass.upstream_name, "backend");
    ASSERT_EQ(api.proxy.set_headers.size(), 2u);
    EXPECT_EQ(api.proxy.set_headers[0].name, "Host");
    EXPECT_EQ(api.proxy.set_headers[1].name, "X-Forwarded-Proto");
}

TEST(LiteNginxConfigTest, RejectsVariablesInV1) {
    auto config_result = ConfigLoader::load_from_string(R"(
        http {
            listen 8080;
            server {
                server_name localhost;
                location / {
                    proxy_pass http://backend;
                    proxy_set_header Host $host;
                }
            }
        }
    )",
                                                        "inline.conf");

    ASSERT_FALSE(config_result.has_value());
    EXPECT_NE(config_result.error().message.find("does not support variables"), std::string::npos);
}

TEST(LiteNginxConfigTest, ParsesConnectionPoolSteal) {
    auto load = [](std::string_view steal_value) {
        std::string conf = R"(
            http {
                listen 8080;
                connection_pool {
                    keepalive_size 8;
                    keepalive_timeout 10s;
                    steal )";
        conf += steal_value;
        conf += R"(;
                }
                server { server_name localhost; location / { proxy_pass http://127.0.0.1:9001; } }
            }
        )";
        return ConfigLoader::load_from_string(conf, "steal.conf");
    };

    {
        auto config = load("auto");
        ASSERT_TRUE(config.has_value()) << config.error().message;
        EXPECT_EQ(config->http.connection_pool.steal, PoolSteal::Auto);
    }
    {
        auto config = load("on");
        ASSERT_TRUE(config.has_value()) << config.error().message;
        EXPECT_EQ(config->http.connection_pool.steal, PoolSteal::On);
    }
    {
        auto config = load("off");
        ASSERT_TRUE(config.has_value()) << config.error().message;
        EXPECT_EQ(config->http.connection_pool.steal, PoolSteal::Off);
    }
    {
        auto config = load("maybe");
        EXPECT_FALSE(config.has_value());
        EXPECT_NE(config.error().message.find("steal"), std::string::npos);
    }
}

TEST(LiteNginxConfigTest, ParsesConnectionPoolSizingDirectives) {
    {
        auto config_result = ConfigLoader::load_from_string(R"(
            http {
                listen 8080;
                connection_pool {
                    keepalive_size 8;
                    keepalive_timeout 10s;
                    max_idle_total 512;
                    initial_group_capacity 64;
                }
                server { server_name localhost; location / { proxy_pass http://127.0.0.1:9001; } }
            }
        )",
                                                            "sizing.conf");
        ASSERT_TRUE(config_result.has_value()) << config_result.error().message;
        EXPECT_EQ(config_result->http.connection_pool.max_idle_total, 512u);
        EXPECT_EQ(config_result->http.connection_pool.initial_group_capacity, 64u);
    }
    {
        // Omitted => 0, the "derive / use built-in default" sentinel consumed by make_pool_options.
        auto config_result = ConfigLoader::load_from_string(R"(
            http {
                listen 8080;
                connection_pool {
                    keepalive_size 8;
                    keepalive_timeout 10s;
                }
                server { server_name localhost; location / { proxy_pass http://127.0.0.1:9001; } }
            }
        )",
                                                            "sizing_default.conf");
        ASSERT_TRUE(config_result.has_value()) << config_result.error().message;
        EXPECT_EQ(config_result->http.connection_pool.max_idle_total, 0u);
        EXPECT_EQ(config_result->http.connection_pool.initial_group_capacity, 0u);
    }
    {
        auto config_result = ConfigLoader::load_from_string(R"(
            http {
                listen 8080;
                connection_pool {
                    keepalive_size 8;
                    max_idle_total not-a-number;
                }
                server { server_name localhost; location / { proxy_pass http://127.0.0.1:9001; } }
            }
        )",
                                                            "sizing_bad.conf");
        ASSERT_FALSE(config_result.has_value());
        EXPECT_NE(config_result.error().message.find("max_idle_total"), std::string::npos);
    }
}

TEST(LiteNginxConfigTest, ParsesUpstreamServerSchemePrefix) {
    auto config_result = ConfigLoader::load_from_string(R"(
        http {
            listen 8080;
            upstream mixed {
                server 127.0.0.1:9001 weight=2;
                server https://example.com:443 weight=1;
                server http://127.0.0.1:9002;
            }
            server { server_name localhost; location / { proxy_pass http://mixed; } }
        }
    )",
                                                        "scheme.conf");

    ASSERT_TRUE(config_result.has_value()) << config_result.error().message;
    ASSERT_EQ(config_result->http.upstreams.size(), 1u);
    ASSERT_EQ(config_result->http.upstreams[0].servers.size(), 3u);

    EXPECT_FALSE(config_result->http.upstreams[0].servers[0].tls);
    EXPECT_EQ(config_result->http.upstreams[0].servers[0].host, "127.0.0.1");

    EXPECT_TRUE(config_result->http.upstreams[0].servers[1].tls);
    EXPECT_EQ(config_result->http.upstreams[0].servers[1].host, "example.com");
    EXPECT_EQ(config_result->http.upstreams[0].servers[1].port, 443u);

    EXPECT_FALSE(config_result->http.upstreams[0].servers[2].tls);
    EXPECT_EQ(config_result->http.upstreams[0].servers[2].host, "127.0.0.1");
    EXPECT_EQ(config_result->http.upstreams[0].servers[2].port, 9002u);
}

TEST(LiteNginxConfigTest, RejectsUnsupportedDirective) {
    auto config_result = ConfigLoader::load_from_string(R"(
        http {
            listen 8080;
            server {
                server_name localhost;
                rewrite ^/a/(.*)$ /b/$1 last;
                location / {
                    proxy_pass http://127.0.0.1:9001;
                }
            }
        }
    )",
                                                        "inline.conf");

    ASSERT_FALSE(config_result.has_value());
    EXPECT_NE(config_result.error().message.find("unsupported directive"), std::string::npos);
}

TEST(LiteNginxConfigTest, RejectsInvalidConnectionPoolSize) {
    auto config_result = ConfigLoader::load_from_string(R"(
        http {
            listen 8080;
            connection_pool {
                keepalive_size not-a-number;
            }
            upstream backend {
                server 127.0.0.1:9001;
            }
            server {
                server_name localhost;
                location / {
                    proxy_pass http://backend;
                }
            }
        }
    )",
                                                        "inline.conf");

    ASSERT_FALSE(config_result.has_value());
    EXPECT_NE(config_result.error().message.find("keepalive_size"), std::string::npos);
}

TEST(LiteNginxConfigTest, RejectsUnknownNamedUpstream) {
    auto config_result = ConfigLoader::load_from_string(R"(
        http {
            listen 8080;
            server {
                server_name localhost;
                location / {
                    proxy_pass http://missing_backend;
                }
            }
        }
    )",
                                                        "inline.conf");

    ASSERT_FALSE(config_result.has_value());
    EXPECT_NE(config_result.error().message.find("unknown upstream"), std::string::npos);
}

TEST(LiteNginxConfigTest, RejectsSslListenWithoutCertificates) {
    auto config_result = ConfigLoader::load_from_string(R"(
        http {
            listen 8443 ssl;
            server {
                server_name localhost;
                location / {
                    proxy_pass http://127.0.0.1:9001;
                }
            }
        }
    )",
                                                        "inline.conf");

    ASSERT_FALSE(config_result.has_value());
    EXPECT_NE(config_result.error().message.find("certificate and certificate_key"), std::string::npos);
}

TEST(LiteNginxConfigTest, RejectsHttp3ListenWithoutSsl) {
    auto config_result = ConfigLoader::load_from_string(R"(
        http {
            listen 8443 http3;
            server {
                server_name localhost;
                location / {
                    proxy_pass http://127.0.0.1:9001;
                }
            }
        }
    )",
                                                        "inline.conf");

    ASSERT_FALSE(config_result.has_value());
    EXPECT_NE(config_result.error().message.find("http3 requires ssl"), std::string::npos);
}

TEST(LiteNginxConfigTest, AcceptsQuicListenAlias) {
    auto config_result = ConfigLoader::load_from_string(R"(
        http {
            listen 8443 ssl quic;
            server {
                server_name localhost;
                certificate /tmp/localhost.crt;
                certificate_key /tmp/localhost.key;
                location / {
                    proxy_pass http://127.0.0.1:9001;
                }
            }
        }
    )",
                                                        "inline.conf");

    ASSERT_TRUE(config_result.has_value()) << config_result.error().message;
    ASSERT_EQ(config_result->http.listens.size(), 1u);
    EXPECT_TRUE(config_result->http.listens[0].tls);
    EXPECT_TRUE(config_result->http.listens[0].http3);
}

} // namespace
