// TLS handshake engines in QUIC mode (feature/tls/10 slice 1): CRYPTO-stream
// interop against real BoringSSL QUIC peers over SSL_QUIC_METHOD, plus the
// QUIC-only protocol rules that diverge from the TCP shape (RFC 9001):
// no EndOfEarlyData — transcript included (§8.3), no middlebox compatibility
// mode (§8.4), mandatory ALPN (§8.1), the 0xffffffff max_early_data sentinel
// in tickets (§4.6.1), and the write-secret-before-read-secret export order.
//
// The CRYPTO streams themselves flow plaintext — the Finished messages only
// bind the handshake secrets — so the interop tests byte-compare EVERY
// set_secret export against the BoringSSL peer's own captures (hs read/write,
// app read/write, 0-RTT where offered). That subsumes the RFC 9001 Appendix A
// key-derivation vectors: if the application secrets match a real BoringSSL
// peer's, the schedule is transitively correct.

#include <gtest/gtest.h>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "TlsCertFixtures.h"
#include "tls/handshake/TlsClientHandshakeEngine.h"
#include "tls/handshake/TlsServerHandshakeEngine.h"

#include <fiber/tls/TlsConfig.h>
#include <fiber/tls/TlsConnectedState.h>
#include <fiber/tls/TlsTicketService.h>
#include <fiber/tls/TlsVersion.h>
#include <fiber/tls/crypto/Tls13KeySchedule.h>
#include <fiber/tls/crypto/TlsSecret.h>
#include <fiber/tls/handshake/TlsHandshakeCodec.h>
#include <fiber/tls/handshake/TlsHandshakeMessage.h>
#include "LoopTestSupport.h"

namespace {

namespace certfix = fiber::tls::certfix;

using fiber::tls::TlsAlertDesc;
using fiber::tls::TlsCertificateChain;
using fiber::tls::TlsCipherSuiteId;
using fiber::tls::TlsClientConfig;
using fiber::tls::TlsClientHandshakeEngine;
using fiber::tls::TlsPrivateKey;
using fiber::tls::TlsQuicCallbacks;
using fiber::tls::TlsQuicHandshakeResult;
using fiber::tls::TlsQuicLevel;
using fiber::tls::TlsResumptionLookup;
using fiber::tls::TlsSecret;
using fiber::tls::TlsServerConfig;
using fiber::tls::TlsServerHandshakeEngine;
using fiber::tls::TlsSessionOffer;
using fiber::tls::TlsTicketKeyMaterial;
using fiber::tls::TlsTicketKeyPolicy;
using fiber::tls::TlsTicketMinter;
using fiber::tls::TlsTicketRequest;
using fiber::tls::TlsTicketService;
using Event = TlsServerHandshakeEngine::Event;

// TlsQuicLevel mirrors ssl_encryption_level_t value-for-value (the QUIC
// layer casts across the boundary in both directions).
static_assert(static_cast<int>(TlsQuicLevel::Initial) == ssl_encryption_initial);
static_assert(static_cast<int>(TlsQuicLevel::EarlyData) == ssl_encryption_early_data);
static_assert(static_cast<int>(TlsQuicLevel::Handshake) == ssl_encryption_handshake);
static_assert(static_cast<int>(TlsQuicLevel::Application) == ssl_encryption_application);

// ---- BoringSSL loading helpers (test-only OpenSSL usage) ----

X509 *load_cert(const char *pem) {
    BIO *bio = BIO_new_mem_buf(pem, -1);
    X509 *cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return cert;
}

EVP_PKEY *load_key(const char *pem) {
    BIO *bio = BIO_new_mem_buf(pem, -1);
    EVP_PKEY *key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return key;
}

// ALPN selection: the FIRST protocol the client offered (order = preference,
// so the client can assert its own preference was picked).
int select_first_alpn(SSL *, const unsigned char **out, unsigned char *out_len, const unsigned char *in,
                      unsigned int in_len, void *) {
    if (in_len == 0 || in_len > 255 || 1 + in[0] > in_len) {
        return SSL_TLSEXT_ERR_NOACK;
    }
    *out = in + 1;
    *out_len = in[0];
    return SSL_TLSEXT_ERR_OK;
}

// =====================================================================
// Engine-side capture sink — TlsQuicCallbacks into vectors
// =====================================================================

struct EngineQuicSink {
    struct SecretRec {
        TlsQuicLevel level;
        bool write;
        TlsCipherSuiteId suite;
        std::vector<std::uint8_t> bytes;
    };
    struct DataRec {
        TlsQuicLevel level;
        std::vector<std::uint8_t> bytes;
    };

    std::vector<SecretRec> secrets;
    std::vector<DataRec> out; // undrained queue (the pumps move it out per spin)
    std::vector<DataRec> all; // cumulative log — assertions read this post-drive
    std::vector<std::vector<std::uint8_t>> peer_params;
    std::vector<TlsAlertDesc> alerts;

    // Emission order: 'W'/'R' secret at level, 'D' data at level — pins the
    // write-before-read contract and "no data before the level's write secret".
    std::vector<std::pair<char, TlsQuicLevel>> order;

    // The config hands this pointer out; the sink outlives the engine.
    TlsQuicCallbacks cb{&set_secret_fn, &add_data_fn, &on_params_fn, &send_alert_fn, this};

    [[nodiscard]] std::vector<DataRec> drain_out() noexcept { return std::move(out); }

    // The concatenated CRYPTO bytes emitted at `level`.
    [[nodiscard]] std::vector<std::uint8_t> level_bytes(TlsQuicLevel level) const {
        std::vector<std::uint8_t> joined;
        for (const DataRec &rec: all) {
            if (rec.level == level) {
                joined.insert(joined.end(), rec.bytes.begin(), rec.bytes.end());
            }
        }
        return joined;
    }

    [[nodiscard]] const SecretRec *find_secret(TlsQuicLevel level, bool write) const noexcept {
        for (const SecretRec &rec: secrets) {
            if (rec.level == level && rec.write == write) {
                return &rec;
            }
        }
        return nullptr;
    }

    static bool set_secret_fn(void *ctx, TlsQuicLevel level, bool write, TlsCipherSuiteId suite,
                              std::span<const std::uint8_t> secret) noexcept {
        auto &self = *static_cast<EngineQuicSink *>(ctx);
        self.secrets.push_back(SecretRec{level, write, suite, {secret.begin(), secret.end()}});
        self.order.emplace_back(write ? 'W' : 'R', level);
        return true;
    }

    static bool add_data_fn(void *ctx, TlsQuicLevel level, std::span<const std::uint8_t> data) noexcept {
        auto &self = *static_cast<EngineQuicSink *>(ctx);
        const DataRec rec{level, {data.begin(), data.end()}};
        self.out.push_back(rec);
        self.all.push_back(rec);
        self.order.emplace_back('D', level);
        return true;
    }

    static void on_params_fn(void *ctx, std::span<const std::uint8_t> params) noexcept {
        static_cast<EngineQuicSink *>(ctx)->peer_params.push_back({params.begin(), params.end()});
    }

    static void send_alert_fn(void *ctx, TlsAlertDesc desc) noexcept {
        static_cast<EngineQuicSink *>(ctx)->alerts.push_back(desc);
    }
};

// =====================================================================
// BoringQuic — an SSL over SSL_QUIC_METHOD capturing the same shapes
// =====================================================================

struct BoringQuic {
    struct SecretRec {
        int level;
        bool write;
        std::uint16_t cipher_id;
        std::vector<std::uint8_t> bytes;
    };

    SSL_CTX *ctx = nullptr;
    SSL *ssl = nullptr;
    bool owns_ctx = true;

    std::vector<SecretRec> secrets;
    std::vector<std::pair<int, std::vector<std::uint8_t>>> out;
    std::vector<std::uint8_t> alert_bytes;
    std::vector<std::string> order;

    ~BoringQuic() {
        if (ssl != nullptr) {
            SSL_free(ssl);
        }
        if (owns_ctx && ctx != nullptr) {
            SSL_CTX_free(ctx);
        }
    }

    BoringQuic(const BoringQuic &) = delete;
    BoringQuic &operator=(const BoringQuic &) = delete;

    [[nodiscard]] std::vector<std::pair<int, std::vector<std::uint8_t>>> drain_out() noexcept { return std::move(out); }

    [[nodiscard]] const SecretRec *find_secret(int level, bool write) const noexcept {
        for (const SecretRec &rec: secrets) {
            if (rec.level == level && rec.write == write) {
                return &rec;
            }
        }
        return nullptr;
    }

    // ---- client ----

    struct ClientOptions {
        bool verify_server = true; // trust kRootRsaPem + pin host example.com
        const char *groups = nullptr; // non-null: restrict/reorder (HRR trigger)
    };

    [[nodiscard]] static std::unique_ptr<BoringQuic> make_client(const ClientOptions &opt,
                                                                 std::span<const std::uint8_t> transport_params) {
        SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
        if (ctx == nullptr) {
            return nullptr;
        }
        SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
        SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
        if (opt.verify_server) {
            X509 *root = load_cert(certfix::kRootRsaPem);
            if (root == nullptr || X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx), root) != 1) {
                SSL_CTX_free(ctx);
                return nullptr;
            }
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        } else {
            SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
        }
        return finish_new(ctx, true, opt.groups, transport_params);
    }

    // ---- server ----

    struct ServerOptions {
        const char *groups = nullptr; // non-null: restrict (forces HRR vs X25519 shares)
    };

    [[nodiscard]] static std::unique_ptr<BoringQuic> make_server(const ServerOptions &opt,
                                                                 std::span<const std::uint8_t> transport_params) {
        SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
        if (ctx == nullptr) {
            return nullptr;
        }
        SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
        SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
        X509 *leaf = load_cert(certfix::kLeafRsaPem);
        X509 *intermediate = load_cert(certfix::kIntermediateRsaPem);
        EVP_PKEY *key = load_key(certfix::kRsa2048KeyPem);
        if (leaf == nullptr || intermediate == nullptr || key == nullptr || SSL_CTX_use_certificate(ctx, leaf) != 1 ||
            SSL_CTX_add0_chain_cert(ctx, intermediate) != 1 || SSL_CTX_use_PrivateKey(ctx, key) != 1 ||
            SSL_CTX_check_private_key(ctx) != 1) {
            SSL_CTX_free(ctx);
            return nullptr;
        }
        SSL_CTX_set_alpn_select_cb(ctx, select_first_alpn, nullptr);
        return finish_new(ctx, false, opt.groups, transport_params);
    }

    // One non-blocking handshake step: 1 = complete, 0 = progress expected,
    // -1 = the peer itself failed.
    [[nodiscard]] int handshake_step() const {
        const int rc = SSL_do_handshake(ssl);
        if (rc == 1) {
            return 1;
        }
        const int err = SSL_get_error(ssl, rc);
        return (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) ? 0 : -1;
    }

    // Runs handshake_step to quiescence (progress or completion).
    [[nodiscard]] bool step() {
        for (int spin = 0; spin < 32; ++spin) {
            const int rc = handshake_step();
            if (rc == 1) {
                return true;
            }
            if (rc < 0) {
                ERR_print_errors_fp(stderr);
                return false;
            }
        }
        return true; // still WANT_READ — waiting for more CRYPTO
    }

    // Data at `level` in. Post-handshake bytes (NSTs) route through
    // SSL_process_quic_post_handshake.
    [[nodiscard]] bool provide(int level, std::span<const std::uint8_t> bytes) {
        if (bytes.empty()) {
            return true;
        }
        if (SSL_provide_quic_data(ssl, static_cast<ssl_encryption_level_t>(level), bytes.data(), bytes.size()) != 1) {
            ERR_print_errors_fp(stderr);
            return false;
        }
        if (static_cast<bool>(SSL_is_init_finished(ssl))) {
            if (SSL_process_quic_post_handshake(ssl) != 1) {
                ERR_print_errors_fp(stderr);
                return false;
            }
            return step();
        }
        return step();
    }

    [[nodiscard]] bool finished() const noexcept { return static_cast<bool>(SSL_is_init_finished(ssl)); }

private:
    BoringQuic() = default;

    [[nodiscard]] static std::unique_ptr<BoringQuic> finish_new(SSL_CTX *ctx, bool client, const char *groups,
                                                                std::span<const std::uint8_t> transport_params) {
        auto peer = std::unique_ptr<BoringQuic>(new BoringQuic());
        peer->ctx = ctx;
        peer->ssl = SSL_new(ctx);
        if (peer->ssl == nullptr) {
            return nullptr;
        }
        if (client) {
            SSL_set_connect_state(peer->ssl);
            if (SSL_set1_host(peer->ssl, "example.com") != 1) {
                return nullptr;
            }
            // "h2", "http/1.1" — split literals: \x02h would greedily parse
            // as one hex escape ('h' is a hex digit).
            static const unsigned char kProtos[] = "\x02"
                                                   "h2"
                                                   "\x08"
                                                   "http/1.1";
            if (SSL_set_alpn_protos(peer->ssl, kProtos, sizeof(kProtos) - 1) != 0) {
                return nullptr;
            }
        } else {
            SSL_set_accept_state(peer->ssl);
        }
        if (groups != nullptr && SSL_set1_groups_list(peer->ssl, groups) != 1) {
            return nullptr;
        }
        SSL_set_app_data(peer->ssl, peer.get());
        if (SSL_set_quic_method(peer->ssl, &kMethod) != 1 ||
            SSL_set_quic_transport_params(peer->ssl, transport_params.data(), transport_params.size()) != 1) {
            return nullptr;
        }
        return peer;
    }

    static int on_set_read_secret(SSL *ssl, ssl_encryption_level_t level, const SSL_CIPHER *cipher,
                                  const uint8_t *secret, size_t len) {
        auto &self = *static_cast<BoringQuic *>(SSL_get_app_data(ssl));
        self.secrets.push_back(
                SecretRec{static_cast<int>(level), false, SSL_CIPHER_get_protocol_id(cipher), {secret, secret + len}});
        self.order.push_back("R" + std::to_string(static_cast<int>(level)));
        return 1;
    }

    static int on_set_write_secret(SSL *ssl, ssl_encryption_level_t level, const SSL_CIPHER *cipher,
                                   const uint8_t *secret, size_t len) {
        auto &self = *static_cast<BoringQuic *>(SSL_get_app_data(ssl));
        self.secrets.push_back(
                SecretRec{static_cast<int>(level), true, SSL_CIPHER_get_protocol_id(cipher), {secret, secret + len}});
        self.order.push_back("W" + std::to_string(static_cast<int>(level)));
        return 1;
    }

    static int on_add_handshake_data(SSL *ssl, ssl_encryption_level_t level, const uint8_t *data, size_t len) {
        auto &self = *static_cast<BoringQuic *>(SSL_get_app_data(ssl));
        self.out.emplace_back(static_cast<int>(level), std::vector<std::uint8_t>(data, data + len));
        self.order.push_back("D" + std::to_string(static_cast<int>(level)));
        return 1;
    }

    static int on_flush_flight(SSL *) { return 1; }

    static int on_send_alert(SSL *ssl, ssl_encryption_level_t, uint8_t alert) {
        auto &self = *static_cast<BoringQuic *>(SSL_get_app_data(ssl));
        self.alert_bytes.push_back(alert);
        return 1;
    }

    static const SSL_QUIC_METHOD kMethod;
};

const SSL_QUIC_METHOD BoringQuic::kMethod = {&BoringQuic::on_set_read_secret, &BoringQuic::on_set_write_secret,
                                             &BoringQuic::on_add_handshake_data, &BoringQuic::on_flush_flight,
                                             &BoringQuic::on_send_alert};

// =====================================================================
// Shared material + assertion helpers
// =====================================================================

constexpr std::array<std::uint8_t, 8> kClientParams{0xC1, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
constexpr std::array<std::uint8_t, 8> kServerParams{0x5A, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};

struct QuicMaterial {
    std::string chain_pem;
    std::optional<TlsCertificateChain> chain;
    std::optional<TlsPrivateKey> key;
    std::vector<std::string_view> alpn{"h2", "http/1.1"};
    std::optional<fiber::tls::TlsTrustStore> trust;

    QuicMaterial() {
        chain_pem.assign(certfix::kLeafRsaPem);
        chain_pem.append(certfix::kIntermediateRsaPem);
        auto parsed = TlsCertificateChain::parse_pem_bundle({chain_pem.data(), chain_pem.size()});
        EXPECT_TRUE(parsed.has_value()); // ASSERT may not return from a ctor
        chain = std::move(parsed).value();
        auto parsed_key = TlsPrivateKey::parse_pem({certfix::kRsa2048KeyPem, std::strlen(certfix::kRsa2048KeyPem)});
        EXPECT_TRUE(parsed_key.has_value());
        key = std::move(parsed_key).value();
        auto parsed_trust =
                fiber::tls::TlsTrustStore::from_pem_bundle({certfix::kRootRsaPem, std::strlen(certfix::kRootRsaPem)});
        EXPECT_TRUE(parsed_trust.has_value());
        trust = std::move(parsed_trust).value();
    }

    [[nodiscard]] TlsServerConfig server_cfg(EngineQuicSink &sink) const {
        TlsServerConfig cfg;
        cfg.chain = &*chain;
        cfg.key = &*key;
        cfg.alpn = alpn;
        cfg.now_unix_ms = certfix::kRefNowMs;
        cfg.min_version = fiber::tls::kTlsVersionTls13; // QUIC is 1.3-only (10 §3.4)
        cfg.max_version = fiber::tls::kTlsVersionTls13;
        cfg.quic = &sink.cb;
        cfg.quic_transport_params = kServerParams;
        return cfg;
    }

    [[nodiscard]] TlsClientConfig client_cfg(EngineQuicSink &sink) const {
        TlsClientConfig cfg;
        cfg.sni_host = "example.com";
        cfg.alpn = alpn;
        cfg.trust = &*trust;
        cfg.now_unix_ms = certfix::kRefNowMs;
        cfg.min_version = fiber::tls::kTlsVersionTls13;
        cfg.max_version = fiber::tls::kTlsVersionTls13;
        cfg.quic = &sink.cb;
        cfg.quic_transport_params = kClientParams;
        return cfg;
    }
};

// The engine-side callback contract: where a side exports BOTH directions at
// a level the write secret precedes the read secret (the BoringSSL QUIC
// ordering), and CRYPTO data never precedes the level's write secret (Initial
// is pre-keyed by construction; EarlyData is stream-only, so a server that
// accepts 0-RTT exports ONLY the read secret there — legal).
void expect_engine_export_order(const EngineQuicSink &sink) {
    const auto first_of = [&sink](char kind, TlsQuicLevel level) -> int {
        for (std::size_t i = 0; i < sink.order.size(); ++i) {
            if (sink.order[i].first == kind && sink.order[i].second == level) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };
    for (const TlsQuicLevel level: {TlsQuicLevel::EarlyData, TlsQuicLevel::Handshake, TlsQuicLevel::Application}) {
        const int w = first_of('W', level);
        const int r = first_of('R', level);
        const int d = first_of('D', level);
        if (r >= 0 && w >= 0) {
            EXPECT_TRUE(w < r) << "read secret before write secret at level " << static_cast<int>(level);
        }
        if (d >= 0 && level != TlsQuicLevel::Initial) {
            EXPECT_TRUE(w >= 0 && w < d) << "data before write secret at level " << static_cast<int>(level);
        }
    }
}

// Levels of the emitted CRYPTO stream are monotone non-decreasing, and
// add_handshake_data never fires at EarlyData (0-RTT is STREAM frames; the
// CRYPTO stream has no 0-RTT level content by construction).
void expect_crypto_level_shape(const EngineQuicSink &sink) {
    int last = static_cast<int>(TlsQuicLevel::Initial);
    for (const EngineQuicSink::DataRec &rec: sink.all) {
        const int lvl = static_cast<int>(rec.level);
        EXPECT_GE(lvl, last);
        EXPECT_NE(static_cast<int>(TlsQuicLevel::EarlyData), lvl);
        last = std::max(last, lvl);
    }
}

bool secrets_equal(const std::vector<std::uint8_t> &a, const std::vector<std::uint8_t> &b) noexcept {
    return a.size() == b.size() && 0 == std::memcmp(a.data(), b.data(), a.size());
}

// BoringSSL's server preference picks ChaCha20 on hosts without AES
// acceleration, so the negotiated suite is host-dependent: derive the
// expected Finished size (4-byte header + hash-length verify_data) instead
// of pinning a suite. Only 0x1302 carries SHA-384.
std::size_t finished_size(TlsCipherSuiteId suite) noexcept {
    return 4 + (suite == TlsCipherSuiteId::TlsAes256GcmSha384 ? 48 : 32);
}

// Byte-compares every engine export against the BoringSSL peer's capture.
// The pairing always flips directions — our client faces their server and
// vice versa — so the engine's write secret at a level must equal the peer's
// READ secret there (both derive from client_X_traffic_secret, etc.).
// EarlyData is the one asymmetric level: with 0-RTT rejected the engine
// (client) still exported its early write secret while the peer (server)
// derived nothing there — legal, nothing to compare.
void expect_secrets_match(const EngineQuicSink &engine, const BoringQuic &peer) {
    for (const EngineQuicSink::SecretRec &rec: engine.secrets) {
        const BoringQuic::SecretRec *same_dir = peer.find_secret(static_cast<int>(rec.level), rec.write);
        const BoringQuic::SecretRec *other = peer.find_secret(static_cast<int>(rec.level), !rec.write);
        if (same_dir == nullptr) { // the peer skipped the level entirely
            ASSERT_EQ(nullptr, other);
            ASSERT_EQ(TlsQuicLevel::EarlyData, rec.level); // only 0-RTT rejection does this
            continue;
        }
        EXPECT_EQ(same_dir->cipher_id, static_cast<std::uint16_t>(rec.suite));
        if (rec.level == TlsQuicLevel::EarlyData && other == nullptr) {
            continue; // rejected 0-RTT — engine-only early export
        }
        ASSERT_NE(nullptr, other);
        EXPECT_TRUE(secrets_equal(rec.bytes, other->bytes))
                << "secret mismatch level=" << static_cast<int>(rec.level) << " write=" << rec.write;
    }
}

// =====================================================================
// Drive loops
// =====================================================================

// Our client engine ↔ a BoringSSL QUIC server. Guards each direction on its
// receiver: a done engine cannot be fed (the post-handshake NST the server
// emits is capture-only).
bool pump_client_engine(EngineQuicSink &sink, TlsClientHandshakeEngine &engine, BoringQuic &peer) {
    for (int spin = 0; spin < 128; ++spin) {
        bool progressed = false;
        for (const auto &rec: sink.drain_out()) {
            progressed = true;
            if (!peer.provide(static_cast<int>(rec.level), rec.bytes)) {
                return false;
            }
        }
        if (engine.done() && peer.finished()) {
            return !engine.failed();
        }
        if (engine.failed()) {
            return false;
        }
        for (const auto &rec: peer.drain_out()) {
            progressed = true;
            if (engine.done()) {
                continue; // post-handshake tail — not the engine's to consume (slice 2)
            }
            const auto event = engine.feed_quic(static_cast<TlsQuicLevel>(rec.first), rec.second);
            if (!event.has_value()) {
                return false;
            }
        }
        if (engine.done() && peer.finished()) {
            return !engine.failed();
        }
        if (engine.failed()) {
            return false;
        }
        if (!progressed && !engine.done() && !peer.finished()) {
            return false; // no forward motion available — deadlock, not drive
        }
    }
    return false;
}

// Our server engine ↔ a BoringSSL QUIC client. The client speaks first.
bool pump_server_engine(EngineQuicSink &sink, TlsServerHandshakeEngine &engine, BoringQuic &peer) {
    if (!peer.step()) {
        return false;
    }
    for (int spin = 0; spin < 128; ++spin) {
        bool progressed = false;
        for (const auto &rec: peer.drain_out()) {
            progressed = true;
            if (engine.done()) {
                continue; // post-handshake tail (client KeyUpdate etc.) — slice 2
            }
            const auto event = engine.feed_quic(static_cast<TlsQuicLevel>(rec.first), rec.second);
            if (!event.has_value()) {
                return false;
            }
        }
        if (engine.done() && peer.finished()) {
            return !engine.failed();
        }
        if (engine.failed()) {
            return false;
        }
        for (const auto &rec: sink.drain_out()) {
            progressed = true;
            if (!peer.provide(static_cast<int>(rec.level), rec.bytes)) {
                return false;
            }
        }
        if (engine.done() && peer.finished()) {
            return !engine.failed();
        }
        if (engine.failed()) {
            return false;
        }
        if (!progressed && !engine.done() && !peer.finished()) {
            return false;
        }
    }
    return false;
}

// Both FSMs ours: each sink's output becomes the other engine's feed.
bool pump_self(EngineQuicSink &client_sink, TlsClientHandshakeEngine &client, EngineQuicSink &server_sink,
               TlsServerHandshakeEngine &server) {
    for (int spin = 0; spin < 128 && !(client.done() && server.done()); ++spin) {
        bool progressed = false;
        for (const auto &rec: client_sink.drain_out()) {
            progressed = true;
            if (!server.done() && !server.feed_quic(rec.level, rec.bytes).has_value()) {
                return false;
            }
        }
        for (const auto &rec: server_sink.drain_out()) {
            progressed = true;
            if (!client.done() && !client.feed_quic(rec.level, rec.bytes).has_value()) {
                return false;
            }
        }
        if (client.failed() || server.failed()) {
            return false;
        }
        if (!progressed && !(client.done() && server.done())) {
            return false;
        }
    }
    return client.done() && server.done() && !client.failed() && !server.failed();
}

// =====================================================================
// ClientHello surgery for the QUIC-only rejection rules
// =====================================================================

// The retained first flight (Initial-level CRYPTO) of a driven client engine.
std::vector<std::uint8_t> take_client_hello(EngineQuicSink &sink) {
    const std::vector<EngineQuicSink::DataRec> recs = sink.drain_out();
    for (const EngineQuicSink::DataRec &rec: recs) {
        if (rec.level == TlsQuicLevel::Initial && !rec.bytes.empty() && rec.bytes[0] == 0x01) {
            return rec.bytes;
        }
    }
    return {};
}

// Splices a 32-byte legacy_session_id into a ClientHello (fixing the message
// length) — §8.4's compat-mode client shape. Message layout: 4-byte header,
// legacy_version(2), random(32) → the session_id_len byte sits at offset 38.
std::vector<std::uint8_t> with_session_id(std::vector<std::uint8_t> ch) {
    if (ch.size() < 40 || ch[38] != 0) {
        return {};
    }
    std::vector<std::uint8_t> patched;
    patched.reserve(ch.size() + 32);
    patched.insert(patched.end(), ch.begin(), ch.begin() + 38);
    patched.push_back(32);
    patched.insert(patched.end(), 32, 0xAB);
    patched.insert(patched.end(), ch.begin() + 39, ch.end());
    const std::uint32_t body = static_cast<std::uint32_t>(patched.size() - 4);
    patched[1] = static_cast<std::uint8_t>(body >> 16);
    patched[2] = static_cast<std::uint8_t>(body >> 8);
    patched[3] = static_cast<std::uint8_t>(body);
    return patched;
}

// Removes the ALPN extension from a ClientHello, fixing the extension-block
// length and the message length. False when there is nothing to strip. The
// decoder wants the message BODY — everything past the 4-byte header.
bool strip_alpn(std::vector<std::uint8_t> &ch) {
    fiber::tls::TlsClientHello view;
    if (ch.size() < 4 || !fiber::tls::tls_decode_client_hello(ch.data() + 4, ch.size() - 4, view).has_value() ||
        !view.has_alpn) {
        return false;
    }
    const std::size_t ext_off = static_cast<std::size_t>(view.extensions_block.data() - ch.data());
    const std::size_t block_len = view.extensions_block.size();
    const std::size_t list_end = ext_off + block_len;
    std::vector<std::uint8_t> kept;
    std::size_t off = ext_off;
    while (off + 4 <= list_end) {
        const std::size_t type = (static_cast<std::size_t>(ch[off]) << 8) | ch[off + 1];
        const std::size_t len = (static_cast<std::size_t>(ch[off + 2]) << 8) | ch[off + 3];
        if (off + 4 + len > list_end) {
            return false;
        }
        if (type != 0x0010) { // application_layer_protocol_negotiation
            kept.insert(kept.end(), ch.begin() + static_cast<std::ptrdiff_t>(off),
                        ch.begin() + static_cast<std::ptrdiff_t>(off + 4 + len));
        }
        off += 4 + len;
    }
    if (kept.size() > 0xFFFF) {
        return false;
    }
    std::vector<std::uint8_t> patched;
    patched.reserve(4 + (ext_off - 4) + 2 + kept.size() + (ch.size() - list_end));
    // The 4-byte header and the body up to the extension block length prefix.
    patched.insert(patched.end(), ch.begin(), ch.begin() + static_cast<std::ptrdiff_t>(ext_off - 2));
    patched.push_back(static_cast<std::uint8_t>(kept.size() >> 8));
    patched.push_back(static_cast<std::uint8_t>(kept.size()));
    patched.insert(patched.end(), kept.begin(), kept.end());
    patched.insert(patched.end(), ch.begin() + static_cast<std::ptrdiff_t>(list_end), ch.end());
    const std::uint32_t body = static_cast<std::uint32_t>(patched.size() - 4);
    patched[1] = static_cast<std::uint8_t>(body >> 16);
    patched[2] = static_cast<std::uint8_t>(body >> 8);
    patched[3] = static_cast<std::uint8_t>(body);
    ch = std::move(patched);
    return true;
}

// =====================================================================
// In-memory session store (mirrors TlsServerHandshakeEngineTest's)
// =====================================================================

class TestSessionStore {
public:
    struct Entry {
        std::vector<std::uint8_t> psk;
        TlsCipherSuiteId suite = TlsCipherSuiteId::TlsAes128GcmSha256;
        std::string alpn;
        std::uint32_t age_add = 0;
        std::uint32_t max_early_data = 0;
        std::int64_t issued_ms = 0;
        bool quic = false; // the mint face — echoed so the engine's face gate passes QUIC tickets
    };

    [[nodiscard]] const Entry *first_entry() const noexcept {
        return entries.empty() ? nullptr : &entries.begin()->second;
    }

    // The face-gate probe: relabel the stored ticket as TCP-minted so the
    // engine's face gate (10 §6.1) must treat the offer as a miss.
    void flip_first_face() noexcept {
        if (!entries.empty()) {
            entries.begin()->second.quic = false;
        }
    }

    [[nodiscard]] std::vector<std::uint8_t> first_ticket() const {
        return entries.empty() ? std::vector<std::uint8_t>{} : entries.begin()->first;
    }

    static std::size_t mint(void *ctx, const TlsTicketRequest &req, std::span<std::uint8_t> out) noexcept {
        auto &self = *static_cast<TestSessionStore *>(ctx);
        const std::array<std::uint8_t, 1> nonce{req.ticket_nonce};
        auto psk = fiber::tls::tls13_resumption_psk(TlsSecret::from_bytes(req.resumption_master), nonce);
        if (!psk.has_value()) {
            return 0;
        }
        Entry entry;
        entry.psk.assign(psk->bytes().begin(), psk->bytes().end());
        entry.suite = req.suite;
        entry.alpn.assign(req.alpn);
        entry.age_add = req.ticket_age_add;
        entry.max_early_data = req.max_early_data;
        entry.issued_ms = req.now_unix_ms;
        entry.quic = req.quic;
        const std::uint64_t id = self.next_id++;
        std::array<std::uint8_t, 8> blob{};
        for (unsigned i = 0; i < 8; ++i) {
            blob[i] = static_cast<std::uint8_t>(id >> (8 * (7 - i)));
        }
        self.entries.emplace(std::vector<std::uint8_t>(blob.begin(), blob.end()), std::move(entry));
        if (out.size() < blob.size()) {
            return 0;
        }
        std::memcpy(out.data(), blob.data(), blob.size());
        return blob.size();
    }

    static bool lookup(void *ctx, std::span<const std::uint8_t> identity, std::string_view, std::int64_t,
                       std::span<const std::uint8_t>, fiber::tls::TlsResumedSession &out) noexcept {
        const auto &self = *static_cast<TestSessionStore *>(ctx);
        const auto it = self.entries.find(std::vector<std::uint8_t>(identity.begin(), identity.end()));
        if (it == self.entries.end()) {
            return false;
        }
        const Entry &entry = it->second;
        out.psk = entry.psk;
        out.suite = entry.suite;
        out.alpn = entry.alpn;
        out.ticket_age_add = entry.age_add;
        out.max_early_data = entry.max_early_data;
        out.quic = entry.quic;
        out.ticket_issued_ms = entry.issued_ms;
        return true;
    }

    [[nodiscard]] TlsTicketMinter minter_hook() noexcept { return {&mint, this}; }
    [[nodiscard]] TlsResumptionLookup lookup_hook() noexcept { return {&lookup, this}; }

private:
    std::map<std::vector<std::uint8_t>, Entry> entries;
    std::uint64_t next_id = 1;
};

} // namespace

// =====================================================================
// §1 — our QUIC client ↔ BoringSSL QUIC server: full handshake, all five
// export points byte-verified, 0x39 both directions, ALPN, empty
// legacy_session_id, level shape, empty record stream.
// =====================================================================

TEST(TlsQuicHandshake, OurClientDrivesBoringServer) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        QuicMaterial material;
        EngineQuicSink sink;
        auto peer = BoringQuic::make_server(BoringQuic::ServerOptions{}, kServerParams);
        ASSERT_NE(nullptr, peer);

        TlsClientHandshakeEngine engine(material.client_cfg(sink), nullptr);
        ASSERT_FALSE(engine.done());

        // The record stream never carries a byte in QUIC mode.
        const std::vector<std::uint8_t> initial = sink.level_bytes(TlsQuicLevel::Initial);
        ASSERT_FALSE(initial.empty());
        EXPECT_EQ(0, initial[38]); // empty legacy_session_id (RFC 9001 §8.4)

        ASSERT_TRUE(pump_client_engine(sink, engine, *peer));
        ASSERT_TRUE(engine.done());
        EXPECT_FALSE(engine.failed());

        // Export order + level shape.
        expect_engine_export_order(sink);
        expect_crypto_level_shape(sink);
        EXPECT_EQ(0u, sink.alerts.size());

        // No 0-RTT offered: exactly hs+app secrets, nothing at EarlyData.
        ASSERT_EQ(4u, sink.secrets.size());
        EXPECT_EQ(nullptr, sink.find_secret(TlsQuicLevel::EarlyData, true));
        EXPECT_EQ(nullptr, sink.find_secret(TlsQuicLevel::EarlyData, false));

        // Every export byte-equals BoringSSL's own capture at the same
        // (level, direction) — the RFC 9001 Appendix A KAT, subsumed.
        expect_secrets_match(sink, *peer);

        // 0x39 both directions: the server got ours, we got its params.
        const uint8_t *server_params = nullptr;
        size_t server_params_len = 0;
        SSL_get_peer_quic_transport_params(peer->ssl, &server_params, &server_params_len);
        ASSERT_EQ(kClientParams.size(), server_params_len);
        EXPECT_EQ(0, std::memcmp(kClientParams.data(), server_params, server_params_len));
        ASSERT_EQ(1u, sink.peer_params.size());
        EXPECT_TRUE(secrets_equal({kServerParams.begin(), kServerParams.end()}, sink.peer_params[0]));

        // The second flight is exactly one Finished (4-byte header +
        // hash-length verify_data; no EoED in QUIC, no client certificate
        // requested).
        ASSERT_NE(nullptr, sink.find_secret(TlsQuicLevel::Handshake, true));
        EXPECT_EQ(finished_size(sink.find_secret(TlsQuicLevel::Handshake, true)->suite),
                  sink.level_bytes(TlsQuicLevel::Handshake).size());

        // HandshakeDone tail delivery.
        const TlsQuicHandshakeResult result = engine.take_quic_result();
        EXPECT_EQ(32u, result.resumption_master.len());
        ASSERT_EQ(2u, result.alpn_len);
        EXPECT_EQ(0, std::memcmp("h2", result.alpn.data(), 2));
        EXPECT_FALSE(result.session_resumed);
        EXPECT_FALSE(result.early_data_accepted);
        EXPECT_EQ(2u, result.peer_chain.size());

        // The BoringSSL server emitted its post-handshake NST (1-RTT CRYPTO);
        // consuming it is slice 2's post-handshake consumer, not the engine's.
        EXPECT_TRUE(peer->finished());
    });
}

// The HRR mirror: a BoringSSL server that only accepts P-256 against our
// X25519 first share — CH1 at Initial, HRR, CH2 still at Initial, flight at
// Handshake. (Initial is the only level that may legally repeat.)
TEST(TlsQuicHandshake, OurClientBoringServerHrr) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        QuicMaterial material;
        EngineQuicSink sink;
        auto peer = BoringQuic::make_server(BoringQuic::ServerOptions{.groups = "P-256"}, kServerParams);
        ASSERT_NE(nullptr, peer);

        TlsClientHandshakeEngine engine(material.client_cfg(sink), nullptr);
        ASSERT_TRUE(pump_client_engine(sink, engine, *peer));
        EXPECT_FALSE(engine.failed());
        expect_secrets_match(sink, *peer);

        // Two ClientHellos, both at Initial; everything else at Handshake.
        std::size_t initial_messages = 0;
        for (const EngineQuicSink::DataRec &rec: sink.all) {
            if (rec.level == TlsQuicLevel::Initial && rec.bytes[0] == 0x01) {
                ++initial_messages;
            }
        }
        EXPECT_EQ(2u, initial_messages);
        const TlsQuicHandshakeResult result = engine.take_quic_result();
        EXPECT_EQ(2u, result.peer_chain.size());
    });
}

// =====================================================================
// §2 — our QUIC server ↔ BoringSSL QUIC client: mirror matrix + the NST
// minted with the 0xffffffff QUIC sentinel, parsed by a real client.
// =====================================================================

TEST(TlsQuicHandshake, OurServerDrivesBoringClient) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        QuicMaterial material;
        EngineQuicSink sink;
        TestSessionStore store;
        const TlsTicketMinter minter = store.minter_hook();
        auto peer = BoringQuic::make_client(BoringQuic::ClientOptions{}, kClientParams);
        ASSERT_NE(nullptr, peer);

        TlsServerConfig cfg = material.server_cfg(sink);
        cfg.enable_early_data = true; // the minted ticket carries the QUIC sentinel
        TlsServerHandshakeEngine engine(cfg, nullptr, &minter);
        EXPECT_FALSE(engine.done());

        ASSERT_TRUE(pump_server_engine(sink, engine, *peer));
        ASSERT_TRUE(engine.done());
        EXPECT_FALSE(engine.failed());

        expect_engine_export_order(sink);
        EXPECT_EQ(0u, sink.alerts.size());

        // No early data offered by a plain client: hs + app secrets only.
        ASSERT_EQ(4u, sink.secrets.size());
        expect_secrets_match(sink, *peer);

        // 0x39 both directions: the boring client saw OUR server's params,
        // and our engine captured the client's.
        const uint8_t *seen_by_client = nullptr;
        size_t seen_by_client_len = 0;
        SSL_get_peer_quic_transport_params(peer->ssl, &seen_by_client, &seen_by_client_len);
        ASSERT_EQ(kServerParams.size(), seen_by_client_len);
        EXPECT_EQ(0, std::memcmp(kServerParams.data(), seen_by_client, seen_by_client_len));
        ASSERT_EQ(1u, sink.peer_params.size());
        EXPECT_TRUE(secrets_equal({kClientParams.begin(), kClientParams.end()}, sink.peer_params[0]));

        // Server flight at Handshake, NST after the client's Fin at
        // Application (1-RTT CRYPTO) — the drive delivered and BoringSSL's
        // post-handshake path consumed it.
        EXPECT_FALSE(sink.level_bytes(TlsQuicLevel::Handshake).empty());
        EXPECT_FALSE(sink.level_bytes(TlsQuicLevel::Application).empty());

        const TlsQuicHandshakeResult result = engine.take_quic_result();
        EXPECT_EQ(32u, result.resumption_master.len());
        ASSERT_EQ(2u, result.alpn_len); // record_alpn fills the QUIC result in this mode
        EXPECT_EQ(0, std::memcmp("h2", result.alpn.data(), 2));
        EXPECT_FALSE(result.session_resumed);
        EXPECT_FALSE(result.early_data_accepted);
        EXPECT_TRUE(result.peer_chain.empty()); // no client certificate

        // The minted ticket carries the QUIC sentinel, not a byte budget.
        const TestSessionStore::Entry *entry = store.first_entry();
        ASSERT_NE(nullptr, entry);
        EXPECT_EQ(0xffffffffu, entry->max_early_data);
        EXPECT_EQ("h2", entry->alpn);
    });
}

// HRR from our side: a BoringSSL client whose first share is P-256 while it
// supports X25519 ("P-256:X25519"); the server prefers X25519 → HRR at
// Initial, flight at Handshake, secrets match.
TEST(TlsQuicHandshake, OurServerBoringClientHrr) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        QuicMaterial material;
        EngineQuicSink sink;
        auto peer = BoringQuic::make_client(BoringQuic::ClientOptions{.groups = "P-256:X25519"}, kClientParams);
        ASSERT_NE(nullptr, peer);

        TlsServerHandshakeEngine engine(material.server_cfg(sink), nullptr, nullptr);
        ASSERT_TRUE(pump_server_engine(sink, engine, *peer));
        EXPECT_FALSE(engine.failed());
        expect_secrets_match(sink, *peer);

        // The SH rides Initial CRYPTO in every real stack (BoringSSL's
        // tls_set_write_state flushes pending messages at the OLD level
        // before advancing; quic-go/ngtcp2 read the SH from the Initial
        // crypto stream) — so an HRR exchange leaves TWO ServerHello-typed
        // Initial records: the HRR and the real SH. Everything past the SH
        // is Handshake-level.
        std::size_t initial_records = 0;
        for (const EngineQuicSink::DataRec &rec: sink.all) {
            if (rec.level == TlsQuicLevel::Initial) {
                ++initial_records;
                EXPECT_EQ(0x02, rec.bytes[0]);
            }
        }
        EXPECT_EQ(2u, initial_records);
        EXPECT_FALSE(sink.level_bytes(TlsQuicLevel::Handshake).empty());
    });
}

// =====================================================================
// §3 — QUIC-only rejection rules on inbound ClientHellos
// =====================================================================

// §8.4: a compat-mode legacy_session_id is illegal_parameter.
TEST(TlsQuicHandshake, ServerRejectsCompatSessionId) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        QuicMaterial material;
        EngineQuicSink client_sink;
        TlsClientHandshakeEngine client(material.client_cfg(client_sink), nullptr);
        const std::vector<std::uint8_t> ch = with_session_id(take_client_hello(client_sink));
        ASSERT_FALSE(ch.empty());

        EngineQuicSink server_sink;
        TlsServerHandshakeEngine server(material.server_cfg(server_sink), nullptr, nullptr);
        const auto event = server.feed_quic(TlsQuicLevel::Initial, ch);
        ASSERT_TRUE(event.has_value());
        EXPECT_EQ(Event::Failed, *event);
        EXPECT_TRUE(server.failed());
        ASSERT_EQ(1u, server_sink.alerts.size());
        EXPECT_EQ(TlsAlertDesc::IllegalParameter, server_sink.alerts[0]);
        EXPECT_TRUE(server_sink.out.empty()); // nothing was emitted
    });
}

// §8.1: a ClientHello without ALPN is no_application_protocol.
TEST(TlsQuicHandshake, ServerRequiresAlpn) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        QuicMaterial material;
        EngineQuicSink client_sink;
        TlsClientHandshakeEngine client(material.client_cfg(client_sink), nullptr);
        std::vector<std::uint8_t> ch = take_client_hello(client_sink);
        ASSERT_FALSE(ch.empty());
        ASSERT_TRUE(strip_alpn(ch));

        EngineQuicSink server_sink;
        TlsServerHandshakeEngine server(material.server_cfg(server_sink), nullptr, nullptr);
        const auto event = server.feed_quic(TlsQuicLevel::Initial, ch);
        ASSERT_TRUE(event.has_value());
        EXPECT_EQ(Event::Failed, *event);
        ASSERT_EQ(1u, server_sink.alerts.size());
        EXPECT_EQ(TlsAlertDesc::NoApplicationProtocol, server_sink.alerts[0]);
    });
}

// The client side of the same rule: a QUIC config without ALPN is a
// configuration bug, answered at construction.
TEST(TlsQuicHandshake, ClientRequiresAlpnConfig) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        QuicMaterial material;
        EngineQuicSink sink;
        TlsClientConfig cfg = material.client_cfg(sink);
        cfg.alpn = {};
        TlsClientHandshakeEngine engine(cfg, nullptr);
        EXPECT_TRUE(engine.done());
        EXPECT_TRUE(engine.failed());
        EXPECT_EQ(TlsAlertDesc::InternalError, engine.failure_alert());
        EXPECT_TRUE(sink.out.empty());
    });
}

// =====================================================================
// §4 — inbound plumbing: garbage, cap, partial, level ping, leftover
// =====================================================================

TEST(TlsQuicHandshake, GarbageFirstMessageAlerts) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        QuicMaterial material;
        EngineQuicSink sink;
        TlsServerHandshakeEngine engine(material.server_cfg(sink), nullptr, nullptr);
        const std::array<std::uint8_t, 16> garbage{0x99, 0x00, 0x00, 0x0C, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        const auto event = engine.feed_quic(TlsQuicLevel::Initial, garbage);
        ASSERT_TRUE(event.has_value());
        EXPECT_EQ(Event::Failed, *event);
        ASSERT_EQ(1u, sink.alerts.size());
        EXPECT_EQ(TlsAlertDesc::UnexpectedMessage, sink.alerts[0]);
        EXPECT_TRUE(sink.out.empty()); // alerts are callback-only, never CRYPTO bytes
    });
}

TEST(TlsQuicHandshake, MessageCapPartialAndPing) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        QuicMaterial material;
        {
            EngineQuicSink sink;
            TlsServerHandshakeEngine engine(material.server_cfg(sink), nullptr, nullptr);
            // Declared body_len 0x400001 (> the ClientHello reassembly cap): fatal.
            const std::array<std::uint8_t, 5> oversize{0x01, 0x40, 0x00, 0x01, 0xFF};
            const auto event = engine.feed_quic(TlsQuicLevel::Initial, oversize);
            ASSERT_TRUE(event.has_value());
            EXPECT_EQ(Event::Failed, *event);
            ASSERT_EQ(1u, sink.alerts.size());
            EXPECT_EQ(TlsAlertDesc::DecodeError, sink.alerts[0]);
        }
        {
            // A truncated header/body is NeedMore — engine keeps waiting.
            EngineQuicSink sink;
            TlsServerHandshakeEngine engine(material.server_cfg(sink), nullptr, nullptr);
            const std::array<std::uint8_t, 3> partial{0x01, 0x00, 0x02};
            const auto event = engine.feed_quic(TlsQuicLevel::Initial, partial);
            ASSERT_TRUE(event.has_value());
            EXPECT_EQ(Event::None, *event);
            EXPECT_FALSE(engine.done());
            EXPECT_TRUE(sink.alerts.empty());
        }
        {
            // Empty span at the current level is a legal level ping.
            EngineQuicSink sink;
            TlsServerHandshakeEngine engine(material.server_cfg(sink), nullptr, nullptr);
            const auto event = engine.feed_quic(TlsQuicLevel::Initial, {});
            ASSERT_TRUE(event.has_value());
            EXPECT_EQ(Event::None, *event);
            EXPECT_FALSE(engine.done());
        }
    });
}

// The engine stops stepping at HandshakeDone: bytes trailing the terminal
// message inside one provide surface via take_inbound_leftover (the QUIC
// layer routes them to the post-handshake consumer).
TEST(TlsQuicHandshake, InboundLeftoverAfterDone) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        QuicMaterial material;
        // Pass 1 establishes the exchange shape is fully drivable.
        {
            EngineQuicSink cs;
            EngineQuicSink ss;
            TlsServerHandshakeEngine s(material.server_cfg(ss), nullptr, nullptr);
            TlsClientHandshakeEngine c(material.client_cfg(cs), nullptr);
            ASSERT_TRUE(pump_self(cs, c, ss, s));
        }
        // Pass 2 replays it with a tail glued after the client's terminal
        // Handshake record (the client emits exactly one — its Finished).
        EngineQuicSink client_sink;
        EngineQuicSink server_sink;
        TlsServerHandshakeEngine server(material.server_cfg(server_sink), nullptr, nullptr);
        TlsClientHandshakeEngine client(material.client_cfg(client_sink), nullptr);

        const std::string_view tail = "TAIL-BYTES";
        bool fed_tail = false;
        std::size_t handshake_records = 0;
        for (int spin = 0; spin < 64 && !(server.done() && client.done()); ++spin) {
            for (const auto &rec: client_sink.drain_out()) {
                if (server.done()) {
                    continue;
                }
                if (rec.level == TlsQuicLevel::Handshake) {
                    ++handshake_records;
                    std::vector<std::uint8_t> bytes = rec.bytes;
                    bytes.insert(bytes.end(), tail.begin(), tail.end());
                    fed_tail = true;
                    ASSERT_TRUE(server.feed_quic(rec.level, bytes).has_value());
                } else {
                    ASSERT_TRUE(server.feed_quic(rec.level, rec.bytes).has_value());
                }
            }
            for (const auto &rec: server_sink.drain_out()) {
                if (!client.done()) {
                    ASSERT_TRUE(client.feed_quic(rec.level, rec.bytes).has_value());
                }
            }
            ASSERT_FALSE(server.failed());
            ASSERT_FALSE(client.failed());
        }
        ASSERT_TRUE(server.done());
        ASSERT_TRUE(client.done());
        ASSERT_TRUE(fed_tail);
        ASSERT_EQ(1u, handshake_records); // the tail sat after the final message
        const std::vector<std::uint8_t> leftover = [&] {
            fiber::mem::IoBufChain chain = server.take_inbound_leftover();
            std::vector<std::uint8_t> out;
            out.reserve(chain.readable_bytes());
            for (const fiber::mem::IoBufNode *node = chain.front_node(); node != nullptr; node = node->next) {
                out.insert(out.end(), node->buf.readable_data(), node->buf.readable_data() + node->buf.readable());
            }
            return out;
        }();
        ASSERT_EQ(tail.size(), leftover.size());
        EXPECT_EQ(0, std::memcmp(tail.data(), leftover.data(), tail.size()));
    });
}

// =====================================================================
// §5 — 0-RTT
// =====================================================================

// A crafted PSK offer the BoringSSL server cannot resolve: the client's
// early write secret exports at construction, the server rejects the
// ticket, no early secret exists server-side, and — RFC 9001 §8.3 — no
// EndOfEarlyData appears in the second flight.
TEST(TlsQuicHandshake, EarlyDataOfferRejectedByBoringServer) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        QuicMaterial material;
        EngineQuicSink sink;
        auto peer = BoringQuic::make_server(BoringQuic::ServerOptions{}, kServerParams);
        ASSERT_NE(nullptr, peer);

        std::array<std::uint8_t, 32> identity{};
        for (std::size_t i = 0; i < identity.size(); ++i) {
            identity[i] = static_cast<std::uint8_t>(i);
        }
        const std::vector<std::uint8_t> psk(32, 0x5E);
        TlsSessionOffer offer;
        offer.identity = identity;
        offer.suite = TlsCipherSuiteId::TlsAes128GcmSha256;
        offer.psk = psk;
        offer.max_early_data = 0xffffffff; // the QUIC sentinel (§4.6.1)

        TlsClientHandshakeEngine engine(material.client_cfg(sink), &offer);
        // The early write secret is out before any server bytes exist.
        ASSERT_NE(nullptr, sink.find_secret(TlsQuicLevel::EarlyData, true));

        ASSERT_TRUE(pump_client_engine(sink, engine, *peer));
        EXPECT_FALSE(engine.failed());

        // Full handshake: hs+app secrets match; the server never saw early data.
        expect_secrets_match(sink, *peer);
        EXPECT_EQ(nullptr, peer->find_secret(static_cast<int>(TlsQuicLevel::EarlyData), false));
        EXPECT_EQ(nullptr, peer->find_secret(static_cast<int>(TlsQuicLevel::EarlyData), true));

        // §8.3: the second flight is exactly the Finished — no EoED.
        ASSERT_NE(nullptr, sink.find_secret(TlsQuicLevel::Handshake, true));
        EXPECT_EQ(finished_size(sink.find_secret(TlsQuicLevel::Handshake, true)->suite),
                  sink.level_bytes(TlsQuicLevel::Handshake).size());

        const TlsQuicHandshakeResult result = engine.take_quic_result();
        EXPECT_FALSE(result.session_resumed);
        EXPECT_FALSE(result.early_data_accepted);
        EXPECT_EQ(2u, result.peer_chain.size());
    });
}

// Both FSMs ours with 0-RTT accepted: the client exports the early write
// secret, the server the early read secret, both handshake secrets and both
// application secrets agree, and — §8.3 — no EoED anywhere: the client's
// Handshake-level flight is exactly one Finished, which the server verifies
// over an EoED-free transcript.
TEST(TlsQuicHandshake, SelfInteropEarlyDataAccepted) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        QuicMaterial material;
        TestSessionStore store;
        const TlsTicketMinter minter = store.minter_hook();
        const TlsResumptionLookup lookup = store.lookup_hook();

        // hop 1: mint a 0-RTT-capable ticket (sentinel value).
        {
            EngineQuicSink hop1_client_sink;
            EngineQuicSink hop1_server_sink;
            TlsServerConfig cfg = material.server_cfg(hop1_server_sink);
            cfg.enable_early_data = true;
            TlsServerHandshakeEngine server(cfg, nullptr, &minter);
            TlsClientHandshakeEngine client(material.client_cfg(hop1_client_sink), nullptr);
            ASSERT_TRUE(pump_self(hop1_client_sink, client, hop1_server_sink, server));
        }
        const TestSessionStore::Entry *entry = store.first_entry();
        ASSERT_NE(nullptr, entry);
        EXPECT_EQ(0xffffffffu, entry->max_early_data); // the QUIC sentinel

        // hop 2: offer the ticket with early data.
        EngineQuicSink client_sink;
        EngineQuicSink server_sink;
        TlsSessionOffer offer;
        const std::vector<std::uint8_t> ticket = store.first_ticket();
        offer.identity = ticket;
        offer.obfuscated_ticket_age = entry->age_add;
        offer.suite = entry->suite;
        offer.psk = entry->psk;
        offer.max_early_data = entry->max_early_data;

        TlsServerConfig cfg = material.server_cfg(server_sink);
        cfg.enable_early_data = true;
        TlsServerHandshakeEngine server(cfg, &lookup, &minter);
        TlsClientHandshakeEngine client(material.client_cfg(client_sink), &offer);

        ASSERT_TRUE(pump_self(client_sink, client, server_sink, server));
        EXPECT_FALSE(client.failed());
        EXPECT_FALSE(server.failed());

        // Early data accepted: the server exported the early READ secret,
        // and it equals the client's early WRITE secret.
        const EngineQuicSink::SecretRec *client_early = client_sink.find_secret(TlsQuicLevel::EarlyData, true);
        const EngineQuicSink::SecretRec *server_early = server_sink.find_secret(TlsQuicLevel::EarlyData, false);
        ASSERT_NE(nullptr, client_early);
        ASSERT_NE(nullptr, server_early);
        EXPECT_TRUE(secrets_equal(client_early->bytes, server_early->bytes));
        EXPECT_EQ(nullptr, server_sink.find_secret(TlsQuicLevel::EarlyData, true));
        EXPECT_EQ(nullptr, client_sink.find_secret(TlsQuicLevel::EarlyData, false));

        // The later levels agree across the two engines with DIRECTIONS
        // FLIPPED — client-write and server-read both derive from
        // client_X_traffic_secret and vice versa (the same pairing the
        // BoringSSL interop asserts).
        for (const TlsQuicLevel level: {TlsQuicLevel::Handshake, TlsQuicLevel::Application}) {
            for (const bool write: {true, false}) {
                const EngineQuicSink::SecretRec *c = client_sink.find_secret(level, write);
                const EngineQuicSink::SecretRec *s = server_sink.find_secret(level, !write);
                ASSERT_NE(nullptr, c);
                ASSERT_NE(nullptr, s);
                EXPECT_TRUE(secrets_equal(c->bytes, s->bytes));
            }
        }

        // §8.3: the client's Handshake flight is exactly one Finished (no
        // EoED, no client certificate), and the server verified it.
        ASSERT_NE(nullptr, client_sink.find_secret(TlsQuicLevel::Handshake, true));
        EXPECT_EQ(finished_size(client_sink.find_secret(TlsQuicLevel::Handshake, true)->suite),
                  client_sink.level_bytes(TlsQuicLevel::Handshake).size());

        const TlsQuicHandshakeResult client_result = client.take_quic_result();
        const TlsQuicHandshakeResult server_result = server.take_quic_result();
        EXPECT_TRUE(client_result.session_resumed);
        EXPECT_TRUE(server_result.session_resumed);
        EXPECT_TRUE(client_result.early_data_accepted);
        EXPECT_TRUE(server_result.early_data_accepted);
        EXPECT_TRUE(client_result.peer_chain.empty()); // PSK resume: no certificate flight
        EXPECT_TRUE(server_result.peer_chain.empty());
        EXPECT_EQ(32u, client_result.resumption_master.len());
        EXPECT_TRUE(secrets_equal(
                {client_result.resumption_master.bytes().begin(), client_result.resumption_master.bytes().end()},
                {server_result.resumption_master.bytes().begin(), server_result.resumption_master.bytes().end()}));

        // The 0-RTT read surface yields nothing in QUIC mode: early data is
        // STREAM frames the QUIC layer decrypts with the exported secret.
        EXPECT_TRUE(fiber::mem::IoBufChain(server.take_early_data()).readable_bytes() == 0);

        expect_engine_export_order(client_sink);
        expect_engine_export_order(server_sink);
    });
}

// =====================================================================
// The 0-RTT consistency gate (10 §6.1): remembered params vs current ones
// =====================================================================

// Wraps the real stateless service at mint time — the request's scalars and
// the sealed blob are all a client needs to offer the ticket again (the PSK
// is re-derived from the hop-1 server's exported resumption master).
struct CapturingMinter {
    TlsTicketService *service = nullptr;
    std::vector<std::uint8_t> ticket;
    std::uint32_t age_add = 0;
    TlsCipherSuiteId suite = TlsCipherSuiteId::TlsAes128GcmSha256;

    static std::size_t mint(void *ctx, const TlsTicketRequest &req, std::span<std::uint8_t> out) noexcept {
        auto &self = *static_cast<CapturingMinter *>(ctx);
        const std::size_t len = TlsTicketService::mint_thunk(self.service, req, out);
        if (len != 0) {
            self.ticket.assign(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(len));
            self.age_add = req.ticket_age_add;
            self.suite = req.suite;
        }
        return len;
    }
    [[nodiscard]] TlsTicketMinter hook() noexcept { return {&mint, this}; }
};

namespace {

[[nodiscard]] std::unique_ptr<TlsTicketService> fixed_key_service() {
    TlsTicketKeyMaterial material{};
    material.id = 1;
    material.created_ms = certfix::kRefNowMs;
    for (std::size_t i = 0; i < material.bytes.size(); ++i) {
        material.bytes[i] = static_cast<std::uint8_t>(i * 7 + 1);
    }
    const std::array<TlsTicketKeyMaterial, 1> keys{{material}};
    auto service = std::make_unique<TlsTicketService>(keys, TlsTicketKeyPolicy{});
    if (!service->valid()) {
        return nullptr;
    }
    return service;
}

} // namespace

// The matrix's mismatch leg at engine level: hop 1 mints under context A,
// hop 2 offers the ticket with early data against context B. The gate
// demotes to 1-RTT resumption only — the session still resumes, no early
// secrets are installed, and the client sees early_data_accepted == false.
TEST(TlsQuicHandshake, EarlyDataContextMismatchResumesAtOneRtt) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        QuicMaterial material;
        auto service = fixed_key_service();
        ASSERT_NE(nullptr, service);
        constexpr std::array<std::uint8_t, 6> kContextA{{1, 2, 3, 4, 5, 6}};
        constexpr std::array<std::uint8_t, 6> kContextB{{1, 2, 3, 4, 5, 9}};

        // hop 1: mint a 0-RTT-capable ticket bound to context A.
        TlsSecret master{};
        CapturingMinter capturing{service.get()};
        const TlsTicketMinter minter = capturing.hook();
        {
            EngineQuicSink client_sink;
            EngineQuicSink server_sink;
            TlsServerConfig cfg = material.server_cfg(server_sink);
            cfg.enable_early_data = true;
            cfg.quic_early_data_context = kContextA;
            TlsServerHandshakeEngine server(cfg, nullptr, &minter);
            TlsClientHandshakeEngine client(material.client_cfg(client_sink), nullptr);
            ASSERT_TRUE(pump_self(client_sink, client, server_sink, server));
            ASSERT_TRUE(server.done());
            master = TlsSecret::from_bytes(server.take_quic_result().resumption_master.bytes());
        }
        ASSERT_FALSE(capturing.ticket.empty());

        // The client-side receipt derivation: PSK = f(resumption_master, 0).
        const std::array<std::uint8_t, 1> nonce{0};
        const auto psk = fiber::tls::tls13_resumption_psk(master, nonce);
        ASSERT_TRUE(psk.has_value());

        // hop 2: offer the ticket with early data against context B.
        EngineQuicSink client_sink;
        EngineQuicSink server_sink;
        TlsSessionOffer offer;
        offer.identity = capturing.ticket;
        offer.obfuscated_ticket_age = capturing.age_add; // zero elapsed age
        offer.suite = capturing.suite;
        offer.psk = psk->bytes();
        offer.max_early_data = 0xffffffff;

        const TlsResumptionLookup lookup = service->lookup();
        TlsServerConfig cfg = material.server_cfg(server_sink);
        cfg.enable_early_data = true;
        cfg.quic_early_data_context = kContextB; // the server's params changed
        TlsServerHandshakeEngine server(cfg, &lookup, &minter);
        TlsClientHandshakeEngine client(material.client_cfg(client_sink), &offer);

        ASSERT_TRUE(pump_self(client_sink, client, server_sink, server));
        EXPECT_FALSE(client.failed());
        EXPECT_FALSE(server.failed());

        // 0-RTT vetoed: the server never installs the early read secret.
        EXPECT_EQ(nullptr, server_sink.find_secret(TlsQuicLevel::EarlyData, false));
        // ...but the session resumed at 1-RTT (not a full handshake).
        const TlsQuicHandshakeResult result = client.take_quic_result();
        EXPECT_TRUE(result.session_resumed);
        EXPECT_FALSE(result.early_data_accepted);

        // The 1-RTT legs still agree across the engines.
        for (const TlsQuicLevel level: {TlsQuicLevel::Handshake, TlsQuicLevel::Application}) {
            const auto *client_write = client_sink.find_secret(level, true);
            const auto *server_read = server_sink.find_secret(level, false);
            ASSERT_NE(nullptr, client_write);
            ASSERT_NE(nullptr, server_read);
            EXPECT_TRUE(secrets_equal(client_write->bytes, server_read->bytes));
        }
    });
}

// The face gate's miss leg: a ticket relabeled as TCP-minted (the cross-face
// offer) is a plain miss — the handshake runs FULL (session_resumed false),
// never a fatal error.
TEST(TlsQuicHandshake, CrossFaceTicketFallsBackToFullHandshake) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        QuicMaterial material;
        TestSessionStore store;
        const TlsTicketMinter minter = store.minter_hook();
        const TlsResumptionLookup lookup = store.lookup_hook();

        // hop 1: mint a 0-RTT-capable ticket on the QUIC face.
        {
            EngineQuicSink client_sink;
            EngineQuicSink server_sink;
            TlsServerConfig cfg = material.server_cfg(server_sink);
            cfg.enable_early_data = true;
            TlsServerHandshakeEngine server(cfg, nullptr, &minter);
            TlsClientHandshakeEngine client(material.client_cfg(client_sink), nullptr);
            ASSERT_TRUE(pump_self(client_sink, client, server_sink, server));
        }
        const TestSessionStore::Entry *entry = store.first_entry();
        ASSERT_NE(nullptr, entry);
        store.flip_first_face(); // the stored ticket now claims the TCP face

        // hop 2: offer with early data — the face gate must treat it as a miss.
        EngineQuicSink client_sink;
        EngineQuicSink server_sink;
        TlsSessionOffer offer;
        const std::vector<std::uint8_t> ticket = store.first_ticket();
        offer.identity = ticket;
        offer.obfuscated_ticket_age = entry->age_add;
        offer.suite = entry->suite;
        offer.psk = entry->psk;
        offer.max_early_data = entry->max_early_data;

        TlsServerConfig cfg = material.server_cfg(server_sink);
        cfg.enable_early_data = true;
        TlsServerHandshakeEngine server(cfg, &lookup, &minter);
        TlsClientHandshakeEngine client(material.client_cfg(client_sink), &offer);

        ASSERT_TRUE(pump_self(client_sink, client, server_sink, server));
        EXPECT_FALSE(client.failed());
        EXPECT_FALSE(server.failed());

        EXPECT_EQ(nullptr, server_sink.find_secret(TlsQuicLevel::EarlyData, false));
        const TlsQuicHandshakeResult result = client.take_quic_result();
        EXPECT_FALSE(result.session_resumed); // a full handshake, not a resume
        EXPECT_FALSE(result.early_data_accepted);

        for (const TlsQuicLevel level: {TlsQuicLevel::Handshake, TlsQuicLevel::Application}) {
            const auto *client_write = client_sink.find_secret(level, true);
            const auto *server_read = server_sink.find_secret(level, false);
            ASSERT_NE(nullptr, client_write);
            ASSERT_NE(nullptr, server_read);
            EXPECT_TRUE(secrets_equal(client_write->bytes, server_read->bytes));
        }
    });
}
