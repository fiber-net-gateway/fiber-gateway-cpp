// TlsConnection (feature/tls/09 §3, slice 1): the connected-phase record
// engine built from a TlsConnectedState. Coverage:
//   - our 06 client ↔ 07 server pair pumped to done, then TWO connections
//     talking to each other: app data both ways, record splitting, ALPN,
//     close_notify both directions (plaintext delivered in the same feed as
//     the close_notify still reads first)
//   - TLS 1.3 post-handshake dispatch: KeyUpdate read-side rekey +
//     update_requested passive response (the full loop through BOTH
//     connections — the response flies under the OLD write keys, the
//     rotation applies to the records after it), update_not_requested (no
//     response, the peer's write keys unchanged), NewSessionTicket
//     swallowed, peer fatal alert latched, corrupted record →
//     bad_record_mac (our own alert emitted), post-handshake CCS fatal
//   - engine take_inbound_leftover: bytes fed past the terminal event ride
//     into the glue-side reader and reassemble with the follow-up feed
//   - TLS 1.2: a synthetic key-block pair (dispatch coverage without an
//     engine) — handshake message after the handshake fatal (09 §1
//     decision 3: HelloRequest/renegotiation refused), app data + SEALED
//     close_notify both ways
//   - BoringSSL interop over memory BIOs: 1.3 app round trip + close_notify
//     + SSL_key_update(REQUESTED) verifying our response ordering against a
//     real peer; 1.2 app round trip + sealed close_notify both ways.

#include <gtest/gtest.h>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
#include <fiber/tls/TlsConnection.h>
#include <fiber/tls/crypto/Tls12KeySchedule.h>
#include <fiber/tls/crypto/Tls13KeySchedule.h>
#include <fiber/tls/handshake/TlsClientHandshakeEngine.h>
#include <fiber/tls/handshake/TlsHandshakeMessage.h>
#include <fiber/tls/handshake/TlsServerHandshakeEngine.h>
#include <fiber/tls/record/TlsRecord.h>
#include <fiber/tls/record/TlsRecordCipher.h>
#include <fiber/tls/record/TlsRecordReader.h>
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
using fiber::tls::TlsConnection;
using fiber::tls::TlsConnectionRole;
using fiber::tls::TlsContentType;
using fiber::tls::TlsHandshakeType;
using fiber::tls::TlsPrivateKey;
using fiber::tls::TlsProtocolVersion;
using fiber::tls::TlsRecordCipher;
using fiber::tls::TlsRecordProtectionKind;
using fiber::tls::TlsSecret;
using fiber::tls::TlsServerConfig;
using fiber::tls::TlsServerHandshakeEngine;
using fiber::tls::TlsTrustStore;
using ClientEvent = TlsClientHandshakeEngine::Event;
using ServerEvent = TlsServerHandshakeEngine::Event;
using ReadStatus = TlsConnection::ReadStatus;

constexpr std::uint8_t kTypeChangeCipherSpec = 20;
constexpr std::uint8_t kTypeAlert = 21;
constexpr std::uint8_t kTypeHandshake = 22;
constexpr std::uint8_t kTypeApplicationData = 23;

// ---- BoringSSL loading helpers (test-only OpenSSL usage) ----

X509 *load_cert(const char *pem) {
    BIO *bio = BIO_new_mem_buf(pem, -1);
    X509 *cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return cert;
}

struct BoringClientOptions {
    bool tls12_only = false;
};

// An SSL client over memory BIOs (the 07 engine-test harness, trimmed to
// what the connection tests need).
class BoringClient {
public:
    static std::unique_ptr<BoringClient> make(const BoringClientOptions &opt = {}) {
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
        X509 *root = load_cert(certfix::kRootRsaPem);
        if (root == nullptr || X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx), root) != 1) {
            SSL_CTX_free(ctx);
            return nullptr;
        }
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        std::unique_ptr<BoringClient> client(new BoringClient(ctx));
        client->ssl_ = SSL_new(ctx);
        if (client->ssl_ == nullptr) {
            return nullptr;
        }
        SSL_set_connect_state(client->ssl_);
        client->rbio_ = BIO_new(BIO_s_mem());
        client->wbio_ = BIO_new(BIO_s_mem());
        SSL_set_bio(client->ssl_, client->rbio_, client->wbio_);
        if (SSL_set1_host(client->ssl_, "example.com") != 1) {
            return nullptr;
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

    // 1 = handshake complete, 0 = progress expected, -1 = the client failed.
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

// ---- shared helpers ----

std::vector<std::uint8_t> chain_bytes(const IoBufChain &chain) {
    std::vector<std::uint8_t> out;
    out.reserve(chain.readable_bytes());
    for (const fiber::mem::IoBufNode *node = chain.front_node(); node != nullptr; node = node->next) {
        const std::uint8_t *data = node->buf.readable_data();
        out.insert(out.end(), data, data + node->buf.readable());
    }
    return out;
}

bool feed_engine_bytes(TlsServerHandshakeEngine &engine, std::span<const std::uint8_t> bytes, ServerEvent &last) {
    last = ServerEvent::None;
    std::size_t off = 0;
    while (off < bytes.size() && !engine.done()) {
        IoBuf buf = IoBuf::allocate(bytes.size() - off);
        if (!buf.valid()) {
            EXPECT_TRUE(false);
            return false;
        }
        std::memcpy(buf.writable_data(), bytes.data() + off, bytes.size() - off);
        buf.commit(bytes.size() - off);
        const IoResult<ServerEvent> event = engine.feed(std::move(buf));
        if (!event.has_value()) {
            EXPECT_TRUE(false);
            return false;
        }
        last = *event;
        off += bytes.size() - off;
    }
    return true;
}

// Bi-directional pump until the engine reaches its terminal state.
bool drive(BoringClient &client, TlsServerHandshakeEngine &engine) {
    for (int spin = 0; spin < 256; ++spin) {
        const std::vector<std::uint8_t> out = chain_bytes(engine.take_output());
        if (!out.empty() && !client.ship(out)) {
            EXPECT_TRUE(false);
            return false;
        }
        if (engine.done()) {
            return !engine.failed();
        }
        if (!static_cast<bool>(SSL_is_init_finished(client.ssl())) && client.handshake_step() < 0) {
            EXPECT_TRUE(false);
            ERR_print_errors_fp(stderr);
            return false;
        }
        const std::vector<std::uint8_t> flight = client.drain_wbio();
        if (flight.empty()) {
            break;
        }
        ServerEvent event = ServerEvent::None;
        if (!feed_engine_bytes(engine, flight, event)) {
            return false;
        }
    }
    return engine.done() && !engine.failed();
}

struct ServerMaterial {
    std::string chain_pem;
    std::optional<TlsCertificateChain> chain;
    std::optional<TlsPrivateKey> key;
    std::vector<std::string_view> alpn{"h2", "http/1.1"};

    ServerMaterial() {
        chain_pem.assign(certfix::kLeafRsaPem);
        chain_pem.append(certfix::kIntermediateRsaPem);
        auto parsed = TlsCertificateChain::parse_pem_bundle({chain_pem.data(), chain_pem.size()});
        EXPECT_TRUE(parsed.has_value());
        if (parsed.has_value()) {
            chain = std::move(parsed).value();
        }
        auto parsed_key = TlsPrivateKey::parse_pem({certfix::kRsa2048KeyPem, std::strlen(certfix::kRsa2048KeyPem)});
        EXPECT_TRUE(parsed_key.has_value());
        if (parsed_key.has_value()) {
            key = std::move(parsed_key).value();
        }
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

// Our 06 client engine against our 07 server engine. When `tail` is
// non-null, its bytes ride the client's FINAL flight (its Finished — the
// take where the client engine is newly done): the server consumes the
// Fin, reaches done, and the tail stays unconsumed for
// take_inbound_leftover().
bool drive_pair(TlsClientHandshakeEngine &client, TlsServerHandshakeEngine &server,
                const std::vector<std::uint8_t> *tail = nullptr) {
    bool tail_sent = false;
    for (int spin = 0; spin < 64 && !(server.done() && client.done()); ++spin) {
        std::vector<std::uint8_t> to_server = chain_bytes(client.take_output());
        if (!to_server.empty()) {
            if (client.done() && tail != nullptr && !tail_sent) {
                to_server.insert(to_server.end(), tail->begin(), tail->end());
                tail_sent = true;
            }
            IoBuf buf = IoBuf::allocate(to_server.size());
            EXPECT_TRUE(buf.valid());
            if (!buf.valid()) {
                return false;
            }
            std::memcpy(buf.writable_data(), to_server.data(), to_server.size());
            buf.commit(to_server.size());
            EXPECT_TRUE(server.feed(std::move(buf)).has_value());
        }
        if (server.done() && client.done()) {
            break;
        }
        const std::vector<std::uint8_t> to_client = chain_bytes(server.take_output());
        if (!to_client.empty()) {
            IoBuf buf = IoBuf::allocate(to_client.size());
            EXPECT_TRUE(buf.valid());
            if (!buf.valid()) {
                return false;
            }
            std::memcpy(buf.writable_data(), to_client.data(), to_client.size());
            buf.commit(to_client.size());
            EXPECT_TRUE(client.feed(std::move(buf)).has_value());
        }
    }
    return server.done() && client.done() && !server.failed() && !client.failed();
}

// Runs our pair to completion and hands back both connected states.
struct PairStates {
    TlsConnectedState client;
    TlsConnectedState server;
};

PairStates complete_pair(ServerMaterial &material) {
    auto trust = TlsTrustStore::from_pem_bundle({certfix::kRootRsaPem, std::strlen(certfix::kRootRsaPem)});
    EXPECT_TRUE(trust.has_value());

    TlsClientConfig client_cfg;
    client_cfg.sni_host = "example.com";
    client_cfg.alpn = material.alpn;
    client_cfg.trust = &*trust;
    client_cfg.now_unix_ms = certfix::kRefNowMs;

    TlsServerHandshakeEngine server(material.config(), nullptr, nullptr);
    TlsClientHandshakeEngine client(client_cfg, nullptr);
    EXPECT_TRUE(drive_pair(client, server));

    PairStates states;
    states.client = client.take_state();
    states.server = server.take_state();
    return states;
}

// 5-byte record framing around a sealed payload; `outer_type` is 23 (1.3
// rewrites every sealed record) or the payload's true type (1.2 preserves
// it).
std::vector<std::uint8_t> seal_record(TlsRecordCipher &cipher, TlsContentType inner,
                                      std::span<const std::uint8_t> plain, std::uint8_t outer_type) {
    const std::size_t sealed = cipher.seal_output_size(plain.size());
    std::vector<std::uint8_t> out(fiber::tls::kTlsRecordHeaderSize + sealed);
    out[0] = outer_type;
    out[1] = 0x03;
    out[2] = 0x03;
    out[3] = static_cast<std::uint8_t>(sealed >> 8);
    out[4] = static_cast<std::uint8_t>(sealed);
    const auto result = cipher.seal(inner, plain, {out.data() + fiber::tls::kTlsRecordHeaderSize, sealed});
    EXPECT_EQ(TlsRecordCipher::Status::Ok, result.status);
    return out;
}

// Seals one handshake message with the state's write cipher (sequence
// continuity preserved by the move-out/move-back), framed as outer
// app-data — the wire a 1.3 peer sends for post-handshake messages.
std::vector<std::uint8_t> craft_hs_record(TlsConnectedState &state, std::span<const std::uint8_t> message) {
    TlsRecordCipher wc = std::move(state.write_cipher);
    std::vector<std::uint8_t> wire = seal_record(wc, TlsContentType::Handshake, message, kTypeApplicationData);
    state.write_cipher = std::move(wc);
    return wire;
}

// Seals a 1.2 record with the CLIENT-side write keys of a synthetic pair
// (outer type preserved — 1.2 binds it into the AAD).
std::vector<std::uint8_t> craft_12_record(TlsRecordCipher &client_write, TlsContentType inner,
                                          std::span<const std::uint8_t> plain) {
    return seal_record(client_write, inner, plain, static_cast<std::uint8_t>(inner));
}

// Emits a KeyUpdate from the client state AND rotates its write side one
// generation — exactly what the sender of a KeyUpdate must do (RFC 8446
// §4.6.1: the message flies under the current keys, the next records under
// the rotated ones).
std::vector<std::uint8_t> craft_key_update(TlsConnectedState &client_state, bool request_update) {
    const std::uint8_t key_update[5] = {static_cast<std::uint8_t>(TlsHandshakeType::KeyUpdate), 0, 0, 1,
                                        static_cast<std::uint8_t>(request_update ? 1 : 0)};
    std::vector<std::uint8_t> wire = craft_hs_record(client_state, key_update);
    auto next = fiber::tls::tls13_key_update(client_state.client_app_secret);
    EXPECT_TRUE(next.has_value());
    auto keys = fiber::tls::tls13_traffic_keys(*next, client_state.suite);
    EXPECT_TRUE(keys.has_value());
    TlsRecordCipher fresh;
    EXPECT_TRUE(fresh.init(client_state.suite, TlsRecordProtectionKind::Tls13, {keys->key.data(), keys->key_len},
                           {keys->iv.data(), keys->iv_len})
                        .has_value());
    client_state.client_app_secret = std::move(*next);
    client_state.write_cipher = std::move(fresh);
    return wire;
}

// Frames `payload` as records through a connection's write path and returns
// the wire (header-walkable to count records).
std::vector<std::uint8_t> connection_wire(TlsConnection &conn, std::span<const std::uint8_t> payload) {
    EXPECT_TRUE(conn.write(payload).has_value());
    return chain_bytes(conn.take_output());
}

std::size_t count_records(const std::vector<std::uint8_t> &wire) {
    std::size_t records = 0;
    std::size_t off = 0;
    while (off + fiber::tls::kTlsRecordHeaderSize <= wire.size()) {
        const std::size_t len = (static_cast<std::size_t>(wire[off + 3]) << 8) | wire[off + 4];
        off += fiber::tls::kTlsRecordHeaderSize + len;
        ++records;
    }
    EXPECT_EQ(wire.size(), off); // the walk consumed the wire exactly
    return records;
}

// The glue's connected-phase framing (TlsStreamFd's contract): one reader
// per inbound direction — complete records out, a partial record tail
// buffered across feeds — each record handed to the connection.
struct WireFeeder {
    fiber::tls::TlsRecordReader reader{};

    bool feed(TlsConnection &conn, std::span<const std::uint8_t> wire) {
        if (wire.empty()) {
            return true;
        }
        IoBuf buf = IoBuf::allocate(wire.size());
        if (!buf.valid()) {
            return false;
        }
        std::memcpy(buf.writable_data(), wire.data(), wire.size());
        buf.commit(wire.size());
        if (!reader.feed(std::move(buf))) {
            return false;
        }
        for (;;) {
            fiber::tls::TlsRecordReader::Result next = reader.next();
            if (next.status == fiber::tls::TlsRecordReader::Result::Status::Fatal) {
                conn.on_framing_fatal(next.alert);
                return true;
            }
            if (next.status == fiber::tls::TlsRecordReader::Result::Status::NeedMore) {
                return true; // partial tail stays buffered for the next feed
            }
            conn.on_record(std::move(next.record));
            if (conn.failed() || conn.peer_closed()) {
                return true; // terminal: further records drop
            }
        }
    }
};

std::vector<std::uint8_t> read_all(TlsConnection &conn) {
    std::vector<std::uint8_t> out;
    std::array<std::uint8_t, 4096> buf{};
    for (;;) {
        std::size_t n = 0;
        const ReadStatus status = conn.read(buf.data(), buf.size(), n);
        if (status != ReadStatus::Ok || n == 0) {
            break;
        }
        out.insert(out.end(), buf.data(), buf.data() + n);
    }
    return out;
}

// A full 1.2-connected pair without an engine: a known master secret
// expands into both directions' write keys. Dispatch coverage (HelloRequest
// fatal, app data, sealed alerts) needs no handshake on the wire.
struct Synthetic12Pair {
    TlsConnectedState client;
    TlsConnectedState server;
};

Synthetic12Pair make_synthetic_12_pair() {
    const TlsCipherSuiteId suite = TlsCipherSuiteId::EcdheRsaAes128GcmSha256;
    std::array<std::uint8_t, 48> master_bytes{};
    for (std::size_t i = 0; i < master_bytes.size(); ++i) {
        master_bytes[i] = static_cast<std::uint8_t>(i * 5 + 1);
    }
    TlsSecret master = TlsSecret::from_bytes(master_bytes);
    std::array<std::uint8_t, 32> client_random{};
    std::array<std::uint8_t, 32> server_random{};
    for (std::size_t i = 0; i < 32; ++i) {
        client_random[i] = static_cast<std::uint8_t>(0x10 + i);
        server_random[i] = static_cast<std::uint8_t>(0xF0 - i);
    }
    auto block = fiber::tls::tls12_key_block(suite, master, client_random, server_random);
    EXPECT_TRUE(block.has_value());

    Synthetic12Pair pair;
    pair.client.version = TlsProtocolVersion::Tls12;
    pair.client.suite = suite;
    EXPECT_TRUE(pair.client.read_cipher
                        .init(suite, TlsRecordProtectionKind::Tls12, {block->server.key.data(), block->server.key_len},
                              {block->server.iv.data(), block->server.iv_len})
                        .has_value());
    EXPECT_TRUE(pair.client.write_cipher
                        .init(suite, TlsRecordProtectionKind::Tls12, {block->client.key.data(), block->client.key_len},
                              {block->client.iv.data(), block->client.iv_len})
                        .has_value());
    pair.server.version = TlsProtocolVersion::Tls12;
    pair.server.suite = suite;
    EXPECT_TRUE(pair.server.read_cipher
                        .init(suite, TlsRecordProtectionKind::Tls12, {block->client.key.data(), block->client.key_len},
                              {block->client.iv.data(), block->client.iv_len})
                        .has_value());
    EXPECT_TRUE(pair.server.write_cipher
                        .init(suite, TlsRecordProtectionKind::Tls12, {block->server.key.data(), block->server.key_len},
                              {block->server.iv.data(), block->server.iv_len})
                        .has_value());
    return pair;
}

} // namespace

// =====================================================================
// our pair → two connections
// =====================================================================

TEST(TlsConnectionTest, AppRoundTripAndAlpn13) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        ServerMaterial material;
        PairStates states = complete_pair(material);
        ASSERT_EQ(TlsProtocolVersion::Tls13, states.server.version);

        TlsConnection client(TlsConnectionRole::Client, std::move(states.client));
        TlsConnection server(TlsConnectionRole::Server, std::move(states.server));
        ASSERT_FALSE(client.failed());
        ASSERT_FALSE(server.failed());
        EXPECT_EQ("h2", std::string_view(reinterpret_cast<const char *>(client.alpn().data()), client.alpn().size()));
        EXPECT_EQ("h2", std::string_view(reinterpret_cast<const char *>(server.alpn().data()), server.alpn().size()));

        WireFeeder server_feeds;
        WireFeeder client_feeds;

        // client → server (span write)
        const std::vector<std::uint8_t> ping{'p', 'i', 'n', 'g'};
        ASSERT_TRUE(server_feeds.feed(server, connection_wire(client, ping)));
        EXPECT_EQ(ping, read_all(server));

        // server → client (span write)
        const std::vector<std::uint8_t> pong{'p', 'o', 'n', 'g'};
        ASSERT_TRUE(server.write(pong).has_value());
        ASSERT_TRUE(client_feeds.feed(client, chain_bytes(server.take_output())));
        EXPECT_EQ(pong, read_all(client));
    });
}

TEST(TlsConnectionTest, LargePayloadSplitsIntoRecords13) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        ServerMaterial material;
        PairStates states = complete_pair(material);

        TlsConnection client(TlsConnectionRole::Client, std::move(states.client));
        TlsConnection server(TlsConnectionRole::Server, std::move(states.server));

        std::vector<std::uint8_t> big(40000);
        for (std::size_t i = 0; i < big.size(); ++i) {
            big[i] = static_cast<std::uint8_t>(i * 7 + 3);
        }
        WireFeeder server_feeds;
        const std::vector<std::uint8_t> wire = connection_wire(client, big);
        EXPECT_LE(3u, count_records(wire)); // 40000 > 2 × 16384
        ASSERT_TRUE(server_feeds.feed(server, wire));
        EXPECT_EQ(big, read_all(server));
        EXPECT_FALSE(server.failed());
    });
}

TEST(TlsConnectionTest, CloseNotifyBothDirections13) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        ServerMaterial material;
        PairStates states = complete_pair(material);

        TlsConnection client(TlsConnectionRole::Client, std::move(states.client));
        TlsConnection server(TlsConnectionRole::Server, std::move(states.server));

        // Plaintext delivered in the SAME feed as the close_notify still reads
        // first; only then does PeerClosed surface.
        const std::vector<std::uint8_t> bye{'b', 'y', 'e'};
        ASSERT_TRUE(client.write(bye).has_value());
        ASSERT_TRUE(client.close_notify().has_value());
        WireFeeder server_feeds;
        WireFeeder client_feeds;
        const std::vector<std::uint8_t> wire = chain_bytes(client.take_output());
        EXPECT_EQ(2u, count_records(wire)); // app data + close_notify
        ASSERT_TRUE(server_feeds.feed(server, wire));
        EXPECT_FALSE(server.failed());
        EXPECT_EQ(bye, read_all(server));
        std::size_t n = 0;
        std::array<std::uint8_t, 8> scratch{};
        EXPECT_EQ(ReadStatus::PeerClosed, server.read(scratch.data(), scratch.size(), n));
        EXPECT_EQ(0u, n);
        EXPECT_TRUE(server.peer_closed());
        // Writing after the PEER closed stays legal (half-close semantics are
        // the glue's call); our own close_notify is idempotent success.
        EXPECT_TRUE(server.close_notify().has_value());

        // The answering close_notify crosses back.
        ASSERT_TRUE(client_feeds.feed(client, chain_bytes(server.take_output())));
        EXPECT_TRUE(client.peer_closed());
        EXPECT_EQ(ReadStatus::PeerClosed, client.read(scratch.data(), scratch.size(), n));
    });
}

TEST(TlsConnectionTest, EmptyWriteEmitsOneZeroLengthRecord13) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        ServerMaterial material;
        PairStates states = complete_pair(material);

        TlsConnection client(TlsConnectionRole::Client, std::move(states.client));
        TlsConnection server(TlsConnectionRole::Server, std::move(states.server));

        WireFeeder server_feeds;
        const std::vector<std::uint8_t> wire = connection_wire(client, std::span<const std::uint8_t>{});
        EXPECT_EQ(1u, count_records(wire));
        const std::size_t len = (static_cast<std::size_t>(wire[3]) << 8) | wire[4];
        EXPECT_EQ(17u, len); // tag(16) + inner type(1)
        ASSERT_TRUE(server_feeds.feed(server, wire));
        EXPECT_TRUE(read_all(server).empty()); // nothing delivered, nothing failed
        EXPECT_FALSE(server.failed());
        std::size_t n = 0;
        std::array<std::uint8_t, 8> scratch{};
        EXPECT_EQ(ReadStatus::NeedMore, server.read(scratch.data(), scratch.size(), n));
    });
}

TEST(TlsConnectionTest, KeyUpdateRequestedRoundTrip13) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        ServerMaterial material;
        PairStates states = complete_pair(material);

        // The client sends KeyUpdate(update_requested) and rotates its write
        // side; the server connection must rekey its READ side on receipt.
        const std::vector<std::uint8_t> ku = craft_key_update(states.client, true);
        TlsConnection client(TlsConnectionRole::Client, std::move(states.client));
        TlsConnection server(TlsConnectionRole::Server, std::move(states.server));
        WireFeeder server_feeds;
        WireFeeder client_feeds;
        ASSERT_TRUE(server_feeds.feed(server, ku));
        EXPECT_FALSE(server.failed());
        EXPECT_TRUE(read_all(server).empty()); // a KeyUpdate carries no app data

        // The passive response: the write emits [KeyUpdate under the OLD server
        // write keys][app data under the ROTATED ones].
        const std::vector<std::uint8_t> resp{'r', 'e', 's', 'p'};
        ASSERT_TRUE(server.write(resp).has_value());
        const std::vector<std::uint8_t> flight = chain_bytes(server.take_output());
        EXPECT_EQ(2u, count_records(flight));
        ASSERT_TRUE(client_feeds.feed(client, flight));
        EXPECT_FALSE(client.failed());
        EXPECT_EQ(resp, read_all(client)); // proves the client's read side rotated in step

        // The client's post-rotation write opens with the server's rotated READ
        // keys.
        const std::vector<std::uint8_t> tail{'f', 'i', 'n', 'a', 'l'};
        ASSERT_TRUE(client.write(tail).has_value());
        ASSERT_TRUE(server_feeds.feed(server, chain_bytes(client.take_output())));
        EXPECT_EQ(tail, read_all(server));
        EXPECT_FALSE(server.failed());
    });
}

TEST(TlsConnectionTest, KeyUpdateNotRequestedKeepsPeerWriteKeys13) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        ServerMaterial material;
        PairStates states = complete_pair(material);

        const std::vector<std::uint8_t> ku = craft_key_update(states.client, false);
        TlsConnection client(TlsConnectionRole::Client, std::move(states.client));
        TlsConnection server(TlsConnectionRole::Server, std::move(states.server));
        WireFeeder server_feeds;
        WireFeeder client_feeds;
        ASSERT_TRUE(server_feeds.feed(server, ku));
        EXPECT_FALSE(server.failed());

        // No response owed: exactly ONE record on the wire, sealed under the
        // server's ORIGINAL write keys (the client's read side never rotated).
        const std::vector<std::uint8_t> msg{'o', 'k'};
        ASSERT_TRUE(server.write(msg).has_value());
        const std::vector<std::uint8_t> flight = chain_bytes(server.take_output());
        EXPECT_EQ(1u, count_records(flight));
        ASSERT_TRUE(client_feeds.feed(client, flight));
        EXPECT_EQ(msg, read_all(client));
    });
}

TEST(TlsConnectionTest, NewSessionTicketIsSwallowed13) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        ServerMaterial material;
        PairStates states = complete_pair(material);

        // An NST (type 4) the peer keeps sending after the handshake: body is
        // opaque here — the connection swallows without parsing (09 §1: no
        // client session cache).
        std::vector<std::uint8_t> message{4, 0, 0, 40};
        message.resize(4 + 40, 0xAB);
        const std::vector<std::uint8_t> nst = craft_hs_record(states.client, message);

        TlsConnection client(TlsConnectionRole::Client, std::move(states.client));
        TlsConnection server(TlsConnectionRole::Server, std::move(states.server));
        WireFeeder server_feeds;
        ASSERT_TRUE(server_feeds.feed(server, nst));
        EXPECT_FALSE(server.failed());
        EXPECT_TRUE(read_all(server).empty());

        // The stream stays alive after the swallow.
        const std::vector<std::uint8_t> ping{'p', 'i', 'n', 'g'};
        ASSERT_TRUE(server_feeds.feed(server, connection_wire(client, ping)));
        EXPECT_EQ(ping, read_all(server));
    });
}

TEST(TlsConnectionTest, PeerFatalAlertLatches13) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        ServerMaterial material;
        PairStates states = complete_pair(material);

        // A fatal alert from the peer (sealed, inner type alert): terminal, no
        // response owed.
        const std::uint8_t alert[2] = {2, static_cast<std::uint8_t>(TlsAlertDesc::DecodeError)};
        TlsRecordCipher wc = std::move(states.client.write_cipher);
        const std::vector<std::uint8_t> wire = seal_record(wc, TlsContentType::Alert, alert, kTypeApplicationData);
        states.client.write_cipher = std::move(wc);

        TlsConnection client(TlsConnectionRole::Client, std::move(states.client));
        TlsConnection server(TlsConnectionRole::Server, std::move(states.server));
        WireFeeder server_feeds;
        ASSERT_TRUE(server_feeds.feed(server, wire));
        EXPECT_TRUE(server.failed());
        EXPECT_TRUE(chain_bytes(server.take_output()).empty()); // nothing sent back

        std::size_t n = 0;
        std::array<std::uint8_t, 8> scratch{};
        EXPECT_EQ(ReadStatus::Fatal, server.read(scratch.data(), scratch.size(), n));
        const std::uint8_t dead_byte = 0;
        EXPECT_FALSE(server.write({&dead_byte, 1}).has_value());
        // The client side is untouched — half-close semantics live in the glue.
        EXPECT_FALSE(client.failed());
    });
}

TEST(TlsConnectionTest, CorruptedRecordLatchesBadRecordMac13) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        ServerMaterial material;
        PairStates states = complete_pair(material);

        const std::vector<std::uint8_t> ping{'p', 'i', 'n', 'g'};
        TlsRecordCipher wc = std::move(states.client.write_cipher);
        std::vector<std::uint8_t> wire = seal_record(wc, TlsContentType::ApplicationData, ping, kTypeApplicationData);
        states.client.write_cipher = std::move(wc);
        wire[wire.size() / 2] ^= 0x40; // flip one ciphertext byte

        TlsConnection client(TlsConnectionRole::Client, std::move(states.client));
        TlsConnection server(TlsConnectionRole::Server, std::move(states.server));
        WireFeeder server_feeds;
        ASSERT_TRUE(server_feeds.feed(server, wire));
        EXPECT_TRUE(server.failed());
        // Our own fatal alert IS encoded — the glue flushes it best-effort.
        EXPECT_FALSE(chain_bytes(server.take_output()).empty());
        // The connection is terminal: further records/writes refuse work.
        const std::vector<std::uint8_t> more = connection_wire(client, ping);
        ASSERT_TRUE(server_feeds.feed(server, more));
        EXPECT_TRUE(read_all(server).empty());
        EXPECT_FALSE(server.write(ping).has_value());
    });
}

TEST(TlsConnectionTest, CcsAfterHandshakeIsFatal13) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        ServerMaterial material;
        PairStates states = complete_pair(material);

        TlsConnection client(TlsConnectionRole::Client, std::move(states.client));
        TlsConnection server(TlsConnectionRole::Server, std::move(states.server));
        WireFeeder server_feeds;
        const std::uint8_t ccs[] = {kTypeChangeCipherSpec, 0x03, 0x03, 0x00, 0x01, 0x01};
        ASSERT_TRUE(server_feeds.feed(server, ccs));
        EXPECT_TRUE(server.failed());
    });
}

TEST(TlsConnectionTest, TwoPostHandshakeMessagesInOneRecordDrain) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        ServerMaterial material;
        PairStates states = complete_pair(material);

        // Two complete post-handshake messages coalesced into ONE sealed
        // record: the KeyUpdate rekeys the read side, the NST swallows, and
        // the fragment drains fully (nothing strands in the reassembler
        // waiting for bytes that never come).
        std::vector<std::uint8_t> messages{static_cast<std::uint8_t>(TlsHandshakeType::KeyUpdate), 0, 0, 1, 0};
        const std::uint8_t nst[]{4, 0, 0, 4, 0xAA, 0xBB, 0xCC, 0xDD};
        messages.insert(messages.end(), nst, nst + sizeof(nst));

        // Seal the coalesced payload, then rotate the client's write side one
        // generation — the sender's obligation after a KeyUpdate (mirrors
        // craft_key_update's rotation for a multi-message payload).
        TlsRecordCipher wc = std::move(states.client.write_cipher);
        std::vector<std::uint8_t> wire = seal_record(wc, TlsContentType::Handshake, messages, kTypeApplicationData);
        states.client.write_cipher = std::move(wc);
        auto next = fiber::tls::tls13_key_update(states.client.client_app_secret);
        ASSERT_TRUE(next.has_value());
        auto keys = fiber::tls::tls13_traffic_keys(*next, states.client.suite);
        ASSERT_TRUE(keys.has_value());
        TlsRecordCipher fresh;
        ASSERT_TRUE(fresh.init(states.client.suite, TlsRecordProtectionKind::Tls13, {keys->key.data(), keys->key_len},
                               {keys->iv.data(), keys->iv_len})
                            .has_value());
        states.client.client_app_secret = std::move(*next);
        states.client.write_cipher = std::move(fresh);

        TlsConnection client(TlsConnectionRole::Client, std::move(states.client));
        TlsConnection server(TlsConnectionRole::Server, std::move(states.server));
        WireFeeder server_feeds;
        ASSERT_TRUE(server_feeds.feed(server, wire));
        EXPECT_FALSE(server.failed());

        // The stream stays alive behind the rekeyed read side: the client
        // (write side rotated past the KeyUpdate by craft) → server opens.
        const std::vector<std::uint8_t> ping{'p', 'i', 'n', 'g'};
        ASSERT_TRUE(server_feeds.feed(server, connection_wire(client, ping)));
        EXPECT_EQ(ping, read_all(server));
    });
}

TEST(TlsConnectionTest, EngineLeftoverFeedsConnection) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        ServerMaterial material;
        auto trust = TlsTrustStore::from_pem_bundle({certfix::kRootRsaPem, std::strlen(certfix::kRootRsaPem)});
        ASSERT_TRUE(trust.has_value());

        TlsClientConfig client_cfg;
        client_cfg.sni_host = "example.com";
        client_cfg.alpn = material.alpn;
        client_cfg.trust = &*trust;
        client_cfg.now_unix_ms = certfix::kRefNowMs;

        TlsServerHandshakeEngine server(material.config(), nullptr, nullptr);
        TlsClientHandshakeEngine client(client_cfg, nullptr);

        // Trailing partial-record header riding the client's final flight: the
        // server engine consumes the Finished, reaches done, and these bytes
        // stay unconsumed.
        const std::vector<std::uint8_t> tail{kTypeApplicationData, 0x03, 0x03};
        ASSERT_TRUE(drive_pair(client, server, &tail));

        IoBufChain leftover = server.take_inbound_leftover();
        ASSERT_EQ(3u, leftover.readable_bytes());
        const std::vector<std::uint8_t> leftover_bytes = chain_bytes(leftover);
        EXPECT_EQ(tail, leftover_bytes);
        EXPECT_EQ(0u, server.take_inbound_leftover().readable_bytes()); // second take is empty

        TlsConnectedState client_state = client.take_state();
        TlsConnectedState server_state = server.take_state();

        // The rest of that record (header tail + sealed body) arrives through
        // the CONNECTION — the leftover bytes and the follow-up feed reassemble
        // into one record.
        TlsRecordCipher wc = std::move(client_state.write_cipher);
        const std::vector<std::uint8_t> ping{'p', 'i', 'n', 'g'};
        const std::vector<std::uint8_t> record =
                seal_record(wc, TlsContentType::ApplicationData, ping, kTypeApplicationData);
        client_state.write_cipher = std::move(wc);

        TlsConnection server_conn(TlsConnectionRole::Server, std::move(server_state));
        WireFeeder server_feeds;
        // The leftover partial header and the follow-up feed reassemble into
        // one record inside the glue's reader — the connection only ever
        // sees complete records.
        ASSERT_TRUE(server_feeds.feed(server_conn, leftover_bytes));
        ASSERT_TRUE(server_feeds.feed(server_conn, {record.data() + 3, record.size() - 3}));
        EXPECT_EQ(ping, read_all(server_conn));
        EXPECT_FALSE(server_conn.failed());
    });
}

// =====================================================================
// TLS 1.2 (synthetic key-block pair)
// =====================================================================

TEST(TlsConnectionTest, HandshakeRecordAfter12HandshakeIsFatal) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        ServerMaterial material; // pool only
        Synthetic12Pair pair = make_synthetic_12_pair();

        // HelloRequest (type 0, empty body) — renegotiation is refused outright
        // (09 §1 decision 3). Sealed with the client's write keys, outer type
        // preserved (1.2 binds it into the AAD).
        const std::uint8_t hello_request[4] = {0, 0, 0, 0};
        const std::vector<std::uint8_t> wire =
                craft_12_record(pair.client.write_cipher, TlsContentType::Handshake, hello_request);

        TlsConnection server_conn(TlsConnectionRole::Server, std::move(pair.server));
        WireFeeder server_feeds;
        ASSERT_TRUE(server_feeds.feed(server_conn, wire));
        EXPECT_TRUE(server_conn.failed());
        EXPECT_FALSE(chain_bytes(server_conn.take_output()).empty()); // our fatal alert flies
    });
}

TEST(TlsConnectionTest, DegenerateShortSealedRecordLatchesBadRecordMac12) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        ServerMaterial material; // pool only
        Synthetic12Pair pair = make_synthetic_12_pair();

        // A sealed body shorter than nonce+tag can never authenticate: the
        // length guard rejects it BEFORE any buffer arithmetic (the chain
        // adapter's body/tag sizing must underflow nothing on degenerate
        // lengths) and it collapses to bad_record_mac like any other
        // unauthentic record.
        const std::uint8_t short_record[] = {kTypeHandshake, 0x03, 0x03, 0x00, 0x01, 0x00};

        TlsConnection server_conn(TlsConnectionRole::Server, std::move(pair.server));
        WireFeeder server_feeds;
        ASSERT_TRUE(server_feeds.feed(server_conn, short_record));
        EXPECT_TRUE(server_conn.failed());
        EXPECT_FALSE(chain_bytes(server_conn.take_output()).empty()); // our fatal alert flies
    });
}

TEST(TlsConnectionTest, Synthetic12AppAndCloseNotifyBothWays) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        ServerMaterial material; // pool only
        Synthetic12Pair pair = make_synthetic_12_pair();

        TlsConnection client(TlsConnectionRole::Client, std::move(pair.client));
        TlsConnection server(TlsConnectionRole::Server, std::move(pair.server));

        WireFeeder server_feeds;
        WireFeeder client_feeds;
        const std::vector<std::uint8_t> ping{'1', '2', 'p', 'i', 'n', 'g'};
        ASSERT_TRUE(server_feeds.feed(server, connection_wire(client, ping)));
        EXPECT_EQ(ping, read_all(server));

        const std::vector<std::uint8_t> pong{'1', '2', 'p', 'o', 'n', 'g'};
        ASSERT_TRUE(client_feeds.feed(client, connection_wire(server, pong)));
        EXPECT_EQ(pong, read_all(client));

        // close_notify in 1.2 flies as a SEALED alert record (outer type 21
        // preserved) — this exercises the sealed-1.2-alert decode on receive.
        ASSERT_TRUE(client.close_notify().has_value());
        const std::vector<std::uint8_t> close_wire = chain_bytes(client.take_output());
        ASSERT_EQ(1u, count_records(close_wire));
        EXPECT_EQ(kTypeAlert, close_wire[0]); // 1.2 preserves the record type
        ASSERT_TRUE(server_feeds.feed(server, close_wire));
        EXPECT_TRUE(server.peer_closed());
        EXPECT_FALSE(server.failed());

        ASSERT_TRUE(server.close_notify().has_value());
        ASSERT_TRUE(client_feeds.feed(client, chain_bytes(server.take_output())));
        EXPECT_TRUE(client.peer_closed());
    });
}

// =====================================================================
// BoringSSL interop (memory BIOs)
// =====================================================================

TEST(TlsConnectionTest, BoringSsl13AppCloseNotifyAndKeyUpdate) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        auto client = BoringClient::make();
        ASSERT_NE(nullptr, client);
        ServerMaterial material;
        TlsServerHandshakeEngine engine(material.config(), nullptr, nullptr);
        ASSERT_TRUE(drive(*client, engine));
        TlsConnectedState state = engine.take_state();
        ASSERT_EQ(TlsProtocolVersion::Tls13, state.version);
        TlsConnection conn(TlsConnectionRole::Server, std::move(state));
        ASSERT_FALSE(conn.failed());
        WireFeeder conn_feeds;

        // BoringSSL → connection
        const char kMessage[] = "hello from boringssl";
        ASSERT_EQ(sizeof(kMessage) - 1,
                  static_cast<std::size_t>(SSL_write(client->ssl(), kMessage, static_cast<int>(sizeof(kMessage) - 1))));
        ASSERT_TRUE(conn_feeds.feed(conn, client->drain_wbio()));
        const std::vector<std::uint8_t> got = read_all(conn);
        ASSERT_EQ(sizeof(kMessage) - 1, got.size());
        EXPECT_EQ(0, std::memcmp(kMessage, got.data(), got.size()));

        // connection → BoringSSL
        const char kReply[] = "reply from fiber";
        ASSERT_TRUE(conn.write({reinterpret_cast<const std::uint8_t *>(kReply), sizeof(kReply) - 1}).has_value());
        ASSERT_TRUE(client->ship(chain_bytes(conn.take_output())));
        std::array<char, 128> buf{};
        const int read = SSL_read(client->ssl(), buf.data(), static_cast<int>(buf.size()));
        ASSERT_GT(read, 0);
        EXPECT_EQ(0, std::memcmp(kReply, buf.data(), sizeof(kReply) - 1));

        // KeyUpdate(update_requested) from the real peer: our read side rekeys,
        // and the passive response (under the OLD write keys, rotation after)
        // must satisfy BoringSSL's expectations — its next SSL_read consumes our
        // response record and the app data behind it.
        ASSERT_EQ(1, SSL_key_update(client->ssl(), SSL_KEY_UPDATE_REQUESTED));
        (void) client->handshake_step(); // flush the queued KeyUpdate
        ASSERT_TRUE(conn_feeds.feed(conn, client->drain_wbio()));
        EXPECT_FALSE(conn.failed());
        const char kAfterKu[] = "after key update";
        ASSERT_TRUE(conn.write({reinterpret_cast<const std::uint8_t *>(kAfterKu), sizeof(kAfterKu) - 1}).has_value());
        ASSERT_TRUE(client->ship(chain_bytes(conn.take_output())));
        const int read2 = SSL_read(client->ssl(), buf.data(), static_cast<int>(buf.size()));
        ASSERT_GT(read2, 0);
        EXPECT_EQ(0, std::memcmp(kAfterKu, buf.data(), sizeof(kAfterKu) - 1));

        // BoringSSL's close_notify (sealed inner alert): peer_closed latches.
        ASSERT_EQ(0, SSL_shutdown(client->ssl()));
        ASSERT_TRUE(conn_feeds.feed(conn, client->drain_wbio()));
        EXPECT_TRUE(conn.peer_closed());
        EXPECT_FALSE(conn.failed());
    });
}

TEST(TlsConnectionTest, BoringSsl12AppAndSealedCloseNotify) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        auto client = BoringClient::make(BoringClientOptions{.tls12_only = true});
        ASSERT_NE(nullptr, client);
        ServerMaterial material;
        TlsServerHandshakeEngine engine(material.config(), nullptr, nullptr);
        ASSERT_TRUE(drive(*client, engine));
        TlsConnectedState state = engine.take_state();
        ASSERT_EQ(TlsProtocolVersion::Tls12, state.version);
        TlsConnection conn(TlsConnectionRole::Server, std::move(state));
        ASSERT_FALSE(conn.failed());
        WireFeeder conn_feeds;

        // App data both ways under the 1.2 key block.
        const char kMessage[] = "12 hello";
        ASSERT_EQ(sizeof(kMessage) - 1,
                  static_cast<std::size_t>(SSL_write(client->ssl(), kMessage, static_cast<int>(sizeof(kMessage) - 1))));
        ASSERT_TRUE(conn_feeds.feed(conn, client->drain_wbio()));
        const std::vector<std::uint8_t> got = read_all(conn);
        ASSERT_EQ(sizeof(kMessage) - 1, got.size());
        EXPECT_EQ(0, std::memcmp(kMessage, got.data(), got.size()));

        const char kReply[] = "12 reply";
        ASSERT_TRUE(conn.write({reinterpret_cast<const std::uint8_t *>(kReply), sizeof(kReply) - 1}).has_value());
        ASSERT_TRUE(client->ship(chain_bytes(conn.take_output())));
        std::array<char, 64> buf{};
        ASSERT_GT(SSL_read(client->ssl(), buf.data(), static_cast<int>(buf.size())), 0);
        EXPECT_EQ(0, std::memcmp(kReply, buf.data(), sizeof(kReply) - 1));

        // BoringSSL's close_notify arrives as a SEALED 1.2 alert record.
        ASSERT_EQ(0, SSL_shutdown(client->ssl()));
        ASSERT_TRUE(conn_feeds.feed(conn, client->drain_wbio()));
        EXPECT_TRUE(conn.peer_closed());
        EXPECT_FALSE(conn.failed());

        // Our close_notify back: BoringSSL reads it as clean EOF.
        ASSERT_TRUE(conn.close_notify().has_value());
        ASSERT_TRUE(client->ship(chain_bytes(conn.take_output())));
        EXPECT_EQ(0, SSL_read(client->ssl(), buf.data(), static_cast<int>(buf.size())));
        EXPECT_EQ(SSL_ERROR_ZERO_RETURN, SSL_get_error(client->ssl(), 0));
    });
}
