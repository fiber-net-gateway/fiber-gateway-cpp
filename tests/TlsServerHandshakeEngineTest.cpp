// TLS server handshake engine against a real BoringSSL client peer
// (feature/tls/07 §8): the 1.3 full handshake (P1–P3, incl. HRR and mTLS)
// and the 1.2 full handshake (P4, incl. the RFC 5077 NST mint).
// The peer is an SSL client object over memory BIOs; the drive loop ships
// engine output into the client's read BIO and feeds the client's write BIO
// back, in whole-flight or 1-byte slices. Post-handshake app-data round
// trips exercise the moved TlsConnectedState ciphers against the same
// BoringSSL session, and a second driver runs OUR 06 client engine against
// this server engine (both FSMs ours, no BoringSSL in the loop).

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

#include <fiber/common/IoError.h>
#include <fiber/common/mem/IoBuf.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/tls/TlsConfig.h>
#include <fiber/tls/TlsConnectedState.h>
#include <fiber/tls/TlsTicketService.h>
#include <fiber/tls/crypto/Tls13KeySchedule.h>
#include <fiber/tls/crypto/TlsKeyExchange.h>
#include <fiber/tls/crypto/TlsSignature.h>
#include <fiber/tls/handshake/TlsClientHandshakeEngine.h>
#include <fiber/tls/handshake/TlsHandshakeCodec.h>
#include <fiber/tls/handshake/TlsHandshakeMessage.h>
#include <fiber/tls/handshake/TlsServerHandshakeEngine.h>
#include <fiber/tls/record/TlsRecord.h>
#include "LoopTestSupport.h"

namespace {

namespace certfix = fiber::tls::certfix;

using fiber::common::IoResult;
using fiber::mem::IoBuf;
using fiber::mem::IoBufChain;
using fiber::mem::IoBufNodePool;
using fiber::tls::TlsAlertDesc;
using fiber::tls::TlsCertificateChain;
using fiber::tls::TlsCipherSuiteId;
using fiber::tls::TlsClientConfig;
using fiber::tls::TlsClientHandshakeEngine;
using fiber::tls::TlsConnectedState;
using fiber::tls::TlsPrivateKey;
using fiber::tls::TlsRecordCipher;
using fiber::tls::TlsResumedSession;
using fiber::tls::TlsResumptionLookup;
using fiber::tls::TlsSecret;
using fiber::tls::TlsServerConfig;
using fiber::tls::TlsServerHandshakeEngine;
using fiber::tls::TlsSessionOffer;
using fiber::tls::TlsTicketContents;
using fiber::tls::TlsTicketKeyMaterial;
using fiber::tls::TlsTicketKeyPolicy;
using fiber::tls::TlsTicketMinter;
using fiber::tls::TlsTicketRequest;
using fiber::tls::TlsTicketService;
using fiber::tls::TlsTrustStore;
using ClientEvent = TlsClientHandshakeEngine::Event;
using Event = TlsServerHandshakeEngine::Event;

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

// ---- BoringClient: an SSL client over memory BIOs ----

// Sessions the BoringSSL client collected from NewSessionTickets. With
// SSL_SESS_CACHE_NO_INTERNAL_STORE the new-session callback owns them; every
// collecting test scopes a guard that frees what it gathered.
std::vector<SSL_SESSION *> g_new_sessions;

int stash_new_session(SSL *, SSL_SESSION *sess) noexcept {
    g_new_sessions.push_back(sess);
    return 1; // the stash took ownership
}

void free_collected_sessions() noexcept {
    for (SSL_SESSION *sess: g_new_sessions) {
        SSL_SESSION_free(sess);
    }
    g_new_sessions.clear();
}

struct CollectedSessionsGuard {
    ~CollectedSessionsGuard() { free_collected_sessions(); }
};

struct ClientOptions {
    const char *trust_pem = certfix::kRootRsaPem; // null = no verification
    const char *host = "example.com";
    bool offer_alpn = true;
    bool tls12_only = false; // pin the client to TLS 1.2
    const char *tls12_ciphers = nullptr; // non-null: pin the 1.2 cipher list (suite pin)
    const char *groups = nullptr; // non-null: restrict/reorder groups (HRR trigger)
    bool client_cert = false; // load the kClientRsaPem credential (mTLS)
    bool collect_tickets = false; // stash sessions the NSTs carry (PSK/0-RTT tests)
    bool send_sni = false; // put opt.host on the wire as SNI (ticket-name-binding tests)
};

class BoringClient {
public:
    static std::unique_ptr<BoringClient> make(const ClientOptions &opt) {
        SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
        if (ctx == nullptr) {
            return nullptr;
        }
        if (opt.tls12_only) {
            SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
            SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION);
        } else {
            SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
            SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
        }
        if (opt.trust_pem != nullptr) {
            X509 *root = load_cert(opt.trust_pem);
            if (root == nullptr || X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx), root) != 1) {
                SSL_CTX_free(ctx);
                return nullptr;
            }
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        } else {
            SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
        }

        if (opt.collect_tickets) {
            SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
            SSL_CTX_sess_set_new_cb(ctx, &stash_new_session);
        }
        std::unique_ptr<BoringClient> client(new BoringClient(ctx));
        client->ssl_ = SSL_new(ctx);
        if (client->ssl_ == nullptr) {
            return nullptr;
        }
        SSL_set_connect_state(client->ssl_);
        client->rbio_ = BIO_new(BIO_s_mem());
        client->wbio_ = BIO_new(BIO_s_mem());
        SSL_set_bio(client->ssl_, client->rbio_, client->wbio_);
        if (opt.trust_pem != nullptr && SSL_set1_host(client->ssl_, opt.host) != 1) {
            return nullptr;
        }
        if (opt.send_sni && SSL_set_tlsext_host_name(client->ssl_, opt.host) != 1) {
            return nullptr;
        }
        if (opt.groups != nullptr && SSL_set1_groups_list(client->ssl_, opt.groups) != 1) {
            return nullptr;
        }
        if (opt.tls12_ciphers != nullptr && SSL_CTX_set_cipher_list(ctx, opt.tls12_ciphers) != 1) {
            return nullptr;
        }
        if (opt.client_cert) {
            // The same credential the 06 client tests use as mTLS material.
            X509 *leaf = load_cert(certfix::kClientRsaPem);
            X509 *intermediate = load_cert(certfix::kIntermediateRsaPem);
            EVP_PKEY *key = load_key(certfix::kRsa2048KeyPem);
            if (leaf == nullptr || intermediate == nullptr || key == nullptr ||
                SSL_use_certificate(client->ssl_, leaf) != 1 || SSL_add0_chain_cert(client->ssl_, intermediate) != 1 ||
                SSL_use_PrivateKey(client->ssl_, key) != 1 || SSL_check_private_key(client->ssl_) != 1) {
                return nullptr;
            }
        }
        if (opt.offer_alpn) {
            // "h2", "http/1.1" — the same list the 06 client offers. Split
            // literals: \x02h would greedily parse as one hex escape ('h' is
            // a hex digit).
            static const unsigned char kProtos[] = "\x02"
                                                   "h2"
                                                   "\x08"
                                                   "http/1.1";
            if (SSL_set_alpn_protos(client->ssl_, kProtos, sizeof(kProtos) - 1) != 0) {
                return nullptr;
            }
        }
        return client;
    }

    ~BoringClient() {
        if (ssl_ != nullptr) {
            SSL_free(ssl_); // frees the BIOs
        }
        if (ctx_ != nullptr) {
            SSL_CTX_free(ctx_);
        }
    }

    [[nodiscard]] SSL *ssl() const noexcept { return ssl_; }

    bool ship(std::span<const std::uint8_t> bytes) {
        std::size_t off = 0;
        while (off < bytes.size()) {
            const int wrote = BIO_write(rbio_, bytes.data() + off, static_cast<int>(bytes.size() - off));
            if (wrote <= 0) {
                return false;
            }
            off += static_cast<std::size_t>(wrote);
        }
        return true;
    }

    [[nodiscard]] std::vector<std::uint8_t> drain_wbio() {
        std::vector<std::uint8_t> out;
        for (;;) {
            const int pending = static_cast<int>(BIO_pending(wbio_));
            if (pending <= 0) {
                return out;
            }
            const std::size_t old = out.size();
            out.resize(old + static_cast<std::size_t>(pending));
            const int read = BIO_read(wbio_, out.data() + old, pending);
            if (read <= 0) {
                out.resize(old);
                return out;
            }
            out.resize(old + static_cast<std::size_t>(read));
        }
    }

    // One non-blocking handshake step: 1 = complete, 0 = progress expected,
    // -1 = the client itself failed.
    int handshake_step() {
        const int rc = SSL_do_handshake(ssl_);
        if (rc == 1) {
            return 1;
        }
        const int err = SSL_get_error(ssl_, rc);
        return (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) ? 0 : -1;
    }

private:
    explicit BoringClient(SSL_CTX *ctx) noexcept : ctx_(ctx) {}

    SSL_CTX *ctx_ = nullptr;
    SSL *ssl_ = nullptr;
    BIO *rbio_ = nullptr;
    BIO *wbio_ = nullptr;
};

// ---- engine-side helpers ----

std::vector<std::uint8_t> chain_bytes(IoBufChain chain) {
    std::vector<std::uint8_t> out;
    out.reserve(chain.readable_bytes());
    for (const fiber::mem::IoBufNode *node = chain.front_node(); node != nullptr; node = node->next) {
        const std::uint8_t *data = node->buf.readable_data();
        out.insert(out.end(), data, data + node->buf.readable());
    }
    return out;
}

// Feeds `bytes` (whole or one byte at a time); false = engine-level failure.
bool feed_bytes(TlsServerHandshakeEngine &engine, std::span<const std::uint8_t> bytes, bool sliced, Event &last) {
    last = Event::None;
    std::size_t off = 0;
    while (off < bytes.size() && !engine.done()) {
        const std::size_t chunk = sliced ? 1 : bytes.size() - off;
        IoBuf buf = IoBuf::allocate(chunk);
        if (!buf.valid()) {
            EXPECT_TRUE(false);
            return false;
        }
        std::memcpy(buf.writable_data(), bytes.data() + off, chunk);
        buf.commit(chunk);
        const IoResult<Event> event = engine.feed(std::move(buf));
        if (!event.has_value()) {
            EXPECT_TRUE(false);
            return false;
        }
        last = *event;
        off += chunk;
    }
    return true;
}

// Bi-directional pump until the engine reaches its terminal state. Captures
// the server→client wire (HRR sentinel search).
struct DriveLog {
    std::vector<std::uint8_t> server_to_client;
};

bool drive(BoringClient &client, TlsServerHandshakeEngine &engine, bool sliced, DriveLog *log = nullptr) {
    for (int spin = 0; spin < 256; ++spin) {
        const std::vector<std::uint8_t> out = chain_bytes(engine.take_output());
        if (!out.empty()) {
            if (log != nullptr) {
                log->server_to_client.insert(log->server_to_client.end(), out.begin(), out.end());
            }
            if (!client.ship(out)) {
                EXPECT_TRUE(false);
                return false;
            }
        }
        if (engine.done()) {
            return !engine.failed();
        }
        if (!static_cast<bool>(SSL_is_init_finished(client.ssl())) && client.handshake_step() < 0) {
            // A deliberate protocol_version refusal lands here; the caller
            // distinguishes expected refusals by checking the engine.
            if (!engine.done() || !engine.failed()) {
                ADD_FAILURE() << "BoringSSL client handshake failed mid-drive";
                ERR_print_errors_fp(stderr);
            }
            return engine.done() && engine.failed();
        }
        const std::vector<std::uint8_t> flight = client.drain_wbio();
        if (flight.empty()) {
            break; // no progress available; the engine decides success below
        }
        Event event = Event::None;
        if (!feed_bytes(engine, flight, sliced, event)) {
            return false;
        }
    }
    return engine.done() && !engine.failed();
}

bool wire_contains(const std::vector<std::uint8_t> &wire, std::span<const std::uint8_t> needle) {
    return std::search(wire.begin(), wire.end(), needle.begin(), needle.end()) != wire.end();
}

bool secrets_equal(const fiber::tls::TlsSecret &a, const fiber::tls::TlsSecret &b) noexcept {
    return a.len() == b.len() && 0 == std::memcmp(a.bytes().data(), b.bytes().data(), a.len());
}

// 5-byte record framing around a sealed payload.
std::vector<std::uint8_t> frame_record(std::uint8_t type, std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> out;
    out.reserve(fiber::tls::kTlsRecordHeaderSize + payload.size());
    out.push_back(type);
    out.push_back(0x03);
    out.push_back(0x03);
    out.push_back(static_cast<std::uint8_t>(payload.size() >> 8));
    out.push_back(static_cast<std::uint8_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// ---- shared server material ----

struct ServerMaterial {
    std::string chain_pem;
    std::optional<TlsCertificateChain> chain;
    std::optional<TlsPrivateKey> key;
    std::vector<std::string_view> alpn{"h2", "http/1.1"};

    ServerMaterial() { load(certfix::kLeafRsaPem, certfix::kRsa2048KeyPem); }

    void load(const char *leaf_pem, const char *key_pem) {
        chain_pem.assign(leaf_pem);
        chain_pem.append(certfix::kIntermediateRsaPem);
        auto parsed = TlsCertificateChain::parse_pem_bundle({chain_pem.data(), chain_pem.size()});
        ASSERT_TRUE(parsed.has_value());
        chain = std::move(parsed).value();
        auto parsed_key = TlsPrivateKey::parse_pem({key_pem, std::strlen(key_pem)});
        ASSERT_TRUE(parsed_key.has_value());
        key = std::move(parsed_key).value();
    }

    [[nodiscard]] TlsServerConfig config() const {
        TlsServerConfig cfg;
        cfg.chain = &*chain;
        cfg.key = &*key;
        cfg.alpn = alpn;
        cfg.now_unix_ms = certfix::kRefNowMs;
        return cfg;
    }
};

} // namespace

// =====================================================================
// §8.1 — full handshake against a BoringSSL client, ALPN, app-data round
// trip. The BoringSSL client's 1.3 suite choice is its built-in preference ∩
// our offer — the test asserts the engine's suite matches what the client
// actually picked (the same note as the 06 client suite tests).
// =====================================================================

TEST(TlsServerHandshake13Full, HandshakeAndAppRoundTrip) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        auto client = BoringClient::make(ClientOptions{});
        ASSERT_NE(nullptr, client);
        ServerMaterial material;
        const TlsServerConfig cfg = material.config();

        TlsServerHandshakeEngine engine(cfg, nullptr, nullptr);
        EXPECT_FALSE(engine.done());
        EXPECT_TRUE(chain_bytes(engine.take_output()).empty()); // no first flight

        ASSERT_TRUE(drive(*client, engine, false));
        ASSERT_TRUE(engine.done());
        EXPECT_FALSE(engine.failed());

        TlsConnectedState state = engine.take_state();
        EXPECT_EQ(fiber::tls::TlsProtocolVersion::Tls13, state.version);
        const SSL_CIPHER *negotiated = SSL_get_current_cipher(client->ssl());
        ASSERT_NE(nullptr, negotiated);
        EXPECT_EQ(SSL_CIPHER_get_protocol_id(negotiated), static_cast<std::uint16_t>(state.suite));
        ASSERT_EQ(2, state.alpn_len);
        EXPECT_EQ(0, std::memcmp("h2", state.alpn.data(), 2));
        EXPECT_FALSE(state.session_resumed);
        EXPECT_FALSE(state.early_data_accepted);
        EXPECT_TRUE(state.peer_chain.empty()); // no client certificate requested
        EXPECT_TRUE(chain_bytes(engine.take_early_data()).empty());
        // The 0-RTT read surface yields nothing when early data was never offered.
        EXPECT_TRUE(chain_bytes(engine.take_early_data()).empty());

        // ---- client → server app data through the moved read cipher ----
        const char kMessage[] = "ping from boringssl client";
        EXPECT_EQ(sizeof(kMessage) - 1,
                  static_cast<std::size_t>(SSL_write(client->ssl(), kMessage, static_cast<int>(sizeof(kMessage) - 1))));
        const std::vector<std::uint8_t> flight = client->drain_wbio();
        std::vector<std::uint8_t> plaintext;
        std::size_t off = 0;
        while (off + fiber::tls::kTlsRecordHeaderSize <= flight.size()) {
            const std::size_t len = (static_cast<std::size_t>(flight[off + 3]) << 8) | flight[off + 4];
            if (flight[off] != 23) {
                off += fiber::tls::kTlsRecordHeaderSize + len;
                continue;
            }
            std::array<std::uint8_t, fiber::tls::kTlsMaxPlaintextSize> dst{};
            const auto opened =
                    state.read_cipher.open(fiber::tls::TlsContentType::ApplicationData,
                                           (static_cast<std::uint16_t>(flight[off + 1]) << 8) | flight[off + 2],
                                           static_cast<std::uint16_t>(len),
                                           {flight.data() + off + fiber::tls::kTlsRecordHeaderSize, len}, dst);
            ASSERT_EQ(fiber::tls::TlsRecordCipher::Status::Ok, opened.status);
            plaintext.insert(plaintext.end(), dst.data(), dst.data() + opened.plain_len);
            off += fiber::tls::kTlsRecordHeaderSize + len;
        }
        EXPECT_EQ(0, std::memcmp(kMessage, plaintext.data(), sizeof(kMessage) - 1));

        // ---- server → client app data through the moved write cipher ----
        const char kReply[] = "pong from fiber server";
        std::vector<std::uint8_t> sealed(state.write_cipher.seal_output_size(sizeof(kReply) - 1));
        const auto sealed_res =
                state.write_cipher.seal(fiber::tls::TlsContentType::ApplicationData,
                                        {reinterpret_cast<const std::uint8_t *>(kReply), sizeof(kReply) - 1}, sealed);
        ASSERT_EQ(fiber::tls::TlsRecordCipher::Status::Ok, sealed_res.status);
        ASSERT_TRUE(client->ship(frame_record(23, {sealed.data(), sealed_res.out_len})));
        std::array<char, 128> got{};
        const int read = SSL_read(client->ssl(), got.data(), static_cast<int>(got.size()));
        ASSERT_GT(read, 0);
        EXPECT_EQ(0, std::memcmp(kReply, got.data(), sizeof(kReply) - 1));
    });
}

// The same handshake fed one byte at a time — the reassembler must be
// indifferent to record/message chunking.
TEST(TlsServerHandshake13Full, HandshakeSlicedFeed) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        auto client = BoringClient::make(ClientOptions{});
        ASSERT_NE(nullptr, client);
        ServerMaterial material;
        const TlsServerConfig cfg = material.config();

        TlsServerHandshakeEngine engine(cfg, nullptr, nullptr);
        ASSERT_TRUE(drive(*client, engine, true));

        TlsConnectedState state = engine.take_state();
        EXPECT_EQ(fiber::tls::TlsProtocolVersion::Tls13, state.version);
        ASSERT_EQ(2, state.alpn_len);
    });
}

// An ECDSA-P256 server credential — the CV scheme selection picks
// ecdsa_secp256r1_sha256 over the same RSA-signed chain shape.
TEST(TlsServerHandshake13Full, EcCredentialHandshake) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        auto client = BoringClient::make(ClientOptions{});
        ASSERT_NE(nullptr, client);
        ServerMaterial material;
        material.load(certfix::kLeafEcP256Pem, certfix::kP256KeyPem);

        TlsServerHandshakeEngine engine(material.config(), nullptr, nullptr);
        ASSERT_TRUE(drive(*client, engine, false));

        TlsConnectedState state = engine.take_state();
        EXPECT_EQ(SSL_CIPHER_get_protocol_id(SSL_get_current_cipher(client->ssl())),
                  static_cast<std::uint16_t>(state.suite));
    });
}

// =====================================================================
// §8.2 — our 06 client engine against this server engine (both FSMs ours)
// =====================================================================

TEST(TlsServerHandshake13Full, OurClientDrivesOurServer) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        ServerMaterial server_material;
        auto trust = TlsTrustStore::from_pem_bundle({certfix::kRootRsaPem, std::strlen(certfix::kRootRsaPem)});
        ASSERT_TRUE(trust.has_value());

        TlsClientConfig client_cfg;
        client_cfg.sni_host = "example.com";
        client_cfg.alpn = server_material.alpn;
        client_cfg.trust = &*trust;
        client_cfg.now_unix_ms = certfix::kRefNowMs;

        TlsServerHandshakeEngine server(server_material.config(), nullptr, nullptr);
        TlsClientHandshakeEngine client(client_cfg, nullptr);
        ASSERT_FALSE(server.done());
        ASSERT_FALSE(client.done());

        // Pure in-memory pump: each side's output becomes the other's feed.
        for (int spin = 0; spin < 64 && !(server.done() && client.done()); ++spin) {
            const std::vector<std::uint8_t> to_server = chain_bytes(client.take_output());
            if (!to_server.empty()) {
                IoBuf buf = IoBuf::allocate(to_server.size());
                ASSERT_TRUE(buf.valid());
                std::memcpy(buf.writable_data(), to_server.data(), to_server.size());
                buf.commit(to_server.size());
                ASSERT_TRUE(server.feed(std::move(buf)).has_value());
            }
            const std::vector<std::uint8_t> to_client = chain_bytes(server.take_output());
            if (!to_client.empty()) {
                IoBuf buf = IoBuf::allocate(to_client.size());
                ASSERT_TRUE(buf.valid());
                std::memcpy(buf.writable_data(), to_client.data(), to_client.size());
                buf.commit(to_client.size());
                ASSERT_TRUE(client.feed(std::move(buf)).has_value());
            }
            if (server.done() && client.done()) {
                break;
            }
            ASSERT_FALSE(server.failed());
            ASSERT_FALSE(client.failed());
        }
        ASSERT_TRUE(server.done());
        ASSERT_TRUE(client.done());
        EXPECT_FALSE(server.failed());
        EXPECT_FALSE(client.failed());

        TlsConnectedState server_state = server.take_state();
        TlsConnectedState client_state = client.take_state();
        EXPECT_EQ(fiber::tls::TlsProtocolVersion::Tls13, server_state.version);
        EXPECT_EQ(client_state.suite, server_state.suite);
        ASSERT_EQ(2, server_state.alpn_len);
        EXPECT_EQ(0, std::memcmp(server_state.alpn.data(), client_state.alpn.data(), client_state.alpn_len));
        // The two engines derived the same application secrets (the handshake
        // itself already proved the traffic keys agree — this pins the DTO).
        EXPECT_TRUE(secrets_equal(client_state.client_app_secret, server_state.client_app_secret));
        EXPECT_TRUE(secrets_equal(client_state.server_app_secret, server_state.server_app_secret));
        // The server saw the full server chain; the client saw none.
        EXPECT_EQ(2u, client_state.peer_chain.size());
        EXPECT_TRUE(server_state.peer_chain.empty());
    });
}

// =====================================================================
// §8.2b — HelloRetryRequest + second-ClientHello protection rules
// =====================================================================

// A BoringSSL client whose first share is P-256 while it SUPPORTS X25519
// (group order "P-256:X25519"): the server prefers X25519 → one HRR → the
// client's CH2 carries the X25619 share → the handshake completes.
TEST(TlsServerHandshake13Hrr, RetryThenComplete) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        auto client = BoringClient::make(ClientOptions{.groups = "P-256:X25519"});
        ASSERT_NE(nullptr, client);
        ServerMaterial material;
        const TlsServerConfig cfg = material.config();

        TlsServerHandshakeEngine engine(cfg, nullptr, nullptr);
        DriveLog log;
        ASSERT_TRUE(drive(*client, engine, false, &log));
        ASSERT_TRUE(engine.done());
        EXPECT_FALSE(engine.failed());

        // The HRR actually flew: the sentinel random is in the server's first
        // plaintext record flight.
        const auto &sentinel = fiber::tls::kTlsHelloRetryRandom;
        EXPECT_TRUE(wire_contains(log.server_to_client, {sentinel.data(), sentinel.size()}));

        TlsConnectedState state = engine.take_state();
        EXPECT_EQ(fiber::tls::TlsProtocolVersion::Tls13, state.version);
        EXPECT_EQ(SSL_CIPHER_get_protocol_id(SSL_get_current_cipher(client->ssl())),
                  static_cast<std::uint16_t>(state.suite));
    });
}

// Hand-built CH pair over the raw codec: everything the drive harness cannot
// make a real client say. CH1 supports X25519 but shares only P-256 → HRR;
// CH2 still shares only P-256 → unexpected_message (no second HRR).
namespace {

std::vector<std::uint8_t> build_client_hello(std::span<const std::uint8_t> random, std::uint8_t random_first_byte,
                                             std::span<const std::uint8_t> point) {
    static const std::uint16_t kSuites[] = {0x1301, 0x1302, 0x1303};
    static const std::uint16_t kGroups[] = {0x001D, 0x0017}; // X25519 first (supported, unshared)
    static const auto kSigalgs = [] {
        std::array<std::uint16_t, fiber::tls::kTls13SignaturePreference.size()> out{};
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = static_cast<std::uint16_t>(fiber::tls::kTls13SignaturePreference[i]);
        }
        return out;
    }();

    std::array<std::uint8_t, 32> random_bytes{};
    std::memcpy(random_bytes.data(), random.data(), 32);
    random_bytes[0] = random_first_byte;
    const std::array<std::uint8_t, 4> session_id{0xDE, 0xAD, 0xBE, 0xEF};

    fiber::tls::TlsClientHelloInput in{};
    in.random = random_bytes;
    in.session_id = session_id;
    in.cipher_suites = kSuites;
    in.supported_groups = kGroups;
    in.signature_algorithms = kSigalgs;
    in.key_share_group = 0x0017; // P-256 share only — the deliberate miss
    in.key_share = point;
    in.sni_host = "example.com";
    in.alpn = {};

    const auto size = fiber::tls::tls_client_hello_size(in);
    EXPECT_TRUE(size.has_value());
    std::vector<std::uint8_t> message(*size);
    const auto encoded = fiber::tls::tls_encode_client_hello(in, message);
    EXPECT_TRUE(encoded.has_value());
    message.resize(encoded->len);

    // One plaintext handshake record.
    std::vector<std::uint8_t> wire;
    wire.reserve(fiber::tls::kTlsRecordHeaderSize + message.size());
    wire.push_back(22);
    wire.push_back(0x03);
    wire.push_back(0x01);
    wire.push_back(static_cast<std::uint8_t>(message.size() >> 8));
    wire.push_back(static_cast<std::uint8_t>(message.size()));
    wire.insert(wire.end(), message.begin(), message.end());
    return wire;
}

bool feed_raw(TlsServerHandshakeEngine &engine, const std::vector<std::uint8_t> &wire, Event &event) {
    IoBuf buf = IoBuf::allocate(wire.size());
    if (!buf.valid()) {
        return false;
    }
    std::memcpy(buf.writable_data(), wire.data(), wire.size());
    buf.commit(wire.size());
    const auto result = engine.feed(std::move(buf));
    if (!result.has_value()) {
        return false;
    }
    event = *result;
    return true;
}

} // namespace

TEST(TlsServerHandshake13Hrr, SecondShareMissAborts) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        // A real P-256 point from our own KX (encap requires a valid one later;
        // here only the share's PRESENCE matters).
        auto kx = fiber::tls::TlsKeyExchange::create(fiber::tls::TlsNamedGroup::Secp256r1);
        ASSERT_TRUE(kx.has_value());
        ASSERT_TRUE(kx.value()->generate().has_value());

        ServerMaterial material;
        TlsServerHandshakeEngine engine(material.config(), nullptr, nullptr);

        const std::array<std::uint8_t, 32> kRandom = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16,
                                                      17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
        const std::span<const std::uint8_t> point = kx.value()->public_value().bytes();

        Event event = Event::None;
        ASSERT_TRUE(feed_raw(engine, build_client_hello(kRandom, 0, point), event));
        EXPECT_EQ(Event::None, event); // the engine now awaits CH2
        EXPECT_FALSE(engine.done());
        const std::vector<std::uint8_t> hrr_flight = chain_bytes(engine.take_output());
        const auto &sentinel = fiber::tls::kTlsHelloRetryRandom;
        EXPECT_TRUE(wire_contains(hrr_flight, {sentinel.data(), sentinel.size()}));

        // CH2: identical echo fields, still no X25519 share → unexpected_message.
        ASSERT_TRUE(feed_raw(engine, build_client_hello(kRandom, 0, point), event));
        EXPECT_EQ(Event::Failed, event);
        EXPECT_EQ(TlsAlertDesc::UnexpectedMessage, engine.failure_alert());
    });
}

TEST(TlsServerHandshake13Hrr, EchoMismatchAborts) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        auto kx = fiber::tls::TlsKeyExchange::create(fiber::tls::TlsNamedGroup::Secp256r1);
        ASSERT_TRUE(kx.has_value());
        ASSERT_TRUE(kx.value()->generate().has_value());

        ServerMaterial material;
        TlsServerHandshakeEngine engine(material.config(), nullptr, nullptr);

        const std::array<std::uint8_t, 32> kRandom = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16,
                                                      17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
        const std::span<const std::uint8_t> point = kx.value()->public_value().bytes();

        Event event = Event::None;
        ASSERT_TRUE(feed_raw(engine, build_client_hello(kRandom, 0, point), event));
        EXPECT_FALSE(engine.done());

        // CH2 with a DIFFERENT random (first byte flipped) → illegal_parameter.
        ASSERT_TRUE(feed_raw(engine, build_client_hello(kRandom, 0x5A, point), event));
        EXPECT_EQ(Event::Failed, event);
        EXPECT_EQ(TlsAlertDesc::IllegalParameter, engine.failure_alert());
    });
}

// =====================================================================
// §8.3 — refusals
// =====================================================================

// A first handshake message that is not a ClientHello → unexpected_message.
TEST(TlsServerHandshake13Refuse, NonHelloFirstMessageRefused) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        ServerMaterial material;
        TlsServerHandshakeEngine engine(material.config(), nullptr, nullptr);

        // One plaintext handshake record carrying a bare Finished header.
        const std::array<std::uint8_t, 9> wire{22, 0x03, 0x03, 0, 4, 20, 0, 0, 0};
        IoBuf buf = IoBuf::allocate(wire.size());
        ASSERT_TRUE(buf.valid());
        std::memcpy(buf.writable_data(), wire.data(), wire.size());
        buf.commit(wire.size());
        const IoResult<Event> event = engine.feed(std::move(buf));
        ASSERT_TRUE(event.has_value());
        EXPECT_EQ(Event::Failed, *event);
        EXPECT_EQ(TlsAlertDesc::UnexpectedMessage, engine.failure_alert());
    });
}

// =====================================================================
// §8.4 — mTLS (1.3 CertificateRequest + client Cert/CV + verify_chain)
// =====================================================================

namespace {

// The server-side trust anchors for client chains.
struct ClientTrustMaterial {
    std::optional<TlsTrustStore> related; // signs kClientRsaPem
    std::optional<TlsTrustStore> unrelated;

    ClientTrustMaterial() {
        auto a = TlsTrustStore::from_pem_bundle({certfix::kRootRsaPem, std::strlen(certfix::kRootRsaPem)});
        auto b = TlsTrustStore::from_pem_bundle({certfix::kRootUnrelatedPem, std::strlen(certfix::kRootUnrelatedPem)});
        EXPECT_TRUE(a.has_value());
        EXPECT_TRUE(b.has_value());
        if (a.has_value()) {
            related = std::move(a).value();
        }
        if (b.has_value()) {
            unrelated = std::move(b).value();
        }
    }
};

} // namespace

// Required client certificate, presented and verified end to end.
TEST(TlsServerHandshake13Mtls, ClientCertificateVerified) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        auto client = BoringClient::make(ClientOptions{.client_cert = true});
        ASSERT_NE(nullptr, client);
        ServerMaterial material;
        ClientTrustMaterial trust;
        TlsServerConfig cfg = material.config();
        cfg.client_trust = &*trust.related;
        cfg.require_client_cert = true;

        TlsServerHandshakeEngine engine(cfg, nullptr, nullptr);
        ASSERT_TRUE(drive(*client, engine, false));

        TlsConnectedState state = engine.take_state();
        ASSERT_EQ(2u, state.peer_chain.size()); // kClientRsaPem leaf + RSA intermediate
        EXPECT_FALSE(state.session_resumed);
    });
}

// require_client_cert with an empty client chain → certificate_required.
TEST(TlsServerHandshake13Mtls, MissingCertificateRequired) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        auto client = BoringClient::make(ClientOptions{}); // no client credential
        ASSERT_NE(nullptr, client);
        ServerMaterial material;
        ClientTrustMaterial trust;
        TlsServerConfig cfg = material.config();
        cfg.client_trust = &*trust.related;
        cfg.require_client_cert = true;

        TlsServerHandshakeEngine engine(cfg, nullptr, nullptr);
        EXPECT_FALSE(drive(*client, engine, false));
        ASSERT_TRUE(engine.done());
        EXPECT_TRUE(engine.failed());
        EXPECT_EQ(TlsAlertDesc::CertificateRequired, engine.failure_alert());
    });
}

// Optional client certificate (require=false): no chain presented still
// completes, with an empty peer chain in the state.
TEST(TlsServerHandshake13Mtls, OptionalCertificateAbsent) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        auto client = BoringClient::make(ClientOptions{});
        ASSERT_NE(nullptr, client);
        ServerMaterial material;
        ClientTrustMaterial trust;
        TlsServerConfig cfg = material.config();
        cfg.client_trust = &*trust.related; // request, but do not require

        TlsServerHandshakeEngine engine(cfg, nullptr, nullptr);
        ASSERT_TRUE(drive(*client, engine, false));
        TlsConnectedState state = engine.take_state();
        EXPECT_TRUE(state.peer_chain.empty());
    });
}

// A presented chain that does not root in the configured anchors fails with
// the verifier's alert.
TEST(TlsServerHandshake13Mtls, UntrustedChainRefused) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        auto client = BoringClient::make(ClientOptions{.client_cert = true});
        ASSERT_NE(nullptr, client);
        ServerMaterial material;
        ClientTrustMaterial trust;
        TlsServerConfig cfg = material.config();
        cfg.client_trust = &*trust.unrelated;
        cfg.require_client_cert = true;

        TlsServerHandshakeEngine engine(cfg, nullptr, nullptr);
        EXPECT_FALSE(drive(*client, engine, false));
        ASSERT_TRUE(engine.done());
        EXPECT_TRUE(engine.failed());
        // 02b's mapping: an unrooted chain is unknown_ca (SSL_alert_from_verify_
        // result parity), not the generic bad_certificate.
        EXPECT_EQ(TlsAlertDesc::UnknownCa, engine.failure_alert());
    });
}

// =====================================================================
// §8.5 — TLS 1.2 full handshake (07 §4.3): a tls12_only BoringSSL client
// forks the 1.2 sub-flow. SH flight, EMS/RI/ALPN in the SH, SKE signature
// over the randoms+params form, the client flight [Cert] CKE [CV], the
// client-CCS read swap, both Finished, and the RFC 5077 NST mint.
// =====================================================================

TEST(TlsServerHandshake12Full, HandshakeAndAppRoundTrip) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        auto client = BoringClient::make(ClientOptions{.tls12_only = true});
        ASSERT_NE(nullptr, client);
        ServerMaterial material;

        TlsServerHandshakeEngine engine(material.config(), nullptr, nullptr);
        DriveLog log;
        ASSERT_TRUE(drive(*client, engine, false, &log));
        ASSERT_TRUE(engine.done());
        EXPECT_FALSE(engine.failed());

        TlsConnectedState state = engine.take_state();
        EXPECT_EQ(fiber::tls::TlsProtocolVersion::Tls12, state.version);
        const SSL_CIPHER *negotiated = SSL_get_current_cipher(client->ssl());
        ASSERT_NE(nullptr, negotiated);
        EXPECT_EQ(SSL_CIPHER_get_protocol_id(negotiated), static_cast<std::uint16_t>(state.suite));
        // The client offered EMS and the SH echoed it.
        EXPECT_EQ(1, SSL_get_extms_support(client->ssl()));
        ASSERT_EQ(2, state.alpn_len);
        EXPECT_EQ(0, std::memcmp("h2", state.alpn.data(), 2));
        EXPECT_FALSE(state.session_resumed);
        EXPECT_FALSE(state.early_data_accepted);
        EXPECT_TRUE(state.peer_chain.empty());
        // The RFC 8446 §4.1.3 downgrade sentinel rides the 1.2 SH random.
        const std::array<std::uint8_t, 8> kDowngrade{0x44, 0x4F, 0x57, 0x4E, 0x47, 0x52, 0x44, 0x01};
        EXPECT_TRUE(wire_contains(log.server_to_client, kDowngrade));

        // ---- client → server app data through the moved read cipher ----
        const char kMessage[] = "ping from boringssl tls12 client";
        EXPECT_EQ(sizeof(kMessage) - 1,
                  static_cast<std::size_t>(SSL_write(client->ssl(), kMessage, static_cast<int>(sizeof(kMessage) - 1))));
        const std::vector<std::uint8_t> flight = client->drain_wbio();
        std::vector<std::uint8_t> plaintext;
        std::size_t off = 0;
        while (off + fiber::tls::kTlsRecordHeaderSize <= flight.size()) {
            const std::size_t len = (static_cast<std::size_t>(flight[off + 3]) << 8) | flight[off + 4];
            if (flight[off] != 23) {
                off += fiber::tls::kTlsRecordHeaderSize + len;
                continue;
            }
            std::array<std::uint8_t, fiber::tls::kTlsMaxPlaintextSize> dst{};
            const auto opened =
                    state.read_cipher.open(fiber::tls::TlsContentType::ApplicationData,
                                           (static_cast<std::uint16_t>(flight[off + 1]) << 8) | flight[off + 2],
                                           static_cast<std::uint16_t>(len),
                                           {flight.data() + off + fiber::tls::kTlsRecordHeaderSize, len}, dst);
            ASSERT_EQ(fiber::tls::TlsRecordCipher::Status::Ok, opened.status);
            plaintext.insert(plaintext.end(), dst.data(), dst.data() + opened.plain_len);
            off += fiber::tls::kTlsRecordHeaderSize + len;
        }
        EXPECT_EQ(0, std::memcmp(kMessage, plaintext.data(), sizeof(kMessage) - 1));

        // ---- server → client app data through the moved write cipher ----
        const char kReply[] = "pong from fiber tls12 server";
        std::vector<std::uint8_t> sealed(state.write_cipher.seal_output_size(sizeof(kReply) - 1));
        const auto sealed_res =
                state.write_cipher.seal(fiber::tls::TlsContentType::ApplicationData,
                                        {reinterpret_cast<const std::uint8_t *>(kReply), sizeof(kReply) - 1}, sealed);
        ASSERT_EQ(fiber::tls::TlsRecordCipher::Status::Ok, sealed_res.status);
        ASSERT_TRUE(client->ship(frame_record(23, {sealed.data(), sealed_res.out_len})));
        std::array<char, 128> got{};
        const int read = SSL_read(client->ssl(), got.data(), static_cast<int>(got.size()));
        ASSERT_GT(read, 0);
        EXPECT_EQ(0, std::memcmp(kReply, got.data(), sizeof(kReply) - 1));
    });
}

// One byte at a time — the 1.2 reassembler path is the shared one, but the
// client flight spans two records (handshake, then CCS+sealed Fin).
TEST(TlsServerHandshake12Full, HandshakeSlicedFeed) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        auto client = BoringClient::make(ClientOptions{.tls12_only = true});
        ASSERT_NE(nullptr, client);
        ServerMaterial material;

        TlsServerHandshakeEngine engine(material.config(), nullptr, nullptr);
        ASSERT_TRUE(drive(*client, engine, true));

        TlsConnectedState state = engine.take_state();
        EXPECT_EQ(fiber::tls::TlsProtocolVersion::Tls12, state.version);
        ASSERT_EQ(2, state.alpn_len);
    });
}

// An ECDSA-P256 server credential: the suite filter keeps only the
// ECDHE-ECDSA half of the registry.
TEST(TlsServerHandshake12Full, EcCredentialHandshake) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        auto client = BoringClient::make(ClientOptions{.tls12_only = true});
        ASSERT_NE(nullptr, client);
        ServerMaterial material;
        material.load(certfix::kLeafEcP256Pem, certfix::kP256KeyPem);

        TlsServerHandshakeEngine engine(material.config(), nullptr, nullptr);
        ASSERT_TRUE(drive(*client, engine, false));

        TlsConnectedState state = engine.take_state();
        EXPECT_EQ(SSL_CIPHER_get_protocol_id(SSL_get_current_cipher(client->ssl())),
                  static_cast<std::uint16_t>(state.suite));
        EXPECT_EQ(0xC02B, static_cast<std::uint16_t>(state.suite)); // ECDHE-ECDSA-AES128-GCM-SHA256
    });
}

// Pinned single-suite clients: the server takes its preference entry that
// survives the pin (0xC030 and 0xCCA8 are both RSA suites at different
// registry depths).
TEST(TlsServerHandshake12Full, PinnedSuiteSelection) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        const std::pair<const char *, std::uint16_t> kPins[] = {{"ECDHE-RSA-AES256-GCM-SHA384", 0xC030},
                                                                {"ECDHE-RSA-CHACHA20-POLY1305", 0xCCA8}};
        for (const auto &[cipher_list, want]: kPins) {
            auto client = BoringClient::make(ClientOptions{.tls12_only = true, .tls12_ciphers = cipher_list});
            ASSERT_NE(nullptr, client);
            ServerMaterial material;
            TlsServerHandshakeEngine engine(material.config(), nullptr, nullptr);
            ASSERT_TRUE(drive(*client, engine, false))
                    << cipher_list << " alert=" << static_cast<int>(engine.failure_alert());

            TlsConnectedState state = engine.take_state();
            EXPECT_EQ(want, static_cast<std::uint16_t>(state.suite)) << cipher_list;
            EXPECT_EQ(SSL_CIPHER_get_protocol_id(SSL_get_current_cipher(client->ssl())), want) << cipher_list;
        }
    });
}

// mTLS at 1.2: the CR is the three-vector form, the client CV signs the raw
// transcript through its own CKE.
TEST(TlsServerHandshake12Full, ClientCertificateVerified) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        auto client = BoringClient::make(ClientOptions{.tls12_only = true, .client_cert = true});
        ASSERT_NE(nullptr, client);
        ServerMaterial material;
        ClientTrustMaterial trust;
        TlsServerConfig cfg = material.config();
        cfg.client_trust = &*trust.related;
        cfg.require_client_cert = true;

        TlsServerHandshakeEngine engine(cfg, nullptr, nullptr);
        ASSERT_TRUE(drive(*client, engine, false));

        TlsConnectedState state = engine.take_state();
        EXPECT_EQ(fiber::tls::TlsProtocolVersion::Tls12, state.version);
        ASSERT_EQ(2u, state.peer_chain.size()); // kClientRsaPem leaf + RSA intermediate
    });
}

namespace {

// A capturing TlsTicketMinter: records the request, mints a fixed blob.
struct CapturingMinter {
    bool called = false;
    TlsCipherSuiteId suite = TlsCipherSuiteId::TlsAes128GcmSha256;
    std::uint32_t timeout_s = 0;
    std::vector<std::uint8_t> resumption_master;
    std::string_view alpn;
    static constexpr std::array<std::uint8_t, 4> kTicket{0xCA, 0xFE, 0xBA, 0xBE};

    static std::size_t mint(void *ctx, const TlsTicketRequest &req, std::span<std::uint8_t> out) noexcept {
        auto &self = *static_cast<CapturingMinter *>(ctx);
        self.called = true;
        self.suite = req.suite;
        self.timeout_s = req.timeout_s;
        self.alpn = req.alpn;
        self.resumption_master.assign(req.resumption_master.begin(), req.resumption_master.end());
        if (out.size() < kTicket.size()) {
            return 0;
        }
        std::memcpy(out.data(), kTicket.data(), kTicket.size());
        return kTicket.size();
    }

    [[nodiscard]] TlsTicketMinter hook() noexcept { return TlsTicketMinter{&mint, this}; }
};

} // namespace

// The BoringSSL client offers RFC 5077 session tickets at 1.2; the server
// echoes the empty extension in the SH and mints one NST after the client
// Finished, in cleartext before its CCS.
TEST(TlsServerHandshake12Full, TicketMintedAfterClientFinished) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        auto client = BoringClient::make(ClientOptions{.tls12_only = true});
        ASSERT_NE(nullptr, client);
        ServerMaterial material;
        CapturingMinter minter;

        TlsTicketMinter hook = minter.hook();
        TlsServerHandshakeEngine engine(material.config(), nullptr, &hook);
        DriveLog log;
        ASSERT_TRUE(drive(*client, engine, false, &log));

        TlsConnectedState state = engine.take_state();
        ASSERT_TRUE(minter.called);
        EXPECT_EQ(state.suite, minter.suite);
        EXPECT_EQ(7200u, minter.timeout_s);
        ASSERT_EQ(2u, minter.alpn.size());
        EXPECT_EQ(0, std::memcmp("h2", minter.alpn.data(), 2));
        ASSERT_EQ(48u, minter.resumption_master.size());
        EXPECT_EQ(0, std::memcmp(state.tls12_master.bytes().data(), minter.resumption_master.data(), 48));
        // The NST is plaintext on the wire — the ticket blob is verbatim.
        EXPECT_TRUE(wire_contains(log.server_to_client, CapturingMinter::kTicket));
    });
}


// =====================================================================
// §8.2/§8.3 — PSK resumption, 0-RTT, and the 1.3 NewSessionTicket (P5)
// =====================================================================

namespace {

// In-memory session store behind both 08 hooks. mint derives the true
// resumption PSK from the request (Expand-Label(resumption_master,
// "resumption", nonce) — exactly what the 08 session layer will do) and
// files it under an 8-byte counter ticket; lookup resolves a ticket back to
// borrowed views of the entry. std::map nodes are stable, so the
// TlsResumedSession spans stay valid for the whole handshake.
class TestSessionStore {
public:
    struct Entry {
        std::vector<std::uint8_t> psk;
        TlsCipherSuiteId suite = TlsCipherSuiteId::TlsAes128GcmSha256;
        std::string alpn;
        std::uint32_t age_add = 0;
        std::uint32_t max_early_data = 0;
        std::int64_t issued_ms = 0;
    };

    bool miss_everything = false; // force lookup misses (fallback test)

    [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }

    // The first-minted ticket blob + entry (hop-1's NST); null when empty.
    [[nodiscard]] std::vector<std::uint8_t> first_ticket() const {
        return entries.empty() ? std::vector<std::uint8_t>{} : entries.begin()->first;
    }
    [[nodiscard]] const Entry *first_entry() const noexcept {
        return entries.empty() ? nullptr : &entries.begin()->second;
    }
    void corrupt_first_psk() noexcept {
        if (!entries.empty()) {
            entries.begin()->second.psk[0] ^= 0xFF; // binder no longer matches the client's
        }
    }
    void age_skew_first(std::int64_t skew_ms) noexcept {
        if (!entries.empty()) {
            entries.begin()->second.issued_ms -= skew_ms; // server_age grows by skew_ms
        }
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
        // The opaque ticket is an 8-byte big-endian counter.
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

    static bool lookup(void *ctx, std::span<const std::uint8_t> identity, std::string_view name,
                       std::int64_t now_unix_ms, TlsResumedSession &out) noexcept {
        // The in-memory store is identity-keyed only; the SNI/expiry pair is
        // the stateless open's contract, not this table's.
        (void) name;
        (void) now_unix_ms;
        const auto &self = *static_cast<TestSessionStore *>(ctx);
        if (self.miss_everything) {
            return false;
        }
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
        out.ticket_issued_ms = entry.issued_ms;
        return true;
    }

    [[nodiscard]] TlsTicketMinter minter_hook() noexcept { return {&mint, this}; }
    [[nodiscard]] TlsResumptionLookup lookup_hook() noexcept { return {&lookup, this}; }

private:
    std::map<std::vector<std::uint8_t>, Entry> entries;
    std::uint64_t next_id = 1;
};

// Hop-1 helper: a full handshake with the minter installed leaves exactly
// one ticket in the store and one session in the client's stash.
void run_ticketed_hop1(const TlsServerConfig &cfg, const TlsTicketMinter &minter) {
    auto client = BoringClient::make(ClientOptions{.collect_tickets = true});
    ASSERT_NE(nullptr, client);
    TlsServerHandshakeEngine engine(cfg, nullptr, &minter);
    ASSERT_TRUE(drive(*client, engine, false));
    EXPECT_FALSE(engine.failed());
    EXPECT_FALSE(engine.take_state().session_resumed);
    // drive's final spin shipped the post-done NST into the client's read
    // BIO; the read path digests it and the stash collects the session.
    std::array<char, 64> sink{};
    (void) SSL_read(client->ssl(), sink.data(), static_cast<int>(sink.size()));
    ASSERT_EQ(1u, g_new_sessions.size());
}

bool feed_server(TlsServerHandshakeEngine &engine, std::span<const std::uint8_t> bytes) {
    IoBuf buf = IoBuf::allocate(bytes.size());
    if (!buf.valid()) {
        return false;
    }
    std::memcpy(buf.writable_data(), bytes.data(), bytes.size());
    buf.commit(bytes.size());
    return engine.feed(std::move(buf)).has_value();
}

bool feed_client(TlsClientHandshakeEngine &engine, std::span<const std::uint8_t> bytes) {
    IoBuf buf = IoBuf::allocate(bytes.size());
    if (!buf.valid()) {
        return false;
    }
    std::memcpy(buf.writable_data(), bytes.data(), bytes.size());
    buf.commit(bytes.size());
    return engine.feed(std::move(buf)).has_value();
}

// Dual-engine pump. Feeding a terminal engine is a FIBER_ASSERT, so each
// direction guards on its receiver: the only post-done emission is the
// server's NST (or, on a failure, its fatal alert), which a finished client
// must not be fed — those bytes land in `server_tail` when given.
bool pump(TlsClientHandshakeEngine &client, TlsServerHandshakeEngine &server,
          std::vector<std::uint8_t> *server_tail = nullptr) {
    for (int spin = 0; spin < 64 && !(server.done() && client.done()); ++spin) {
        const std::vector<std::uint8_t> to_server = chain_bytes(client.take_output());
        if (!to_server.empty()) {
            if (server.done() || !feed_server(server, to_server)) {
                return false;
            }
        }
        const std::vector<std::uint8_t> to_client = chain_bytes(server.take_output());
        if (!to_client.empty()) {
            // Record-granular feed: a flight can carry records past the
            // client's terminal message (the post-done NST, or the fatal
            // alert of a failed handshake); the unfed remainder lands in
            // the tail.
            std::size_t roff = 0;
            while (roff < to_client.size() && !client.done()) {
                std::size_t chunk = to_client.size() - roff;
                if (roff + fiber::tls::kTlsRecordHeaderSize < to_client.size()) {
                    const std::size_t rlen = (static_cast<std::size_t>(to_client[roff + 3]) << 8) | to_client[roff + 4];
                    if (roff + fiber::tls::kTlsRecordHeaderSize + rlen <= to_client.size()) {
                        chunk = fiber::tls::kTlsRecordHeaderSize + rlen; // one complete record
                    }
                }
                if (!feed_client(client, {to_client.data() + roff, chunk})) {
                    return false;
                }
                roff += chunk;
            }
            if (roff < to_client.size() && server_tail != nullptr) {
                server_tail->insert(server_tail->end(), to_client.begin() + static_cast<std::ptrdiff_t>(roff),
                                    to_client.end());
            }
        }
        if (server.failed() || client.failed()) {
            return server.done() && client.done();
        }
    }
    return server.done() && client.done();
}

// Opens every sealed record in `wire` through `read` (advancing its
// sequence) and discards the plaintext; plaintext records pass untouched.
bool open_and_discard(TlsRecordCipher &read, const std::vector<std::uint8_t> &wire) {
    std::size_t off = 0;
    while (off + fiber::tls::kTlsRecordHeaderSize <= wire.size()) {
        const std::size_t len = (static_cast<std::size_t>(wire[off + 3]) << 8) | wire[off + 4];
        if (off + fiber::tls::kTlsRecordHeaderSize + len > wire.size()) {
            return false;
        }
        if (wire[off] == 23) {
            std::array<std::uint8_t, fiber::tls::kTlsMaxPlaintextSize> dst{};
            const auto opened = read.open(fiber::tls::TlsContentType::ApplicationData,
                                          (static_cast<std::uint16_t>(wire[off + 1]) << 8) | wire[off + 2],
                                          static_cast<std::uint16_t>(len),
                                          {wire.data() + off + fiber::tls::kTlsRecordHeaderSize, len}, dst);
            if (opened.status != fiber::tls::TlsRecordCipher::Status::Ok) {
                return false;
            }
        }
        off += fiber::tls::kTlsRecordHeaderSize + len;
    }
    return true;
}

// Concatenates every app-data record opened through `read` (the round-trip
// helper the full-handshake tests inline).
std::vector<std::uint8_t> open_app_records(TlsRecordCipher &read, const std::vector<std::uint8_t> &wire) {
    std::vector<std::uint8_t> plaintext;
    std::size_t off = 0;
    while (off + fiber::tls::kTlsRecordHeaderSize <= wire.size()) {
        const std::size_t len = (static_cast<std::size_t>(wire[off + 3]) << 8) | wire[off + 4];
        if (wire[off] != 23) {
            off += fiber::tls::kTlsRecordHeaderSize + len;
            continue;
        }
        std::array<std::uint8_t, fiber::tls::kTlsMaxPlaintextSize> dst{};
        const auto opened = read.open(fiber::tls::TlsContentType::ApplicationData,
                                      (static_cast<std::uint16_t>(wire[off + 1]) << 8) | wire[off + 2],
                                      static_cast<std::uint16_t>(len),
                                      {wire.data() + off + fiber::tls::kTlsRecordHeaderSize, len}, dst);
        if (opened.status != fiber::tls::TlsRecordCipher::Status::Ok) {
            return {};
        }
        plaintext.insert(plaintext.end(), dst.data(), dst.data() + opened.plain_len);
        off += fiber::tls::kTlsRecordHeaderSize + len;
    }
    return plaintext;
}

std::vector<std::uint8_t> seal_app_record(TlsRecordCipher &write, std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> out(fiber::tls::kTlsRecordHeaderSize + write.seal_output_size(payload.size()));
    const auto sealed =
            write.seal(fiber::tls::TlsContentType::ApplicationData, payload,
                       {out.data() + fiber::tls::kTlsRecordHeaderSize, out.size() - fiber::tls::kTlsRecordHeaderSize});
    if (sealed.status != fiber::tls::TlsRecordCipher::Status::Ok) {
        return {};
    }
    out.resize(fiber::tls::kTlsRecordHeaderSize + sealed.out_len);
    out[0] = 23;
    out[1] = 0x03;
    out[2] = 0x03;
    out[3] = static_cast<std::uint8_t>(sealed.out_len >> 8);
    out[4] = static_cast<std::uint8_t>(sealed.out_len);
    return out;
}

// Client material for the dual-engine tests (the OurClientDrivesOurServer
// setup, factored).
struct DualMaterial {
    ServerMaterial server_material;
    IoResult<TlsTrustStore> trust =
            TlsTrustStore::from_pem_bundle({certfix::kRootRsaPem, std::strlen(certfix::kRootRsaPem)});
    TlsClientConfig client_cfg{};

    DualMaterial() {
        EXPECT_TRUE(trust.has_value()); // ASSERT may not return from a ctor
        client_cfg.sni_host = "example.com";
        client_cfg.alpn = server_material.alpn;
        client_cfg.trust = &*trust;
        client_cfg.now_unix_ms = certfix::kRefNowMs;
    }
};

} // namespace

// A BoringSSL client offers hop-1's ticket; the store resolves it and the
// handshake resumes over the PSK (no certificate flight), then the resumed
// connection mints — and the client parses — a second ticket.
TEST(TlsServerHandshake13Psk, BoringClientResumesWithTicket) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        CollectedSessionsGuard guard;
        ServerMaterial material;
        const TlsServerConfig cfg = material.config();
        TestSessionStore store;
        const TlsTicketMinter minter = store.minter_hook();
        const TlsResumptionLookup lookup = store.lookup_hook();

        run_ticketed_hop1(cfg, minter);
        ASSERT_EQ(1u, store.size());

        auto client = BoringClient::make(ClientOptions{.collect_tickets = true});
        ASSERT_NE(nullptr, client);
        ASSERT_EQ(1, SSL_set_session(client->ssl(), g_new_sessions.front()));
        TlsServerHandshakeEngine engine(cfg, &lookup, &minter);
        ASSERT_TRUE(drive(*client, engine, false));
        EXPECT_FALSE(engine.failed());

        TlsConnectedState state = engine.take_state();
        EXPECT_TRUE(state.session_resumed);
        EXPECT_FALSE(state.early_data_accepted);
        EXPECT_TRUE(state.peer_chain.empty()); // PSK resume: no certificate flight either
        EXPECT_EQ(SSL_CIPHER_get_protocol_id(SSL_get_current_cipher(client->ssl())),
                  static_cast<std::uint16_t>(state.suite));
        EXPECT_EQ(1, SSL_session_reused(client->ssl()));
        EXPECT_EQ(2u, store.size()); // the resumed connection minted its own NST

        // The NST flew after Done; drive's final spin already shipped it into
        // the client's read BIO. The read path digests it and the stash grows.
        std::array<char, 64> sink{};
        (void) SSL_read(client->ssl(), sink.data(), static_cast<int>(sink.size()));
        EXPECT_EQ(2u, g_new_sessions.size());

        // App-data round trip on the resumed connection.
        const char kPing[] = "ping over psk";
        ASSERT_EQ(sizeof(kPing) - 1, SSL_write(client->ssl(), kPing, static_cast<int>(sizeof(kPing) - 1)));
        const std::vector<std::uint8_t> ping = open_app_records(state.read_cipher, client->drain_wbio());
        ASSERT_EQ(sizeof(kPing) - 1, ping.size());
        EXPECT_EQ(0, std::memcmp(kPing, ping.data(), ping.size()));

        const char kPong[] = "pong over psk";
        const std::vector<std::uint8_t> pong =
                seal_app_record(state.write_cipher, {reinterpret_cast<const std::uint8_t *>(kPong), sizeof(kPong) - 1});
        ASSERT_FALSE(pong.empty());
        ASSERT_TRUE(client->ship(pong));
        const int got = SSL_read(client->ssl(), sink.data(), static_cast<int>(sink.size()));
        ASSERT_GT(got, 0);
        EXPECT_EQ(0, std::memcmp(kPong, sink.data(), sizeof(kPong) - 1));
    });
}

// Hand-driven 0-RTT: the early write must fly before the ServerHello lands,
// so the drive harness cannot be used. The engine answers CH + CCS + early
// records in one feed, accepts the early data (ALPN and ticket agree), hands
// the plaintext back through take_early_data, and finishes at the client's
// EOED + Fin.
TEST(TlsServerHandshake13Psk, BoringClientEarlyDataAccepted) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        CollectedSessionsGuard guard;
        ServerMaterial material;
        TlsServerConfig cfg = material.config();
        cfg.enable_early_data = true;
        TestSessionStore store;
        const TlsTicketMinter minter = store.minter_hook();
        const TlsResumptionLookup lookup = store.lookup_hook();

        run_ticketed_hop1(cfg, minter);
        const TestSessionStore::Entry *entry = store.first_entry();
        ASSERT_NE(nullptr, entry);
        EXPECT_EQ(14336u, entry->max_early_data); // kMaxEarlyDataAccepted (BoringSSL parity)

        auto client = BoringClient::make(ClientOptions{.collect_tickets = true});
        ASSERT_NE(nullptr, client);
        ASSERT_EQ(1, SSL_set_session(client->ssl(), g_new_sessions.front()));
        SSL_set_early_data_enabled(client->ssl(), 1); // returns void
        TlsServerHandshakeEngine engine(cfg, &lookup, &minter);

        // ClientHello first. A 0-RTT BoringSSL client early-returns 1 here (the
        // mirror of the accepting server's early return) so the app may write
        // early data before any server flight lands.
        EXPECT_EQ(1, client->handshake_step());
        std::vector<std::uint8_t> first = client->drain_wbio();
        // The early request rides out before any server flight arrives.
        const char kEarly[] = "GET /early HTTP/1.1\r\nhost: example.com\r\n\r\n";
        ASSERT_EQ(sizeof(kEarly) - 1, SSL_write(client->ssl(), kEarly, static_cast<int>(sizeof(kEarly) - 1)));
        const std::vector<std::uint8_t> early_wire = client->drain_wbio();
        first.insert(first.end(), early_wire.begin(), early_wire.end());
        Event event = Event::None;
        ASSERT_TRUE(feed_bytes(engine, first, false, event));

        // The engine's flight closes the window; the client completes and sends
        // EOED (under the EARLY keys, sequence continuing) + Fin (fresh client_hs).
        ASSERT_TRUE(client->ship(chain_bytes(engine.take_output())));
        EXPECT_EQ(1, client->handshake_step());
        ASSERT_TRUE(feed_bytes(engine, client->drain_wbio(), false, event));
        EXPECT_EQ(Event::HandshakeDone, event);
        EXPECT_TRUE(engine.done());
        EXPECT_FALSE(engine.failed());
        EXPECT_EQ(1, SSL_early_data_accepted(client->ssl()));

        TlsConnectedState state = engine.take_state();
        EXPECT_TRUE(state.session_resumed);
        EXPECT_TRUE(state.early_data_accepted);
        const std::vector<std::uint8_t> early = chain_bytes(engine.take_early_data());
        ASSERT_EQ(sizeof(kEarly) - 1, early.size());
        EXPECT_EQ(0, std::memcmp(kEarly, early.data(), early.size()));
        EXPECT_EQ(2u, store.size()); // NST minted on the 0-RTT connection too

        // The NST flies after Done; deliver it, then round-trip app data.
        ASSERT_TRUE(client->ship(chain_bytes(engine.take_output())));
        std::array<char, 64> sink{};
        (void) SSL_read(client->ssl(), sink.data(), static_cast<int>(sink.size()));
        EXPECT_EQ(2u, g_new_sessions.size());

        const char kPing[] = "ping after 0rtt";
        ASSERT_EQ(sizeof(kPing) - 1, SSL_write(client->ssl(), kPing, static_cast<int>(sizeof(kPing) - 1)));
        const std::vector<std::uint8_t> ping = open_app_records(state.read_cipher, client->drain_wbio());
        ASSERT_EQ(sizeof(kPing) - 1, ping.size());
        EXPECT_EQ(0, std::memcmp(kPing, ping.data(), ping.size()));

        const char kPong[] = "pong after 0rtt";
        const std::vector<std::uint8_t> pong =
                seal_app_record(state.write_cipher, {reinterpret_cast<const std::uint8_t *>(kPong), sizeof(kPong) - 1});
        ASSERT_FALSE(pong.empty());
        ASSERT_TRUE(client->ship(pong));
        const int got = SSL_read(client->ssl(), sink.data(), static_cast<int>(sink.size()));
        ASSERT_GT(got, 0);
        EXPECT_EQ(0, std::memcmp(kPong, sink.data(), sizeof(kPong) - 1));
    });
}

// PSK offer + HRR: the client's first share is P-256 (server prefers
// X25519) and it wrote early data before the HRR landed. The early records
// ride BEFORE any server key exists — the skip window counts and drops them
// (§4.2.10); CH2 carries the SECOND binder over message_hash‖HRR‖CH2 and the
// PSK survives the retry.
TEST(TlsServerHandshake13Psk, BoringClientHrrAfterPskResume) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        CollectedSessionsGuard guard;
        ServerMaterial material;
        TlsServerConfig cfg = material.config();
        cfg.enable_early_data = true;
        TestSessionStore store;
        const TlsTicketMinter minter = store.minter_hook();
        const TlsResumptionLookup lookup = store.lookup_hook();

        run_ticketed_hop1(cfg, minter);

        auto client = BoringClient::make(ClientOptions{.groups = "P-256:X25519", .collect_tickets = true});
        ASSERT_NE(nullptr, client);
        ASSERT_EQ(1, SSL_set_session(client->ssl(), g_new_sessions.front()));
        SSL_set_early_data_enabled(client->ssl(), 1); // returns void
        TlsServerHandshakeEngine engine(cfg, &lookup, &minter);

        // CH (P-256 share) + early data, all before any server flight. The
        // 0-RTT client early-returns 1 after emitting the ClientHello.
        EXPECT_EQ(1, client->handshake_step());
        std::vector<std::uint8_t> first = client->drain_wbio();
        const char kEarly[] = "early-then-hrr";
        ASSERT_EQ(sizeof(kEarly) - 1, SSL_write(client->ssl(), kEarly, static_cast<int>(sizeof(kEarly) - 1)));
        const std::vector<std::uint8_t> early_wire = client->drain_wbio();
        first.insert(first.end(), early_wire.begin(), early_wire.end());
        Event event = Event::None;
        ASSERT_TRUE(feed_bytes(engine, first, false, event));

        // HelloRetryRequest came back; the early records were skipped, not fatal.
        const std::vector<std::uint8_t> hrr_flight = chain_bytes(engine.take_output());
        const auto &sentinel = fiber::tls::kTlsHelloRetryRandom;
        EXPECT_TRUE(wire_contains(hrr_flight, {sentinel.data(), sentinel.size()}));
        EXPECT_FALSE(engine.done());

        // CH2 (X25519 share, second binder) → SH flight → client Fin.
        ASSERT_TRUE(client->ship(hrr_flight));
        // The HRR rejected the client's early data: BoringSSL surfaces
        // SSL_ERROR_EARLY_DATA_REJECTED once so the app can rewind its early
        // writes; the retried step emits CH2 and waits for the SH flight.
        const int rejected = SSL_do_handshake(client->ssl());
        ASSERT_EQ(SSL_ERROR_EARLY_DATA_REJECTED, SSL_get_error(client->ssl(), rejected));
        // The rejection is sticky: the reset lets the client act on the HRR and
        // emit CH2 (the app re-sends its early writes afterwards).
        SSL_reset_early_data_reject(client->ssl());
        EXPECT_EQ(0, client->handshake_step());
        ASSERT_TRUE(feed_bytes(engine, client->drain_wbio(), false, event));
        ASSERT_TRUE(client->ship(chain_bytes(engine.take_output())));
        EXPECT_EQ(1, client->handshake_step());
        ASSERT_TRUE(feed_bytes(engine, client->drain_wbio(), false, event));
        EXPECT_EQ(Event::HandshakeDone, event);
        EXPECT_FALSE(engine.failed());

        TlsConnectedState state = engine.take_state();
        EXPECT_TRUE(state.session_resumed); // the PSK survived the retry
        EXPECT_FALSE(state.early_data_accepted); // 0-RTT never survives an HRR
        EXPECT_TRUE(chain_bytes(engine.take_early_data()).empty());
        EXPECT_EQ(0, SSL_early_data_accepted(client->ssl()));
        EXPECT_EQ(2u, store.size());

        // App data still round-trips.
        const char kPing[] = "ping after hrr-psk";
        ASSERT_EQ(sizeof(kPing) - 1, SSL_write(client->ssl(), kPing, static_cast<int>(sizeof(kPing) - 1)));
        const std::vector<std::uint8_t> ping = open_app_records(state.read_cipher, client->drain_wbio());
        ASSERT_EQ(sizeof(kPing) - 1, ping.size());
        EXPECT_EQ(0, std::memcmp(kPing, ping.data(), ping.size()));
    });
}

// Both FSMs ours, no BoringSSL in the loop: our client resumes our server
// off a ticket our minter issued, then the pair does it again with 0-RTT
// (the strongest regression pair for the EOED write-cipher fix).
TEST(TlsServerHandshake13Psk, OurClientResumesOurServer) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        DualMaterial material;
        TlsServerConfig cfg = material.server_material.config();
        TestSessionStore store;
        const TlsTicketMinter minter = store.minter_hook();
        const TlsResumptionLookup lookup = store.lookup_hook();

        // hop 1: our pair, minter installed.
        {
            TlsServerHandshakeEngine server(cfg, nullptr, &minter);
            TlsClientHandshakeEngine client(material.client_cfg, nullptr);
            ASSERT_TRUE(pump(client, server));
            EXPECT_FALSE(server.failed());
            EXPECT_FALSE(client.failed());
            EXPECT_FALSE(server.take_state().session_resumed);
        }
        ASSERT_EQ(1u, store.size());
        const TestSessionStore::Entry *entry = store.first_entry();
        ASSERT_NE(nullptr, entry);

        // hop 2: the offer mirrors the store entry exactly (elapsed 0 → the
        // obfuscated age is the age_add).
        TlsSessionOffer offer;
        const std::vector<std::uint8_t> ticket = store.first_ticket();
        offer.identity = ticket;
        offer.obfuscated_ticket_age = entry->age_add;
        offer.suite = entry->suite;
        offer.psk = entry->psk;

        TlsServerHandshakeEngine server(cfg, &lookup, &minter);
        TlsClientHandshakeEngine client(material.client_cfg, &offer);
        std::vector<std::uint8_t> tail; // the post-done NST
        ASSERT_TRUE(pump(client, server, &tail));
        EXPECT_FALSE(server.failed());
        EXPECT_FALSE(client.failed());

        TlsConnectedState server_state = server.take_state();
        TlsConnectedState client_state = client.take_state();
        EXPECT_TRUE(server_state.session_resumed);
        EXPECT_TRUE(client_state.session_resumed);
        EXPECT_FALSE(server_state.early_data_accepted);
        EXPECT_TRUE(chain_bytes(server.take_early_data()).empty());
        EXPECT_EQ(2u, store.size()); // the resumed connection minted its own NST
        EXPECT_TRUE(secrets_equal(client_state.client_app_secret, server_state.client_app_secret));
        EXPECT_TRUE(secrets_equal(client_state.server_app_secret, server_state.server_app_secret));

        // Post-handshake app data through the moved ciphers, both directions.
        const char kPing[] = "ping ours-psk";
        const std::vector<std::uint8_t> ping = seal_app_record(
                client_state.write_cipher, {reinterpret_cast<const std::uint8_t *>(kPing), sizeof(kPing) - 1});
        ASSERT_FALSE(ping.empty());
        const std::vector<std::uint8_t> opened = open_app_records(server_state.read_cipher, ping);
        ASSERT_EQ(sizeof(kPing) - 1, opened.size());
        EXPECT_EQ(0, std::memcmp(kPing, opened.data(), opened.size()));

        // The NST (sealed under server_app0, write seq 0) must be opened through
        // the client's read cipher first — the pong follows at seq 1.
        ASSERT_TRUE(open_and_discard(client_state.read_cipher, tail));
        const char kPong[] = "pong ours-psk";
        const std::vector<std::uint8_t> pong = seal_app_record(
                server_state.write_cipher, {reinterpret_cast<const std::uint8_t *>(kPong), sizeof(kPong) - 1});
        ASSERT_FALSE(pong.empty());
        const std::vector<std::uint8_t> opened_back = open_app_records(client_state.read_cipher, pong);
        ASSERT_EQ(sizeof(kPong) - 1, opened_back.size());
        EXPECT_EQ(0, std::memcmp(kPong, opened_back.data(), opened_back.size()));
    });
}

// Both FSMs ours with 0-RTT: the client seals early data under the early
// keys, the server accepts (ALPN and ticket agree), sinks the plaintext,
// and the client's EndOfEarlyData closes the window before its Finished.
TEST(TlsServerHandshake13Psk, OurClientEarlyDataAccepted) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        DualMaterial material;
        TlsServerConfig cfg = material.server_material.config();
        cfg.enable_early_data = true;
        TestSessionStore store;
        const TlsTicketMinter minter = store.minter_hook();
        const TlsResumptionLookup lookup = store.lookup_hook();

        // hop 1 mints a 0-RTT-capable ticket.
        {
            TlsServerHandshakeEngine server(cfg, nullptr, &minter);
            TlsClientHandshakeEngine client(material.client_cfg, nullptr);
            ASSERT_TRUE(pump(client, server));
            EXPECT_FALSE(server.failed());
        }
        const TestSessionStore::Entry *entry = store.first_entry();
        ASSERT_NE(nullptr, entry);
        EXPECT_EQ(14336u, entry->max_early_data); // kMaxEarlyDataAccepted (BoringSSL parity)

        TlsSessionOffer offer;
        const std::vector<std::uint8_t> ticket = store.first_ticket();
        offer.identity = ticket;
        offer.obfuscated_ticket_age = entry->age_add;
        offer.suite = entry->suite;
        offer.psk = entry->psk;
        offer.max_early_data = entry->max_early_data;

        TlsServerHandshakeEngine server(cfg, &lookup, &minter);
        TlsClientHandshakeEngine client(material.client_cfg, &offer);
        const std::string_view early_request = "GET /early HTTP/1.1\r\nhost: example.com\r\n\r\n";
        ASSERT_TRUE(client.write_early_data(
                                  {reinterpret_cast<const std::uint8_t *>(early_request.data()), early_request.size()})
                            .has_value());
        std::vector<std::uint8_t> tail; // the post-done NST
        ASSERT_TRUE(pump(client, server, &tail));
        EXPECT_FALSE(server.failed());
        EXPECT_FALSE(client.failed());

        TlsConnectedState server_state = server.take_state();
        TlsConnectedState client_state = client.take_state();
        EXPECT_TRUE(server_state.session_resumed);
        EXPECT_TRUE(client_state.session_resumed);
        EXPECT_TRUE(server_state.early_data_accepted);
        EXPECT_TRUE(client_state.early_data_accepted);
        const std::vector<std::uint8_t> early = chain_bytes(server.take_early_data());
        ASSERT_EQ(early_request.size(), early.size());
        EXPECT_EQ(0, std::memcmp(early_request.data(), early.data(), early.size()));
        EXPECT_EQ(2u, store.size());
        EXPECT_TRUE(secrets_equal(client_state.client_app_secret, server_state.client_app_secret));
        EXPECT_TRUE(secrets_equal(client_state.server_app_secret, server_state.server_app_secret));

        // Post-handshake app data through the moved ciphers, both directions.
        const char kPing[] = "ping ours-0rtt";
        const std::vector<std::uint8_t> ping = seal_app_record(
                client_state.write_cipher, {reinterpret_cast<const std::uint8_t *>(kPing), sizeof(kPing) - 1});
        ASSERT_FALSE(ping.empty());
        const std::vector<std::uint8_t> opened = open_app_records(server_state.read_cipher, ping);
        ASSERT_EQ(sizeof(kPing) - 1, opened.size());
        EXPECT_EQ(0, std::memcmp(kPing, opened.data(), opened.size()));

        ASSERT_TRUE(open_and_discard(client_state.read_cipher, tail)); // NST at seq 0
        const char kPong[] = "pong ours-0rtt";
        const std::vector<std::uint8_t> pong = seal_app_record(
                server_state.write_cipher, {reinterpret_cast<const std::uint8_t *>(kPong), sizeof(kPong) - 1});
        ASSERT_FALSE(pong.empty());
        const std::vector<std::uint8_t> opened_back = open_app_records(client_state.read_cipher, pong);
        ASSERT_EQ(sizeof(kPong) - 1, opened_back.size());
        EXPECT_EQ(0, std::memcmp(kPong, opened_back.data(), opened_back.size()));
    });
}

// The ticket is 0-RTT-capable but the connection has early data disabled:
// the EE omits early_data, the client's early records are trial-decrypted
// under the handshake keys and dropped (§4.2.10 skip), and the handshake —
// resumption included — completes unaffected.
TEST(TlsServerHandshake13Psk, EarlyDataRejectedWhenDisabled) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        DualMaterial material;
        TlsServerConfig hop1_cfg = material.server_material.config();
        hop1_cfg.enable_early_data = true;
        TestSessionStore store;
        const TlsTicketMinter minter = store.minter_hook();
        const TlsResumptionLookup lookup = store.lookup_hook();

        {
            TlsServerHandshakeEngine server(hop1_cfg, nullptr, &minter);
            TlsClientHandshakeEngine client(material.client_cfg, nullptr);
            ASSERT_TRUE(pump(client, server));
            EXPECT_FALSE(server.failed());
        }
        const TestSessionStore::Entry *entry = store.first_entry();
        ASSERT_NE(nullptr, entry);
        ASSERT_EQ(14336u, entry->max_early_data); // kMaxEarlyDataAccepted (BoringSSL parity)

        TlsServerConfig cfg = material.server_material.config(); // early data OFF
        TlsSessionOffer offer;
        const std::vector<std::uint8_t> ticket = store.first_ticket();
        offer.identity = ticket;
        offer.obfuscated_ticket_age = entry->age_add;
        offer.suite = entry->suite;
        offer.psk = entry->psk;
        offer.max_early_data = entry->max_early_data; // the ticket still advertises it

        TlsServerHandshakeEngine server(cfg, &lookup, &minter);
        TlsClientHandshakeEngine client(material.client_cfg, &offer);
        const std::string_view early_request = "GET /early HTTP/1.1\r\nhost: example.com\r\n\r\n";
        ASSERT_TRUE(client.write_early_data(
                                  {reinterpret_cast<const std::uint8_t *>(early_request.data()), early_request.size()})
                            .has_value());
        std::vector<std::uint8_t> tail; // the post-done NST
        ASSERT_TRUE(pump(client, server, &tail));
        EXPECT_FALSE(server.failed());
        EXPECT_FALSE(client.failed());

        TlsConnectedState server_state = server.take_state();
        TlsConnectedState client_state = client.take_state();
        EXPECT_TRUE(server_state.session_resumed);
        EXPECT_TRUE(client_state.session_resumed);
        EXPECT_FALSE(server_state.early_data_accepted);
        EXPECT_FALSE(client_state.early_data_accepted);
        EXPECT_TRUE(chain_bytes(server.take_early_data()).empty());
    });
}

// 15000 early bytes against the 14336 budget: the record crossing the
// budget is fatal unexpected_message (§4.6.1). The client plays the
// adversarial role — its offer claims a budget the ticket never had.
TEST(TlsServerHandshake13Psk, EarlyDataOverBudgetFatal) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        DualMaterial material;
        TlsServerConfig cfg = material.server_material.config();
        cfg.enable_early_data = true;
        TestSessionStore store;
        const TlsTicketMinter minter = store.minter_hook();
        const TlsResumptionLookup lookup = store.lookup_hook();

        {
            TlsServerHandshakeEngine server(cfg, nullptr, &minter);
            TlsClientHandshakeEngine client(material.client_cfg, nullptr);
            ASSERT_TRUE(pump(client, server));
            EXPECT_FALSE(server.failed());
        }
        const TestSessionStore::Entry *entry = store.first_entry();
        ASSERT_NE(nullptr, entry);

        TlsSessionOffer offer;
        const std::vector<std::uint8_t> ticket = store.first_ticket();
        offer.identity = ticket;
        offer.obfuscated_ticket_age = entry->age_add;
        offer.suite = entry->suite;
        offer.psk = entry->psk;
        offer.max_early_data = 16384 + 1024; // a lying client budget

        TlsServerHandshakeEngine server(cfg, &lookup, &minter);
        TlsClientHandshakeEngine client(material.client_cfg, &offer);
        std::vector<std::uint8_t> big(15000, 'x');
        ASSERT_TRUE(client.write_early_data(big).has_value());
        std::vector<std::uint8_t> tail; // the fatal alert, post client-finish
        ASSERT_TRUE(pump(client, server, &tail));

        EXPECT_TRUE(server.failed());
        EXPECT_EQ(TlsAlertDesc::UnexpectedMessage, server.failure_alert());

        // The client finished before the alert flew (the engine cannot be fed
        // post-done); open the tail record through its read cipher: a two-byte
        // fatal alert carrying unexpected_message.
        TlsConnectedState client_state = client.take_state();
        ASSERT_GE(tail.size(), fiber::tls::kTlsRecordHeaderSize);
        const std::size_t alert_len = (static_cast<std::size_t>(tail[3]) << 8) | tail[4];
        std::array<std::uint8_t, 64> alert_dst{};
        const auto alert = client_state.read_cipher.open(
                fiber::tls::TlsContentType::ApplicationData, (static_cast<std::uint16_t>(tail[1]) << 8) | tail[2],
                static_cast<std::uint16_t>(alert_len), {tail.data() + fiber::tls::kTlsRecordHeaderSize, alert_len},
                alert_dst);
        ASSERT_EQ(fiber::tls::TlsRecordCipher::Status::Ok, alert.status);
        ASSERT_EQ(2u, alert.plain_len);
        EXPECT_EQ(static_cast<std::uint8_t>(TlsAlertDesc::UnexpectedMessage), alert_dst[1]);

        // What sank before the budget broke stays readable — and bounded.
        const std::vector<std::uint8_t> early = chain_bytes(server.take_early_data());
        EXPECT_LE(early.size(), 14336u); // kMaxEarlyDataAccepted
    });
}

// The store misses (unknown ticket): the offer degrades to a full handshake
// — no fatal, no resume, and the NST still mints.
TEST(TlsServerHandshake13Psk, UnknownTicketFallsBackToFull) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        CollectedSessionsGuard guard;
        ServerMaterial material;
        const TlsServerConfig cfg = material.config();
        TestSessionStore store;
        const TlsTicketMinter minter = store.minter_hook();
        const TlsResumptionLookup lookup = store.lookup_hook();

        run_ticketed_hop1(cfg, minter);
        store.miss_everything = true;

        auto client = BoringClient::make(ClientOptions{.collect_tickets = true});
        ASSERT_NE(nullptr, client);
        ASSERT_EQ(1, SSL_set_session(client->ssl(), g_new_sessions.front()));
        TlsServerHandshakeEngine engine(cfg, &lookup, &minter);
        ASSERT_TRUE(drive(*client, engine, false));
        EXPECT_FALSE(engine.failed());

        TlsConnectedState state = engine.take_state();
        EXPECT_FALSE(state.session_resumed);
        EXPECT_EQ(0, SSL_session_reused(client->ssl()));
        EXPECT_EQ(2u, store.size()); // the full handshake minted a fresh ticket
    });
}

// The store's PSK no longer matches the binder the client computed over the
// original: fatal decrypt_error (a binder mismatch is never a fallback).
TEST(TlsServerHandshake13Psk, BinderMismatchFatal) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        CollectedSessionsGuard guard;
        ServerMaterial material;
        const TlsServerConfig cfg = material.config();
        TestSessionStore store;
        const TlsTicketMinter minter = store.minter_hook();
        const TlsResumptionLookup lookup = store.lookup_hook();

        run_ticketed_hop1(cfg, minter);
        store.corrupt_first_psk(); // the client's binder still rides the original PSK

        auto client = BoringClient::make(ClientOptions{.collect_tickets = true});
        ASSERT_NE(nullptr, client);
        ASSERT_EQ(1, SSL_set_session(client->ssl(), g_new_sessions.front()));
        TlsServerHandshakeEngine engine(cfg, &lookup, &minter);
        DriveLog log;
        // drive reports the engine's failure as false — that IS the expectation.
        ASSERT_FALSE(drive(*client, engine, false, &log));

        EXPECT_TRUE(engine.failed());
        EXPECT_EQ(TlsAlertDesc::DecryptError, engine.failure_alert());
    });
}

// The ticket is 5 minutes old against the 60 s skew window: rejected (not
// fatal) — the handshake falls back to a full one.
TEST(TlsServerHandshake13Psk, StaleTicketFallsBackToFull) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        CollectedSessionsGuard guard;
        ServerMaterial material;
        const TlsServerConfig cfg = material.config();
        TestSessionStore store;
        const TlsTicketMinter minter = store.minter_hook();
        const TlsResumptionLookup lookup = store.lookup_hook();

        run_ticketed_hop1(cfg, minter);
        store.age_skew_first(5 * 60 * 1000); // issued 5 min "earlier": server_age = 5 min

        auto client = BoringClient::make(ClientOptions{.collect_tickets = true});
        ASSERT_NE(nullptr, client);
        ASSERT_EQ(1, SSL_set_session(client->ssl(), g_new_sessions.front()));
        TlsServerHandshakeEngine engine(cfg, &lookup, &minter);
        ASSERT_TRUE(drive(*client, engine, false));
        EXPECT_FALSE(engine.failed());

        TlsConnectedState state = engine.take_state();
        EXPECT_FALSE(state.session_resumed);
        EXPECT_EQ(0, SSL_session_reused(client->ssl()));
    });
}

// ---- engine surface ----

TEST(TlsServerHandshakeSurface, NullCredentialFailsConstruction) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        TlsServerConfig cfg; // chain/key null
        TlsServerHandshakeEngine engine(cfg, nullptr, nullptr);
        EXPECT_TRUE(engine.done());
        EXPECT_TRUE(engine.failed());
        EXPECT_EQ(TlsAlertDesc::InternalError, engine.failure_alert());
    });
}

// ---- stateless ticket minting through the real engines (08 slice 1) ----

// Wraps TlsTicketService's minter hook to keep a copy of every minted blob:
// the NST's on-wire bytes are sealed (1.3) or handshake-MAC'd (1.2), so the
// capture is the only place both the blob and its provenance are visible.
struct CapturingServiceMinter {
    TlsTicketService &service;
    std::vector<std::uint8_t> ticket;
    bool called = false;

    static std::size_t mint(void *ctx, const TlsTicketRequest &req, std::span<std::uint8_t> out) noexcept {
        auto &self = *static_cast<CapturingServiceMinter *>(ctx);
        self.called = true;
        const std::size_t len = TlsTicketService::mint_thunk(&self.service, req, out);
        if (len > 0) {
            self.ticket.assign(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(len));
        }
        return len;
    }
    [[nodiscard]] TlsTicketMinter hook() noexcept { return TlsTicketMinter{&mint, this}; }
};

// The full 1.3 handshake mints a stateless ticket carrying the DERIVED psk
// and the CH's SNI in the AAD — the future lookup consumes exactly this.
TEST(TlsServerTicketService, Mint13ThroughEngineBindsSniAndCarriesPsk) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        DualMaterial dual;
        const std::array<TlsTicketKeyMaterial, 1> ticket_keys{{
                {.id = 1,
                 .bytes = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
                           0x10},
                 .created_ms = certfix::kRefNowMs},
        }};
        TlsTicketService service(ticket_keys, TlsTicketKeyPolicy{});
        ASSERT_TRUE(service.valid());
        CapturingServiceMinter capture{.service = service};
        const TlsTicketMinter minter = capture.hook();

        TlsServerHandshakeEngine server(dual.server_material.config(), nullptr, &minter);
        TlsClientHandshakeEngine client(dual.client_cfg, nullptr);
        std::vector<std::uint8_t> tail;
        ASSERT_TRUE(pump(client, server, &tail));
        EXPECT_TRUE(capture.called); // 1.3 always mints one ticket after client Fin
        ASSERT_FALSE(capture.ticket.empty());

        const TlsConnectedState state = server.take_state();
        TlsTicketContents contents;
        ASSERT_EQ(TlsTicketService::OpenStatus::Ok,
                  service.open(capture.ticket, "example.com", certfix::kRefNowMs + 1000, contents));
        EXPECT_EQ(fiber::tls::TlsProtocolVersion::Tls13, contents.version);
        EXPECT_EQ(state.suite, contents.suite);
        EXPECT_EQ("h2", contents.alpn_view());
        EXPECT_EQ(certfix::kRefNowMs, contents.issued_ms);
        EXPECT_EQ(7200u, contents.timeout_s);
        // PSK symmetry with the client's own NST-side derivation.
        const std::array<std::uint8_t, 1> nonce{0};
        const auto psk = fiber::tls::tls13_resumption_psk(state.resumption_master, nonce);
        ASSERT_TRUE(psk.has_value());
        EXPECT_TRUE(secrets_equal(*psk, contents.secret));
        // Another vhost cannot use the ticket.
        TlsTicketContents wrong;
        EXPECT_EQ(TlsTicketService::OpenStatus::Rejected,
                  service.open(capture.ticket, "other.example", certfix::kRefNowMs, wrong));
    });
}

// A TLS 1.2 BoringSSL client that offers session tickets (SNI on the wire)
// gets a stateless ticket carrying the 48-byte master secret.
TEST(TlsServerTicketService, Mint12ThroughEngineCarriesMaster) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        auto client = BoringClient::make(ClientOptions{.tls12_only = true, .send_sni = true});
        ASSERT_NE(nullptr, client);
        ServerMaterial material;
        const std::array<TlsTicketKeyMaterial, 1> ticket_keys{{
                {.id = 1,
                 .bytes = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
                           0x10},
                 .created_ms = certfix::kRefNowMs},
        }};
        TlsTicketService service(ticket_keys, TlsTicketKeyPolicy{});
        CapturingServiceMinter capture{.service = service};
        const TlsTicketMinter minter = capture.hook();

        TlsServerHandshakeEngine engine(material.config(), nullptr, &minter);
        ASSERT_TRUE(drive(*client, engine, false));
        EXPECT_TRUE(capture.called);
        ASSERT_FALSE(capture.ticket.empty());

        const TlsConnectedState state = engine.take_state();
        EXPECT_EQ(fiber::tls::TlsProtocolVersion::Tls12, state.version);

        TlsTicketContents contents;
        ASSERT_EQ(TlsTicketService::OpenStatus::Ok,
                  service.open(capture.ticket, "example.com", certfix::kRefNowMs + 1000, contents));
        EXPECT_EQ(fiber::tls::TlsProtocolVersion::Tls12, contents.version);
        EXPECT_EQ(state.suite, contents.suite);
        EXPECT_EQ("h2", contents.alpn_view());
        ASSERT_EQ(48u, contents.secret.len());
        EXPECT_TRUE(secrets_equal(state.tls12_master, contents.secret));
        // SNI was sent, so the empty name is not this ticket's name.
        TlsTicketContents wrong;
        EXPECT_EQ(TlsTicketService::OpenStatus::Rejected, service.open(capture.ticket, "", certfix::kRefNowMs, wrong));
    });
}

// ---- stateless resumption through the real service (08 lookup half) ----

namespace {

// The shared single-key material for the stateless tests, born at the fixed
// test clock so the default windows cover both hops.
std::array<TlsTicketKeyMaterial, 1> stateless_keys() {
    return {{
            {.id = 1,
             .bytes = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10},
             .created_ms = certfix::kRefNowMs},
    }};
}

// Wraps the service's lookup hook and records what the engine passed: the
// CH's SNI and the engine's clock snapshot.
struct RecordingServiceLookup {
    TlsTicketService &service;
    bool called = false;
    std::string seen_name;
    std::int64_t seen_now_ms = 0;

    static bool lookup(void *ctx, std::span<const std::uint8_t> identity, std::string_view name,
                       std::int64_t now_unix_ms, TlsResumedSession &out) noexcept {
        auto &self = *static_cast<RecordingServiceLookup *>(ctx);
        self.called = true;
        self.seen_name.assign(name);
        self.seen_now_ms = now_unix_ms;
        return TlsTicketService::lookup_thunk(&self.service, identity, name, now_unix_ms, out);
    }
    [[nodiscard]] TlsResumptionLookup hook() noexcept { return TlsResumptionLookup{&lookup, this}; }
};

} // namespace

// The real end-to-end loop: hop 1 mints a stateless ticket with the CH's SNI
// bound into its AAD; BoringSSL digests the NST and derives its PSK the RFC
// way (Expand-Label over the nonce — the mint's exact counterpart); hop 2
// offers the ticket back, the lookup opens it against hop 2's CH SNI, and
// the handshake resumes over the sealed PSK — binder verification included.
TEST(TlsServerTicketService, StatelessResume13WithBoringClient) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        CollectedSessionsGuard guard;
        ServerMaterial material;
        const TlsServerConfig cfg = material.config();
        TlsTicketService service(stateless_keys(), TlsTicketKeyPolicy{});
        RecordingServiceLookup recorder{.service = service};
        const TlsTicketMinter minter = service.minter();
        const TlsResumptionLookup lookup = recorder.hook();

        // hop 1: full handshake; the NST lands in the client's session stash.
        {
            auto client = BoringClient::make(ClientOptions{.collect_tickets = true, .send_sni = true});
            ASSERT_NE(nullptr, client);
            TlsServerHandshakeEngine engine(cfg, nullptr, &minter);
            ASSERT_TRUE(drive(*client, engine, false));
            std::array<char, 64> sink{};
            (void) SSL_read(client->ssl(), sink.data(), static_cast<int>(sink.size()));
            ASSERT_EQ(1u, g_new_sessions.size());
        }

        // hop 2: same SNI, the stashed session set — the PSK offer rides the ticket.
        auto client = BoringClient::make(ClientOptions{.collect_tickets = true, .send_sni = true});
        ASSERT_NE(nullptr, client);
        ASSERT_EQ(1, SSL_set_session(client->ssl(), g_new_sessions.front()));
        TlsServerHandshakeEngine engine(cfg, &lookup, &minter);
        ASSERT_TRUE(drive(*client, engine, false));
        EXPECT_FALSE(engine.failed());

        TlsConnectedState state = engine.take_state();
        EXPECT_TRUE(state.session_resumed);
        EXPECT_EQ(1, SSL_session_reused(client->ssl()));
        EXPECT_TRUE(state.peer_chain.empty()); // PSK resume: no certificate flight
        EXPECT_FALSE(state.early_data_accepted);
        // The engine handed the lookup the CH's SNI and its clock snapshot (the
        // same one the age gate uses).
        EXPECT_TRUE(recorder.called);
        EXPECT_EQ("example.com", recorder.seen_name);
        EXPECT_EQ(certfix::kRefNowMs, recorder.seen_now_ms);

        // The resumed connection minted — and the client digested — a fresh
        // stateless ticket of its own.
        std::array<char, 64> sink{};
        (void) SSL_read(client->ssl(), sink.data(), static_cast<int>(sink.size()));
        EXPECT_EQ(2u, g_new_sessions.size());

        // App data over the resumed connection, both directions.
        const char kPing[] = "ping stateless";
        ASSERT_EQ(sizeof(kPing) - 1, SSL_write(client->ssl(), kPing, static_cast<int>(sizeof(kPing) - 1)));
        const std::vector<std::uint8_t> ping = open_app_records(state.read_cipher, client->drain_wbio());
        ASSERT_EQ(sizeof(kPing) - 1, ping.size());
        EXPECT_EQ(0, std::memcmp(kPing, ping.data(), ping.size()));

        const char kPong[] = "pong stateless";
        const std::vector<std::uint8_t> pong =
                seal_app_record(state.write_cipher, {reinterpret_cast<const std::uint8_t *>(kPong), sizeof(kPong) - 1});
        ASSERT_FALSE(pong.empty());
        ASSERT_TRUE(client->ship(pong));
        const int got = SSL_read(client->ssl(), sink.data(), static_cast<int>(sink.size()));
        ASSERT_GT(got, 0);
        EXPECT_EQ(0, std::memcmp(kPong, sink.data(), sizeof(kPong) - 1));
    });
}

// The wrong-vhost fallback: BoringSSL offers the ticket regardless of the new
// SNI (its session use is not name-gated), the lookup opens it against hop 2's
// CH SNI, and the AAD name binding rejects it — the handshake degrades to a
// full one, never a failure. Verification is off: the credential is
// example.com's; the test is the ticket, not the chain.
TEST(TlsServerTicketService, CrossVhostTicketFallsBackToFull) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        CollectedSessionsGuard guard;
        ServerMaterial material;
        const TlsServerConfig cfg = material.config();
        TlsTicketService service(stateless_keys(), TlsTicketKeyPolicy{});
        RecordingServiceLookup recorder{.service = service};
        const TlsTicketMinter minter = service.minter();
        const TlsResumptionLookup lookup = recorder.hook();

        // hop 1 mints under "example.com".
        {
            auto client = BoringClient::make(ClientOptions{.collect_tickets = true, .send_sni = true});
            ASSERT_NE(nullptr, client);
            TlsServerHandshakeEngine engine(cfg, nullptr, &minter);
            ASSERT_TRUE(drive(*client, engine, false));
            std::array<char, 64> sink{};
            (void) SSL_read(client->ssl(), sink.data(), static_cast<int>(sink.size()));
            ASSERT_EQ(1u, g_new_sessions.size());
        }

        // hop 2: a different vhost presents the ticket.
        auto client = BoringClient::make(ClientOptions{
                .trust_pem = nullptr, .host = "other.example", .collect_tickets = true, .send_sni = true});
        ASSERT_NE(nullptr, client);
        ASSERT_EQ(1, SSL_set_session(client->ssl(), g_new_sessions.front()));
        TlsServerHandshakeEngine engine(cfg, &lookup, &minter);
        ASSERT_TRUE(drive(*client, engine, false));
        EXPECT_FALSE(engine.failed());

        TlsConnectedState state = engine.take_state();
        EXPECT_FALSE(state.session_resumed);
        EXPECT_EQ(0, SSL_session_reused(client->ssl()));
        // The lookup ran and saw the new vhost's SNI — the miss is the AAD
        // binding, not a skipped offer.
        EXPECT_TRUE(recorder.called);
        EXPECT_EQ("other.example", recorder.seen_name);
    });
}

// Both FSMs ours with the real service: hop 1's captured ticket opens back
// into the exact offer a future 08 client-side cache would build from the
// NST side, hop 2 resumes through lookup(), and the pair re-derives one set
// of application secrets.
TEST(TlsServerTicketService, StatelessResumeOurPair) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        DualMaterial dual;
        const TlsServerConfig cfg = dual.server_material.config();
        TlsTicketService service(stateless_keys(), TlsTicketKeyPolicy{});
        CapturingServiceMinter capture{.service = service};
        const TlsTicketMinter minter = capture.hook();

        // hop 1: mint exactly one stateless ticket.
        {
            TlsServerHandshakeEngine server(cfg, nullptr, &minter);
            TlsClientHandshakeEngine client(dual.client_cfg, nullptr);
            ASSERT_TRUE(pump(client, server));
            EXPECT_FALSE(server.failed());
        }
        ASSERT_TRUE(capture.called);
        const std::vector<std::uint8_t> ticket = capture.ticket; // copy: hop 2 re-mints into the capture

        // The offer mirrors the opened ticket (elapsed 0 → the obfuscated age is
        // the age_add) — psk included, straight out of the sealed payload.
        TlsTicketContents contents;
        ASSERT_EQ(TlsTicketService::OpenStatus::Ok, service.open(ticket, "example.com", certfix::kRefNowMs, contents));
        TlsSessionOffer offer;
        offer.identity = ticket;
        offer.obfuscated_ticket_age = contents.ticket_age_add;
        offer.suite = contents.suite;
        offer.psk = contents.secret.bytes();
        offer.max_early_data = contents.max_early_data;

        const TlsResumptionLookup lookup = service.lookup();
        TlsServerHandshakeEngine server(cfg, &lookup, &minter);
        TlsClientHandshakeEngine client(dual.client_cfg, &offer);
        std::vector<std::uint8_t> tail; // the post-done NST
        ASSERT_TRUE(pump(client, server, &tail));
        EXPECT_FALSE(server.failed());
        EXPECT_FALSE(client.failed());

        TlsConnectedState server_state = server.take_state();
        TlsConnectedState client_state = client.take_state();
        EXPECT_TRUE(server_state.session_resumed);
        EXPECT_TRUE(client_state.session_resumed);
        EXPECT_FALSE(server_state.early_data_accepted);
        EXPECT_TRUE(secrets_equal(client_state.client_app_secret, server_state.client_app_secret));
        EXPECT_TRUE(secrets_equal(client_state.server_app_secret, server_state.server_app_secret));

        // The resumed connection minted another (distinct-nonce) ticket.
        EXPECT_TRUE(capture.called);
        ASSERT_FALSE(capture.ticket.empty());
        EXPECT_NE(ticket, capture.ticket);
        TlsTicketContents reissued;
        EXPECT_EQ(TlsTicketService::OpenStatus::Ok,
                  service.open(capture.ticket, "example.com", certfix::kRefNowMs, reissued));

        // App data through the moved ciphers, both directions.
        const char kPing[] = "ping ours-stateless";
        const std::vector<std::uint8_t> ping = seal_app_record(
                client_state.write_cipher, {reinterpret_cast<const std::uint8_t *>(kPing), sizeof(kPing) - 1});
        ASSERT_FALSE(ping.empty());
        const std::vector<std::uint8_t> opened = open_app_records(server_state.read_cipher, ping);
        ASSERT_EQ(sizeof(kPing) - 1, opened.size());
        EXPECT_EQ(0, std::memcmp(kPing, opened.data(), opened.size()));

        ASSERT_TRUE(open_and_discard(client_state.read_cipher, tail)); // the NST at seq 0
        const char kPong[] = "pong ours-stateless";
        const std::vector<std::uint8_t> pong = seal_app_record(
                server_state.write_cipher, {reinterpret_cast<const std::uint8_t *>(kPong), sizeof(kPong) - 1});
        ASSERT_FALSE(pong.empty());
        const std::vector<std::uint8_t> opened_back = open_app_records(client_state.read_cipher, pong);
        ASSERT_EQ(sizeof(kPong) - 1, opened_back.size());
        EXPECT_EQ(0, std::memcmp(kPong, opened_back.data(), opened_back.size()));
    });
}

// The 1.2 mirror: hop 1 mints a master-bearing stateless ticket (EMS
// negotiated — the mint gate), hop 2 presents it in the CH's session_ticket
// extension with the stashed random sid, and the abbreviated handshake runs
// on the reused master: the SH echoes the sid, no certificate or key
// exchange flies, and the server Finished goes FIRST — the client's MACs it
// in. App data both ways proves the re-derived key block matches.
TEST(TlsServerTicketService, StatelessResume12WithBoringClient) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        CollectedSessionsGuard guard;
        ServerMaterial material;
        const TlsServerConfig cfg = material.config();
        TlsTicketService service(stateless_keys(), TlsTicketKeyPolicy{});
        RecordingServiceLookup recorder{.service = service};
        const TlsTicketMinter minter = service.minter();
        const TlsResumptionLookup lookup = recorder.hook();

        // hop 1 (1.2): full handshake; the cleartext NST lands in the stash.
        {
            auto client =
                    BoringClient::make(ClientOptions{.tls12_only = true, .collect_tickets = true, .send_sni = true});
            ASSERT_NE(nullptr, client);
            TlsServerHandshakeEngine engine(cfg, nullptr, &minter);
            ASSERT_TRUE(drive(*client, engine, false));
            std::array<char, 64> sink{};
            (void) SSL_read(client->ssl(), sink.data(), static_cast<int>(sink.size()));
            ASSERT_EQ(1u, g_new_sessions.size());
        }

        auto client = BoringClient::make(ClientOptions{.tls12_only = true, .collect_tickets = true, .send_sni = true});
        ASSERT_NE(nullptr, client);
        ASSERT_EQ(1, SSL_set_session(client->ssl(), g_new_sessions.front()));
        TlsServerHandshakeEngine engine(cfg, &lookup, &minter);
        ASSERT_TRUE(drive(*client, engine, false));
        EXPECT_FALSE(engine.failed());

        TlsConnectedState state = engine.take_state();
        EXPECT_EQ(fiber::tls::TlsProtocolVersion::Tls12, state.version);
        EXPECT_TRUE(state.session_resumed);
        EXPECT_EQ(1, SSL_session_reused(client->ssl()));
        EXPECT_TRUE(state.peer_chain.empty()); // abbreviated: no certificate flight
        EXPECT_TRUE(recorder.called);
        EXPECT_EQ("example.com", recorder.seen_name);
        EXPECT_EQ(certfix::kRefNowMs, recorder.seen_now_ms);

        // The abbreviated flight rotated the ticket (fresh nonce over the same
        // master) — the NST rides before the CCS, so drive() already digested it.
        std::array<char, 64> sink{};
        (void) SSL_read(client->ssl(), sink.data(), static_cast<int>(sink.size()));
        EXPECT_EQ(2u, g_new_sessions.size());

        // App data over the resumed connection, both directions.
        const char kPing[] = "ping stateless12";
        ASSERT_EQ(sizeof(kPing) - 1, SSL_write(client->ssl(), kPing, static_cast<int>(sizeof(kPing) - 1)));
        const std::vector<std::uint8_t> ping = open_app_records(state.read_cipher, client->drain_wbio());
        ASSERT_EQ(sizeof(kPing) - 1, ping.size());
        EXPECT_EQ(0, std::memcmp(kPing, ping.data(), ping.size()));

        const char kPong[] = "pong stateless12";
        const std::vector<std::uint8_t> pong =
                seal_app_record(state.write_cipher, {reinterpret_cast<const std::uint8_t *>(kPong), sizeof(kPong) - 1});
        ASSERT_FALSE(pong.empty());
        ASSERT_TRUE(client->ship(pong));
        const int got = SSL_read(client->ssl(), sink.data(), static_cast<int>(sink.size()));
        ASSERT_GT(got, 0);
        EXPECT_EQ(0, std::memcmp(kPong, sink.data(), sizeof(kPong) - 1));
    });
}

// The 1.2 wrong-vhost fallback: the AAD name binding misses inside the
// lookup, the handshake degrades to a full 1.2 one (the client re-keys from
// scratch), and the miss is attributed to the binding, not a skipped offer.
TEST(TlsServerTicketService, CrossVhostTicket12FallsBackToFull) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        CollectedSessionsGuard guard;
        ServerMaterial material;
        const TlsServerConfig cfg = material.config();
        TlsTicketService service(stateless_keys(), TlsTicketKeyPolicy{});
        RecordingServiceLookup recorder{.service = service};
        const TlsTicketMinter minter = service.minter();
        const TlsResumptionLookup lookup = recorder.hook();

        // hop 1 mints under "example.com".
        {
            auto client =
                    BoringClient::make(ClientOptions{.tls12_only = true, .collect_tickets = true, .send_sni = true});
            ASSERT_NE(nullptr, client);
            TlsServerHandshakeEngine engine(cfg, nullptr, &minter);
            ASSERT_TRUE(drive(*client, engine, false));
            std::array<char, 64> sink{};
            (void) SSL_read(client->ssl(), sink.data(), static_cast<int>(sink.size()));
            ASSERT_EQ(1u, g_new_sessions.size());
        }

        // hop 2: a different vhost presents the ticket.
        auto client = BoringClient::make(
                ClientOptions{.trust_pem = nullptr, .host = "other.example", .tls12_only = true, .send_sni = true});
        ASSERT_NE(nullptr, client);
        ASSERT_EQ(1, SSL_set_session(client->ssl(), g_new_sessions.front()));
        TlsServerHandshakeEngine engine(cfg, &lookup, &minter);
        ASSERT_TRUE(drive(*client, engine, false));
        EXPECT_FALSE(engine.failed());

        TlsConnectedState state = engine.take_state();
        EXPECT_EQ(fiber::tls::TlsProtocolVersion::Tls12, state.version);
        EXPECT_FALSE(state.session_resumed);
        EXPECT_EQ(0, SSL_session_reused(client->ssl()));
        EXPECT_TRUE(recorder.called);
        EXPECT_EQ("other.example", recorder.seen_name);
    });
}

// =====================================================================
// 09 §4.1 — per-ClientHello config selection, §4.2 — version bounds
// =====================================================================

namespace {

using fiber::tls::TlsServerConfigSource;

// Selector harness: copies the decoded ClientHello view out of the call
// (the engine's retained bytes are only guaranteed for the call) and
// answers from caller staging.
struct SelectorState {
    const TlsServerConfig *answer = nullptr;
    bool called = false;
    bool seen_has_name = false;
    std::string seen_name;
    std::vector<std::string> seen_alpn;

    static const TlsServerConfig *select(void *ctx, const fiber::tls::TlsClientHello &ch) noexcept {
        auto *self = static_cast<SelectorState *>(ctx);
        self->called = true;
        self->seen_has_name = ch.has_server_name;
        self->seen_name.assign(ch.server_name);
        self->seen_alpn.clear();
        for (std::size_t off = 0; off + 1 < ch.alpn_list.size();) {
            const std::size_t len = ch.alpn_list[off];
            if (off + 1 + len > ch.alpn_list.size()) {
                break;
            }
            self->seen_alpn.emplace_back(reinterpret_cast<const char *>(ch.alpn_list.data()) + off + 1, len);
            off += 1 + len;
        }
        return self->answer;
    }

    [[nodiscard]] TlsServerConfigSource source() noexcept {
        return TlsServerConfigSource{&SelectorState::select, this};
    }
};

} // namespace

// The selected config drives the connection: the selector reads SNI + ALPN
// off the ClientHello and hands back a staged config; the handshake that
// follows is an ordinary full 1.3 one against the BoringSSL client.
TEST(TlsServerHandshakeSelector, SelectsConfigFromClientHello) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        auto client = BoringClient::make(ClientOptions{.send_sni = true});
        ASSERT_NE(nullptr, client);
        ServerMaterial material;
        const TlsServerConfig staged = material.config();
        SelectorState selector{.answer = &staged};
        const TlsServerConfigSource source = selector.source();

        TlsServerHandshakeEngine engine(material.config(), nullptr, nullptr, &source);
        ASSERT_TRUE(drive(*client, engine, false));

        EXPECT_TRUE(selector.called);
        EXPECT_TRUE(selector.seen_has_name);
        EXPECT_EQ("example.com", selector.seen_name);
        ASSERT_EQ(2u, selector.seen_alpn.size());
        EXPECT_EQ("h2", selector.seen_alpn[0]);
        EXPECT_EQ("http/1.1", selector.seen_alpn[1]);

        TlsConnectedState state = engine.take_state();
        EXPECT_EQ(fiber::tls::TlsProtocolVersion::Tls13, state.version);
    });
}

// A null answer is the "no vhost for this SNI" case: handshake_failure,
// nothing else runs.
TEST(TlsServerHandshakeSelector, NullAnswerRefusesWithHandshakeFailure) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        auto client = BoringClient::make(ClientOptions{.send_sni = true});
        ASSERT_NE(nullptr, client);
        ServerMaterial material;
        SelectorState selector{.answer = nullptr}; // the hello selects none
        const TlsServerConfigSource source = selector.source();

        TlsServerHandshakeEngine engine(material.config(), nullptr, nullptr, &source);
        (void) drive(*client, engine, false); // the refusal is the expected path
        ASSERT_TRUE(engine.done());
        EXPECT_TRUE(engine.failed());
        EXPECT_EQ(TlsAlertDesc::HandshakeFailure, engine.failure_alert());
        EXPECT_TRUE(selector.called);
    });
}

// The 1.2 fork gate: a tls12_only client against a config whose floor is
// 1.3 answers protocol_version (the 07 behavior only when the window lets
// 1.2 through).
TEST(TlsServerHandshakeBounds, Tls12ClientBelowFloorRefused) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        auto client = BoringClient::make(ClientOptions{.tls12_only = true});
        ASSERT_NE(nullptr, client);
        ServerMaterial material;
        TlsServerConfig cfg = material.config();
        cfg.min_version = fiber::tls::kTlsVersionTls13;

        TlsServerHandshakeEngine engine(cfg, nullptr, nullptr);
        (void) drive(*client, engine, false); // the refusal is the expected path
        ASSERT_TRUE(engine.done());
        EXPECT_TRUE(engine.failed());
        EXPECT_EQ(TlsAlertDesc::ProtocolVersion, engine.failure_alert());
    });
}
