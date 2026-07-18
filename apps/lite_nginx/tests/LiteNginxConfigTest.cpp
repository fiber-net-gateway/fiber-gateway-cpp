#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#include "config/Config.h"
#include "config/ConfigLoader.h"
#include "config/Lexer.h"

namespace fs = std::filesystem;

namespace {

using fiber::lite_nginx::config::AccessLogKind;
using fiber::lite_nginx::config::ConfigLoader;
using fiber::lite_nginx::config::Lexer;
using fiber::lite_nginx::config::LogAppenderKind;
using fiber::lite_nginx::config::LoggingLevel;
using fiber::lite_nginx::config::PoolSteal;
using fiber::lite_nginx::config::ProxyPassKind;
using fiber::lite_nginx::config::TokenKind;

// RAII temp directory under the system temp dir; removed on destruction.
struct TempDir {
    fs::path path;
    explicit TempDir(std::string name) {
        path = fs::temp_directory_path() / std::move(name);
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;
};

// Saves the cwd on construction and restores it on destruction (best-effort), so a test
// that chdir's into a TempDir cannot leave subsequent tests in a deleted directory.
struct ScopedCwd {
    fs::path saved;
    ScopedCwd() : saved(fs::current_path()) {}
    ~ScopedCwd() {
        std::error_code ec;
        fs::current_path(saved, ec);
    }
    ScopedCwd(const ScopedCwd &) = delete;
    ScopedCwd &operator=(const ScopedCwd &) = delete;
};

void write_file(const fs::path &p, std::string_view content) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << content;
    f.close();
    ASSERT_TRUE(f.good()) << "failed to write " << p;
}

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

TEST(LiteNginxConfigTest, RejectsVariableInHeaderName) {
    // The header NAME must be static; a $ in the name is rejected.
    auto config_result = ConfigLoader::load_from_string(R"(
        http {
            listen 8080;
            server {
                server_name localhost;
                location / {
                    proxy_pass http://127.0.0.1:9001;
                    proxy_set_header $host value;
                }
            }
        }
    )",
                                                        "inline.conf");

    ASSERT_FALSE(config_result.has_value());
    EXPECT_NE(config_result.error().message.find("name must not contain variables"), std::string::npos);
}

TEST(LiteNginxConfigTest, BareDollarValueIsLiteral) {
    // A bare $ without ${ is a literal value (no interpolation), not a template.
    auto config_result = ConfigLoader::load_from_string(R"(
        http {
            listen 8080;
            server {
                server_name localhost;
                location / {
                    proxy_pass http://127.0.0.1:9001;
                    proxy_set_header Host $host;
                }
            }
        }
    )",
                                                        "inline.conf");

    ASSERT_TRUE(config_result.has_value()) << config_result.error().message;
    const auto &loc = config_result->http.servers[0].locations[0];
    ASSERT_EQ(loc.proxy.set_headers.size(), 1u);
    EXPECT_EQ(loc.proxy.set_headers[0].name, "Host");
    EXPECT_EQ(loc.proxy.set_headers[0].value, "$host");
    EXPECT_FALSE(loc.proxy.set_headers[0].is_template);
}

TEST(LiteNginxConfigTest, TemplateHeaderValue) {
    // A value containing ${...} is parsed as a template (compiled at runtime-build, evaluated
    // per request).
    auto config_result = ConfigLoader::load_from_string(R"(
        http {
            listen 8080;
            server {
                server_name localhost;
                location / {
                    proxy_pass http://127.0.0.1:9001;
                    proxy_set_header X-Original-Host "${$header.host}";
                }
            }
        }
    )",
                                                        "inline.conf");

    ASSERT_TRUE(config_result.has_value()) << config_result.error().message;
    const auto &loc = config_result->http.servers[0].locations[0];
    ASSERT_EQ(loc.proxy.set_headers.size(), 1u);
    EXPECT_EQ(loc.proxy.set_headers[0].name, "X-Original-Host");
    EXPECT_EQ(loc.proxy.set_headers[0].value, "${$header.host}");
    EXPECT_TRUE(loc.proxy.set_headers[0].is_template);
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

// ---- File-path resolution (absolute or relative-to-containing-config-file) ----

// A relative script_file resolves against the directory of the file that contains the
// directive (the source_name), never the process pwd. Source_name here is an absolute
// path with a directory, so the result is absolute.
TEST(LiteNginxConfigTest, RelativeScriptFileResolvesAgainstConfigDir) {
    auto config_result = ConfigLoader::load_from_string(R"(
        http {
            listen 8080;
            server {
                server_name localhost;
                location / { script_file sub/x.js; }
            }
        }
    )",
                                                        "/etc/nginx/main.conf");

    ASSERT_TRUE(config_result.has_value()) << config_result.error().message;
    EXPECT_EQ(config_result->http.servers[0].locations[0].script_file, "/etc/nginx/sub/x.js");
}

// An absolute script_file passes through unchanged.
TEST(LiteNginxConfigTest, AbsoluteScriptFilePassesThrough) {
    auto config_result = ConfigLoader::load_from_string(R"(
        http {
            listen 8080;
            server {
                server_name localhost;
                location / { script_file /abs/x.js; }
            }
        }
    )",
                                                        "/etc/nginx/main.conf");

    ASSERT_TRUE(config_result.has_value()) << config_result.error().message;
    EXPECT_EQ(config_result->http.servers[0].locations[0].script_file, "/abs/x.js");
}

// Relative certificate / certificate_key resolve against the config directory too.
TEST(LiteNginxConfigTest, RelativeCertificateResolvesAgainstConfigDir) {
    auto config_result = ConfigLoader::load_from_string(R"(
        http {
            listen 8443 ssl;
            server {
                server_name localhost;
                certificate certs/a.crt;
                certificate_key certs/a.key;
                location / { proxy_pass http://127.0.0.1:9001; }
            }
        }
    )",
                                                        "/etc/nginx/main.conf");

    ASSERT_TRUE(config_result.has_value()) << config_result.error().message;
    const auto &server = config_result->http.servers[0];
    EXPECT_EQ(server.certificate, "/etc/nginx/certs/a.crt");
    EXPECT_EQ(server.certificate_key, "/etc/nginx/certs/a.key");
}

// When source_name has no directory (a bare name, e.g. inline test configs), a relative
// path stays as-is -- preserving the historical pwd-relative behavior for those callers.
TEST(LiteNginxConfigTest, BareSourceNameKeepsRelativeScriptFile) {
    auto config_result = ConfigLoader::load_from_string(R"(
        http {
            listen 8080;
            server { server_name localhost; location / { script_file sub/x.js; } }
        }
    )",
                                                        "inline.conf");

    ASSERT_TRUE(config_result.has_value()) << config_result.error().message;
    EXPECT_EQ(config_result->http.servers[0].locations[0].script_file, "sub/x.js");
}

// load_from_file anchors the entry path to its absolute, symlink-resolved location, so a
// relative `--config` argument still makes downstream paths resolve against the config's
// real directory rather than the process pwd.
TEST(LiteNginxConfigTest, LoadFromFileAnchorsRelativeConfigPath) {
    TempDir dir("lite_nginx_rel_entry");
    write_file(dir.path / "scripts" / "x.js", "resp.sendJson(200, {ok: true});");
    write_file(dir.path / "main.conf", R"(
worker_processes 1;
http {
    listen 127.0.0.1:8080;
    server { server_name localhost; location /* { script_file scripts/x.js; } }
}
)");

    // Load the config by a path relative to a cwd inside the temp dir, so the entry path
    // itself is pwd-relative; the resolved script_file must still be absolute and anchored
    // to the config file's directory.
    ScopedCwd cwd_guard;
    std::error_code chdir_ec;
    fs::current_path(dir.path, chdir_ec);
    ASSERT_FALSE(chdir_ec) << chdir_ec.message();
    auto config_result = ConfigLoader::load_from_file("main.conf");

    ASSERT_TRUE(config_result.has_value()) << config_result.error().message;
    const auto &script_file = config_result->http.servers[0].locations[0].script_file;
    EXPECT_FALSE(script_file.empty());
    EXPECT_EQ(script_file.front(), '/') << script_file;
    EXPECT_NE(script_file.find("scripts/x.js"), std::string::npos) << script_file;
}

// ---- include directive ----

TEST(LiteNginxConfigTest, IncludeSplicesUpstreamFromAnotherFile) {
    TempDir dir("lite_nginx_inc_upstream");
    write_file(dir.path / "upstreams.conf", R"(
        upstream backend {
            server 127.0.0.1:9001;
        }
    )");
    write_file(dir.path / "main.conf", R"(
worker_processes 1;
http {
    listen 8080;
    include upstreams.conf;
    server { server_name localhost; location / { proxy_pass http://backend; } }
}
)");

    auto config_result = ConfigLoader::load_from_file((dir.path / "main.conf").string());
    ASSERT_TRUE(config_result.has_value()) << config_result.error().message;
    ASSERT_EQ(config_result->http.upstreams.size(), 1u);
    EXPECT_EQ(config_result->http.upstreams[0].name, "backend");
}

TEST(LiteNginxConfigTest, IncludeResolvesRelativeToIncludingFile) {
    TempDir dir("lite_nginx_inc_rel");
    // main.conf in dir/ includes "sub/inner.conf"; inner.conf lives in dir/sub/.
    write_file(dir.path / "sub" / "inner.conf", R"(
        upstream inner { server 127.0.0.1:9002; }
    )");
    write_file(dir.path / "main.conf", R"(
http {
    listen 8080;
    include sub/inner.conf;
    server { server_name localhost; location / { proxy_pass http://inner; } }
}
)");

    auto config_result = ConfigLoader::load_from_file((dir.path / "main.conf").string());
    ASSERT_TRUE(config_result.has_value()) << config_result.error().message;
    ASSERT_EQ(config_result->http.upstreams.size(), 1u);
    EXPECT_EQ(config_result->http.upstreams[0].name, "inner");
}

TEST(LiteNginxConfigTest, NestedIncludeResolvesRelativeToIncluder) {
    TempDir dir("lite_nginx_inc_nested");
    // a.conf includes b/b.conf; b/b.conf includes c/c.conf relative to b/.
    write_file(dir.path / "b" / "b.conf", "include c/c.conf;\n");
    write_file(dir.path / "b" / "c" / "c.conf", R"(
        upstream deep { server 127.0.0.1:9003; }
    )");
    write_file(dir.path / "a.conf", R"(
http {
    listen 8080;
    include b/b.conf;
    server { server_name localhost; location / { proxy_pass http://deep; } }
}
)");

    auto config_result = ConfigLoader::load_from_file((dir.path / "a.conf").string());
    ASSERT_TRUE(config_result.has_value()) << config_result.error().message;
    ASSERT_EQ(config_result->http.upstreams.size(), 1u);
    EXPECT_EQ(config_result->http.upstreams[0].name, "deep");
}

TEST(LiteNginxConfigTest, IncludeInsideServerBlockSplicesLocation) {
    TempDir dir("lite_nginx_inc_block");
    write_file(dir.path / "locs.conf", "location /ready { proxy_pass http://127.0.0.1:9001; }\n");
    write_file(dir.path / "main.conf", R"(
http {
    listen 8080;
    server {
        server_name localhost;
        include locs.conf;
    }
}
)");

    auto config_result = ConfigLoader::load_from_file((dir.path / "main.conf").string());
    ASSERT_TRUE(config_result.has_value()) << config_result.error().message;
    ASSERT_EQ(config_result->http.servers[0].locations.size(), 1u);
    EXPECT_EQ(config_result->http.servers[0].locations[0].pattern, "/ready");
}

TEST(LiteNginxConfigTest, IncludeCycleDetected) {
    TempDir dir("lite_nginx_inc_cycle");
    write_file(dir.path / "a.conf", "include b.conf;\n");
    write_file(dir.path / "b.conf", "include a.conf;\n");

    auto config_result = ConfigLoader::load_from_file((dir.path / "a.conf").string());
    ASSERT_FALSE(config_result.has_value());
    EXPECT_NE(config_result.error().message.find("include cycle"), std::string::npos);
}

TEST(LiteNginxConfigTest, MissingIncludeFileErrors) {
    TempDir dir("lite_nginx_inc_missing");
    write_file(dir.path / "main.conf", R"(
http {
    listen 8080;
    include does_not_exist.conf;
    server { server_name localhost; location / { proxy_pass http://127.0.0.1:9001; } }
}
)");

    auto config_result = ConfigLoader::load_from_file((dir.path / "main.conf").string());
    ASSERT_FALSE(config_result.has_value());
    EXPECT_NE(config_result.error().message.find("include file not found"), std::string::npos);
    // The error location is the missing included file path.
    EXPECT_NE(config_result.error().location.source_name.find("does_not_exist.conf"), std::string::npos);
}

TEST(LiteNginxConfigTest, ParsesLoggingAndAccessLogInheritanceInputs) {
    TempDir dir("lite_nginx_logging_config");
    const fs::path config_path = dir.path / "lite_nginx.conf";
    write_file(config_path, R"(
logging {
    appender access_file {
        type file;
        path logs/access.log;
        mode 0640;
        buffer_size 64k;
        flush_interval 200ms;
        rotate_size 128k;
        archive_name "{base}.{utc}.{seq}";
        rotate_keep 14;
        min_level info;
        max_level info;
    }
    appender stderr {
        type console;
        stream stderr;
        min_level warn;
    }
    logger lite_nginx.access {
        level info;
        appender access_file;
        additive off;
    }
    root_logger {
        level info;
        verbosity 2;
        appender stderr;
    }
}
http {
    listen 8080;
    access_log lite_nginx.access "http ${$req.method}";
    server {
        server_name localhost;
        access_log off;
        location /health {
            access_log location.access "location ${$req.path}";
            proxy_pass http://127.0.0.1:9001;
        }
    }
}
)");

    auto config = ConfigLoader::load_from_file(config_path.string());
    ASSERT_TRUE(config.has_value()) << config.error().message;
    ASSERT_TRUE(config->logging.configured);
    ASSERT_EQ(config->logging.appenders.size(), 2u);
    const auto &access = config->logging.appenders[0];
    EXPECT_EQ(access.name, "access_file");
    EXPECT_EQ(access.kind, LogAppenderKind::File);
    EXPECT_EQ(access.path, (dir.path / "logs/access.log").lexically_normal().string());
    EXPECT_EQ(access.file_mode, 0640u);
    EXPECT_EQ(access.buffer_size, 64u * 1024u);
    EXPECT_EQ(access.flush_interval, std::chrono::milliseconds(200));
    ASSERT_TRUE(access.rotation.has_value());
    EXPECT_EQ(access.rotation->max_file_size, 128u * 1024u);
    EXPECT_EQ(access.rotation->archive_name, "{base}.{utc}.{seq}");
    EXPECT_EQ(access.rotation->max_archives, 14u);
    EXPECT_EQ(access.min_level, LoggingLevel::Info);
    EXPECT_EQ(access.max_level, LoggingLevel::Info);
    ASSERT_EQ(config->logging.loggers.size(), 1u);
    EXPECT_FALSE(config->logging.loggers[0].additive);
    EXPECT_EQ(config->logging.root.verbosity, 2u);
    ASSERT_TRUE(config->http.access_log.has_value());
    EXPECT_EQ(config->http.access_log->kind, AccessLogKind::Template);
    EXPECT_EQ(config->http.access_log->logger_name, "lite_nginx.access");
    EXPECT_EQ(config->http.access_log->message_template, "http ${$req.method}");
    ASSERT_TRUE(config->http.servers[0].access_log.has_value());
    EXPECT_EQ(config->http.servers[0].access_log->kind, AccessLogKind::Off);
    ASSERT_TRUE(config->http.servers[0].locations[0].access_log.has_value());
    EXPECT_EQ(config->http.servers[0].locations[0].access_log->kind, AccessLogKind::Template);
    EXPECT_EQ(config->http.servers[0].locations[0].access_log->logger_name, "location.access");
}

TEST(LiteNginxConfigTest, RejectsInvalidAccessLogShapeAndLoggerName) {
    auto old_on = ConfigLoader::load_from_string(R"(
http {
    listen 8080;
    access_log on;
    server { server_name localhost; location / { proxy_pass http://127.0.0.1:9001; } }
}
)",
                                                 "old_access_log.conf");
    ASSERT_FALSE(old_on.has_value());
    EXPECT_NE(old_on.error().message.find("expects '<logger-name> <message-template>' or 'off'"), std::string::npos);

    auto invalid_name = ConfigLoader::load_from_string(R"(
http {
    listen 8080;
    access_log bad..name "message";
    server { server_name localhost; location / { proxy_pass http://127.0.0.1:9001; } }
}
)",
                                                       "invalid_access_logger.conf");
    ASSERT_FALSE(invalid_name.has_value());
    EXPECT_NE(invalid_name.error().message.find("logger name is invalid"), std::string::npos);
}

TEST(LiteNginxConfigTest, RejectsInvalidLoggingConfiguration) {
    auto unknown_appender = ConfigLoader::load_from_string(R"(
logging {
    logger lite_nginx.access { appender missing; additive off; }
    root_logger { level info; }
}
http {
    listen 8080;
    server { server_name localhost; location / { proxy_pass http://127.0.0.1:9001; } }
}
)",
                                                           "bad_logging.conf");
    ASSERT_FALSE(unknown_appender.has_value());
    EXPECT_NE(unknown_appender.error().message.find("unknown appender"), std::string::npos);

    auto invalid_buffer = ConfigLoader::load_from_string(R"(
logging {
    appender file { type file; path access.log; buffer_size 4k; }
    root_logger { appender file; }
}
http {
    listen 8080;
    server { server_name localhost; location / { proxy_pass http://127.0.0.1:9001; } }
}
)",
                                                         "bad_buffer.conf");
    ASSERT_FALSE(invalid_buffer.has_value());
    EXPECT_NE(invalid_buffer.error().message.find("buffer_size and flush_interval"), std::string::npos);

    auto incomplete_rotation = ConfigLoader::load_from_string(R"(
logging {
    appender file { type file; path access.log; rotate_size 128k; }
    root_logger { appender file; }
}
http {
    listen 8080;
    server { server_name localhost; location / { proxy_pass http://127.0.0.1:9001; } }
}
)",
                                                              "bad_rotation.conf");
    ASSERT_FALSE(incomplete_rotation.has_value());
    EXPECT_NE(incomplete_rotation.error().message.find("must be configured together"), std::string::npos);

    auto invalid_archive_name = ConfigLoader::load_from_string(R"(
logging {
    appender file {
        type file;
        path access.log;
        rotate_size 128k;
        archive_name "../{base}.{seq}";
        rotate_keep 4;
    }
    root_logger { appender file; }
}
http {
    listen 8080;
    server { server_name localhost; location / { proxy_pass http://127.0.0.1:9001; } }
}
)",
                                                               "bad_archive_name.conf");
    ASSERT_FALSE(invalid_archive_name.has_value());
    EXPECT_NE(invalid_archive_name.error().message.find("archive_name"), std::string::npos);
}

} // namespace
