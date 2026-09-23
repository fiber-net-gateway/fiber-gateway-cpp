// TLS 1.3 client handshake engine against a real BoringSSL server peer
// (feature/tls/06 §8.1, §8.3, §8.4, §8.5). The peer is an SSL object over
// memory BIOs; the drive loop ships engine output into the server's read BIO
// and feeds the server's write BIO back, in whole-flight or 1-byte slices.
// Post-handshake app-data round trips exercise the moved TlsConnectedState
// ciphers against the same BoringSSL session.

#include <gtest/gtest.h>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "TlsCertFixtures.h"

#include <fiber/common/IoError.h>
#include <fiber/common/mem/IoBuf.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/tls/TlsConfig.h>
#include <fiber/tls/TlsConnectedState.h>
#include <fiber/tls/crypto/Tls13KeySchedule.h>
#include <fiber/tls/handshake/TlsCipherSuites.h>
#include <fiber/tls/handshake/TlsClientHandshakeEngine.h>
#include <fiber/tls/handshake/TlsHandshakeMessage.h>
#include <fiber/tls/record/TlsRecord.h>

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
using fiber::tls::TlsSessionOffer;
using fiber::tls::TlsTrustStore;
using Event = TlsClientHandshakeEngine::Event;

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

// TLS_KEYLOG=<path> enables NSS-format secret dumping for debugging.
void keylog_line(const SSL *, const char *line) {
    if (const char *path = ::getenv("TLS_KEYLOG"); path != nullptr && path[0] != '\0') {
        if (FILE *f = std::fopen(path, "a"); f != nullptr) {
            std::fprintf(f, "%s\n", line);
            std::fclose(f);
        }
    }
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

struct ServerOptions {
    const char *leaf_pem = certfix::kLeafRsaPem;
    const char *intermediate_pem = certfix::kIntermediateRsaPem;
    const char *key_pem = certfix::kRsa2048KeyPem;
    const char *client_trust_pem = nullptr; // non-null: verify client chains
    bool require_client_cert = false;
    const char *groups = nullptr; // non-null: restrict groups (HRR trigger)
    // ctx-level 0-RTT: tickets are ISSUED 0-RTT-capable (ticket_max_early_data
    // is stamped at issuance only when this is set — tls13_server.cc).
    bool early_data = false;
    // non-null: pin the server to TLS 1.2 with exactly this OpenSSL cipher
    // (unlike 1.3, the 1.2 suite list IS server-configurable — that is how
    // each offered 1.2 suite gets pinned in §8.2).
    const char *tls12_cipher = nullptr;
};

class BoringServer {
public:
    static std::unique_ptr<BoringServer> make(const ServerOptions &opt) {
        SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
        if (ctx == nullptr) {
            return nullptr;
        }
        std::unique_ptr<BoringServer> server(new BoringServer(ctx, true));
        if (opt.tls12_cipher != nullptr) {
            SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
            SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION);
            if (SSL_CTX_set_cipher_list(ctx, opt.tls12_cipher) != 1) {
                return nullptr;
            }
            // NOTE: a 1.2 server only issues RFC 5077 tickets when the CLIENT
            // offered the session_ticket extension — this engine does not (08
            // owns resumption), so no NST flows here and the FSM's
            // tolerate-and-hash-NST path stays covered by its bounds checks.
        } else {
            SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
            SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
        }
        SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
        if (opt.early_data) {
            SSL_CTX_set_early_data_enabled(ctx, 1);
        }

        X509 *leaf = load_cert(opt.leaf_pem);
        if (leaf == nullptr || SSL_CTX_use_certificate(ctx, leaf) != 1) {
            return nullptr;
        }
        if (opt.intermediate_pem != nullptr) {
            X509 *intermediate = load_cert(opt.intermediate_pem);
            if (intermediate == nullptr || SSL_CTX_add0_chain_cert(ctx, intermediate) != 1) {
                return nullptr;
            }
        }
        EVP_PKEY *key = load_key(opt.key_pem);
        if (key == nullptr || SSL_CTX_use_PrivateKey(ctx, key) != 1 || SSL_CTX_check_private_key(ctx) != 1) {
            EVP_PKEY_free(key);
            return nullptr;
        }

        if (opt.groups != nullptr && SSL_CTX_set1_groups_list(ctx, opt.groups) != 1) {
            return nullptr;
        }
        if (opt.client_trust_pem != nullptr) {
            X509 *root = load_cert(opt.client_trust_pem);
            if (root == nullptr || X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx), root) != 1) {
                return nullptr;
            }
            SSL_CTX_set_verify(ctx,
                               opt.require_client_cert
                                       ? static_cast<int>(SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT)
                                       : static_cast<int>(SSL_VERIFY_PEER),
                               nullptr);
        }
        SSL_CTX_set_alpn_select_cb(ctx, select_first_alpn, nullptr);
        if (::getenv("TLS_KEYLOG") != nullptr) {
            SSL_CTX_set_keylog_callback(ctx, keylog_line);
        }

        server->ssl_ = SSL_new(ctx);
        if (server->ssl_ == nullptr) {
            return nullptr;
        }
        SSL_set_accept_state(server->ssl_);
        server->rbio_ = BIO_new(BIO_s_mem());
        server->wbio_ = BIO_new(BIO_s_mem());
        SSL_set_bio(server->ssl_, server->rbio_, server->wbio_);
        return server;
    }

    // A second server sharing this ctx: BoringSSL mints per-ctx ticket keys,
    // so a PSK hop-2 server must adopt hop 1's ctx or the ticket won't
    // decrypt. The ctx outlives this object (the first server owns it).
    static std::unique_ptr<BoringServer> make_on_ctx(SSL_CTX *ctx) {
        if (ctx == nullptr) {
            return nullptr;
        }
        std::unique_ptr<BoringServer> server(new BoringServer(ctx, false));
        server->ssl_ = SSL_new(ctx);
        if (server->ssl_ == nullptr) {
            return nullptr;
        }
        SSL_set_accept_state(server->ssl_);
        server->rbio_ = BIO_new(BIO_s_mem());
        server->wbio_ = BIO_new(BIO_s_mem());
        SSL_set_bio(server->ssl_, server->rbio_, server->wbio_);
        return server;
    }

    ~BoringServer() {
        if (ssl_ != nullptr) {
            SSL_free(ssl_); // frees the BIOs
        }
        if (owns_ctx_ && ctx_ != nullptr) {
            SSL_CTX_free(ctx_);
        }
    }

    [[nodiscard]] SSL_CTX *ctx() const noexcept { return ctx_; }

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

    // One non-blocking handshake step: 1 = complete, 0 = progress expected
    // (WANT_READ/WRITE), -1 = the server itself failed.
    int handshake_step() {
        const int rc = SSL_do_handshake(ssl_);
        if (rc == 1) {
            return 1;
        }
        const int err = SSL_get_error(ssl_, rc);
        return (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) ? 0 : -1;
    }

private:
    explicit BoringServer(SSL_CTX *ctx, bool owns) noexcept : ctx_(ctx), owns_ctx_(owns) {}

    SSL_CTX *ctx_ = nullptr;
    bool owns_ctx_ = true;
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

// Feeds `bytes` (whole or one byte at a time); false = engine-level failure
// (NoMem / empty allocation) already reported via EXPECT.
bool feed_bytes(TlsClientHandshakeEngine &engine, std::span<const std::uint8_t> bytes, bool sliced, Event &last) {
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
// both wire directions (HRR sentinel search, alert parsing).
struct DriveLog {
    std::vector<std::uint8_t> client_to_server;
    std::vector<std::uint8_t> server_to_client;
    // Records after the terminal message of the final flight — fed to nobody.
    // A 0-RTT-accepting BoringSSL server seals its NewSessionTicket HALF-RTT
    // (application keys, write seq 0) and flushes it with the ServerHello
    // flight (tls13_server.cc do_send_half_rtt_ticket); the engine finishes at
    // the server Finished, so the NST record lands here.
    std::vector<std::uint8_t> server_tail;
};

bool drive(BoringServer &server, TlsClientHandshakeEngine &engine, bool sliced, DriveLog &log) {
    for (int spin = 0; spin < 256; ++spin) {
        const std::vector<std::uint8_t> out = chain_bytes(engine.take_output());
        if (!out.empty()) {
            log.client_to_server.insert(log.client_to_server.end(), out.begin(), out.end());
            if (!server.ship(out)) {
                EXPECT_TRUE(false);
                return false;
            }
        }
        if (engine.done()) {
            return !engine.failed();
        }
        if (!static_cast<bool>(SSL_is_init_finished(server.ssl()))) {
            if (server.handshake_step() < 0) {
                ADD_FAILURE() << "BoringSSL server handshake failed mid-drive";
                ERR_print_errors_fp(stderr);
                return false;
            }
        }
        const std::vector<std::uint8_t> flight = server.drain_wbio();
        if (flight.empty()) {
            break; // no progress available; the engine decides success below
        }
        log.server_to_client.insert(log.server_to_client.end(), flight.begin(), flight.end());
        // Record-granular feed: a flight can carry records past the engine's
        // terminal message (a 0-RTT-accepting BoringSSL server seals its
        // NewSessionTicket HALF-RTT — application keys, write seq 0 — and
        // flushes it behind the server Finished; see do_send_half_rtt_ticket).
        // Those records stay unfed and land in the tail log.
        std::size_t roff = 0;
        while (roff < flight.size() && !engine.done()) {
            std::size_t chunk = flight.size() - roff;
            if (roff + fiber::tls::kTlsRecordHeaderSize < flight.size()) {
                const std::size_t rlen = (static_cast<std::size_t>(flight[roff + 3]) << 8) | flight[roff + 4];
                if (roff + fiber::tls::kTlsRecordHeaderSize + rlen <= flight.size()) {
                    chunk = fiber::tls::kTlsRecordHeaderSize + rlen; // one complete record
                }
            }
            Event event = Event::None;
            if (!feed_bytes(engine, {flight.data() + roff, chunk}, sliced, event)) {
                return false;
            }
            roff += chunk;
        }
        if (roff < flight.size()) {
            log.server_tail.assign(flight.begin() + static_cast<std::ptrdiff_t>(roff), flight.end());
        }
    }
    return engine.done() && !engine.failed();
}

// Last plaintext alert record's description byte in a client→server stream.
std::optional<int> last_alert_desc(const std::vector<std::uint8_t> &wire) {
    std::optional<int> desc;
    std::size_t off = 0;
    while (off + fiber::tls::kTlsRecordHeaderSize <= wire.size()) {
        const std::size_t len = (static_cast<std::size_t>(wire[off + 3]) << 8) | wire[off + 4];
        if (off + fiber::tls::kTlsRecordHeaderSize + len > wire.size()) {
            break;
        }
        if (wire[off] == 21 && len == 2) {
            desc = wire[off + fiber::tls::kTlsRecordHeaderSize + 1];
        }
        off += fiber::tls::kTlsRecordHeaderSize + len;
    }
    return desc;
}

// Opens each record in `wire` through the state's read cipher, discarding
// the plaintext. BoringSSL's deferred NewSessionTicket flight must be
// CONSUMED this way before any later app record is opened: every opened
// record advances the cipher's sequence, so skipping records would desync
// the AEAD nonce. Non-app-data records (e.g. a stray CCS) pass untouched.
void open_and_discard_records(TlsConnectedState &state, const std::vector<std::uint8_t> &wire) {
    namespace ft = fiber::tls;
    std::array<std::uint8_t, ft::kTlsMaxPlaintextSize> dst{};
    std::size_t off = 0;
    while (off + ft::kTlsRecordHeaderSize <= wire.size()) {
        const std::size_t len = (static_cast<std::size_t>(wire[off + 3]) << 8) | wire[off + 4];
        if (off + ft::kTlsRecordHeaderSize + len > wire.size()) {
            break;
        }
        if (wire[off] == static_cast<std::uint8_t>(ft::TlsContentType::ApplicationData) && len >= 17) {
            (void) state.read_cipher.open(ft::TlsContentType::ApplicationData,
                                          (static_cast<std::uint16_t>(wire[off + 1]) << 8) | wire[off + 2],
                                          static_cast<std::uint16_t>(len),
                                          {wire.data() + off + ft::kTlsRecordHeaderSize, len}, dst);
        }
        off += ft::kTlsRecordHeaderSize + len;
    }
}

// ---- shared client material ----

struct ClientMaterial {
    IoBufNodePool pool;
    TlsTrustStore trust; // kRootRsaPem anchors by default
    std::vector<std::string_view> alpn{"h2", "http/1.1"};
    std::string client_chain_pem;
    std::optional<TlsCertificateChain> client_chain;
    std::optional<TlsPrivateKey> client_key;

    ClientMaterial() {
        auto store = TlsTrustStore::from_pem_bundle({certfix::kRootRsaPem, std::strlen(certfix::kRootRsaPem)});
        EXPECT_TRUE(store.has_value());
        trust = std::move(store).value();
    }

    [[nodiscard]] TlsClientConfig config(std::string_view sni, std::int64_t now_ms) const {
        TlsClientConfig cfg;
        cfg.sni_host = sni;
        cfg.alpn = alpn;
        cfg.trust = &trust;
        cfg.now_unix_ms = now_ms;
        if (client_chain.has_value() && client_key.has_value()) {
            cfg.client_chain = &*client_chain;
            cfg.client_key = &*client_key;
        }
        return cfg;
    }

    void load_client_credential() {
        client_chain_pem.assign(certfix::kClientRsaPem);
        client_chain_pem.append(certfix::kIntermediateRsaPem);
        auto chain = TlsCertificateChain::parse_pem_bundle({client_chain_pem.data(), client_chain_pem.size()});
        ASSERT_TRUE(chain.has_value());
        client_chain = std::move(chain).value();
        auto key = TlsPrivateKey::parse_pem({certfix::kRsa2048KeyPem, std::strlen(certfix::kRsa2048KeyPem)});
        ASSERT_TRUE(key.has_value());
        client_key = std::move(key).value();
    }
};

} // namespace

// =====================================================================
// §8.1 — full handshake, ALPN, app-data round trip
//
// This BoringSSL exposes no TLS 1.3 ciphersuite selection ("TLS 1.3 ciphers
// do not participate in this mechanism", ssl.h) — the negotiated suite is the
// server's built-in preference ∩ the client offer, so the test asserts the
// engine's suite matches what the server actually picked rather than forcing
// each suite. The per-suite key/record paths are covered by the 02/05 layers.
// =====================================================================

TEST(TlsClientHandshake13Full, HandshakeAndAppRoundTrip) {
    auto server = BoringServer::make(ServerOptions{});
    ASSERT_NE(nullptr, server);
    ClientMaterial material;
    const TlsClientConfig cfg = material.config("example.com", certfix::kRefNowMs);

    TlsClientHandshakeEngine engine(cfg, nullptr, material.pool);
    ASSERT_FALSE(engine.done());

    DriveLog log;
    ASSERT_TRUE(drive(*server, engine, false, log));

    TlsConnectedState state = engine.take_state();
    EXPECT_EQ(fiber::tls::TlsProtocolVersion::Tls13, state.version);
    const SSL_CIPHER *negotiated = SSL_get_current_cipher(server->ssl());
    ASSERT_NE(nullptr, negotiated);
    EXPECT_EQ(SSL_CIPHER_get_protocol_id(negotiated), static_cast<std::uint16_t>(state.suite));
    ASSERT_EQ(2, state.alpn_len);
    EXPECT_EQ(0, std::memcmp("h2", state.alpn.data(), 2));
    EXPECT_FALSE(state.session_resumed);
    EXPECT_FALSE(state.early_data_accepted);
    EXPECT_GE(state.peer_chain.size(), 2u); // leaf + intermediate

    // ---- post-handshake app data in both directions over the live session ----
    EXPECT_EQ(1, server->handshake_step());
    // The CH now always offers psk_key_exchange_modes, so this server DOES
    // mint tickets. A TCP BoringSSL server defers them inside the SSL until
    // its next write — the zero-byte write flushes the flight, which is then
    // consumed through the read cipher to keep the record sequence in step.
    (void) SSL_write(server->ssl(), "", 0);
    open_and_discard_records(state, server->drain_wbio());

    ASSERT_EQ(4, SSL_write(server->ssl(), "ping", 4));
    const std::vector<std::uint8_t> sealed_in = server->drain_wbio();
    ASSERT_GE(sealed_in.size(), fiber::tls::kTlsRecordHeaderSize + 17);
    ASSERT_EQ(static_cast<int>(fiber::tls::TlsContentType::ApplicationData), sealed_in[0]);
    const std::size_t rec_len = (static_cast<std::size_t>(sealed_in[3]) << 8) | sealed_in[4];
    ASSERT_EQ(fiber::tls::kTlsRecordHeaderSize + rec_len, sealed_in.size());

    std::array<std::uint8_t, fiber::tls::kTlsMaxPlaintextSize> open_dst{};
    const auto opened = state.read_cipher.open(
            fiber::tls::TlsContentType::ApplicationData, (static_cast<std::uint16_t>(sealed_in[1]) << 8) | sealed_in[2],
            static_cast<std::uint16_t>(rec_len), {sealed_in.data() + fiber::tls::kTlsRecordHeaderSize, rec_len},
            open_dst);
    ASSERT_EQ(fiber::tls::TlsRecordCipher::Status::Ok, opened.status);
    EXPECT_EQ(fiber::tls::TlsContentType::ApplicationData, opened.inner_type);
    ASSERT_EQ(4u, opened.plain_len);
    EXPECT_EQ(0, std::memcmp("ping", open_dst.data(), 4));

    std::array<std::uint8_t, 64> seal_dst{};
    const auto sealed_out = state.write_cipher.seal(fiber::tls::TlsContentType::ApplicationData,
                                                    {reinterpret_cast<const std::uint8_t *>("y"), 1}, seal_dst);
    ASSERT_EQ(fiber::tls::TlsRecordCipher::Status::Ok, sealed_out.status);
    std::array<std::uint8_t, fiber::tls::kTlsRecordHeaderSize + 64> wire{};
    wire[0] = static_cast<std::uint8_t>(fiber::tls::TlsContentType::ApplicationData);
    wire[1] = 0x03;
    wire[2] = 0x03;
    wire[3] = static_cast<std::uint8_t>(sealed_out.out_len >> 8);
    wire[4] = static_cast<std::uint8_t>(sealed_out.out_len);
    std::memcpy(wire.data() + fiber::tls::kTlsRecordHeaderSize, seal_dst.data(), sealed_out.out_len);
    ASSERT_TRUE(server->ship(wire));

    char back = '\0';
    ASSERT_EQ(1, SSL_read(server->ssl(), &back, 1));
    EXPECT_EQ('y', back);
}

TEST(TlsClientHandshake13ByteFeed, OneByteAtATimeCompletes) {
    auto server = BoringServer::make(ServerOptions{});
    ASSERT_NE(nullptr, server);
    ClientMaterial material;
    const TlsClientConfig cfg = material.config("example.com", certfix::kRefNowMs);

    TlsClientHandshakeEngine engine(cfg, nullptr, material.pool);
    DriveLog log;
    ASSERT_TRUE(drive(*server, engine, true, log)); // every flight fed 1 byte at a time

    TlsConnectedState state = engine.take_state();
    EXPECT_EQ(fiber::tls::TlsProtocolVersion::Tls13, state.version);
    // The server's 1.3 suite cannot be pinned in this BoringSSL build, so the
    // assertion is agreement with what the server actually negotiated.
    EXPECT_EQ(static_cast<std::uint16_t>(state.suite),
              SSL_CIPHER_get_protocol_id(SSL_get_current_cipher(server->ssl())));
    EXPECT_EQ(2, state.alpn_len);
    EXPECT_FALSE(state.session_resumed);
}

// =====================================================================
// §8.3 — HelloRetryRequest (server restricted to P-256)
// =====================================================================

TEST(TlsClientHandshake13Hrr, SecondFlightAfterRetryCompletes) {
    auto server = BoringServer::make(ServerOptions{.groups = "P-256"});
    ASSERT_NE(nullptr, server);
    ClientMaterial material;
    const TlsClientConfig cfg = material.config("example.com", certfix::kRefNowMs);

    TlsClientHandshakeEngine engine(cfg, nullptr, material.pool);
    DriveLog log;
    ASSERT_TRUE(drive(*server, engine, false, log));

    // The HRR really happened: its sentinel random appears in the server stream.
    const auto &sentinel = fiber::tls::kTlsHelloRetryRandom;
    const std::span<const std::uint8_t> wire(log.server_to_client);
    bool found = false;
    for (std::size_t i = 0; i + sentinel.size() <= wire.size(); ++i) {
        if (std::memcmp(wire.data() + i, sentinel.data(), sentinel.size()) == 0) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);

    TlsConnectedState state = engine.take_state();
    EXPECT_EQ(fiber::tls::TlsProtocolVersion::Tls13, state.version);
    EXPECT_FALSE(state.session_resumed);
    EXPECT_GE(state.peer_chain.size(), 1u);
}

// =====================================================================
// §8.4 — client certificates (mTLS)
// =====================================================================

TEST(TlsClientHandshake13Mtls, ServerRequiresClientCertificate) {
    auto server =
            BoringServer::make(ServerOptions{.client_trust_pem = certfix::kRootRsaPem, .require_client_cert = true});
    ASSERT_NE(nullptr, server);
    ClientMaterial material;
    ASSERT_NO_FATAL_FAILURE(material.load_client_credential());
    const TlsClientConfig cfg = material.config("example.com", certfix::kRefNowMs);

    TlsClientHandshakeEngine engine(cfg, nullptr, material.pool);
    DriveLog log;
    ASSERT_TRUE(drive(*server, engine, false, log));

    EXPECT_EQ(1, server->handshake_step());
    X509 *peer = SSL_get_peer_certificate(server->ssl());
    EXPECT_NE(nullptr, peer);
    X509_free(peer);
}

TEST(TlsClientHandshake13Mtls, NoCredentialAnswersEmptyCertificate) {
    auto server =
            BoringServer::make(ServerOptions{.client_trust_pem = certfix::kRootRsaPem, .require_client_cert = false});
    ASSERT_NE(nullptr, server);
    ClientMaterial material; // no client credential loaded
    const TlsClientConfig cfg = material.config("example.com", certfix::kRefNowMs);

    TlsClientHandshakeEngine engine(cfg, nullptr, material.pool);
    DriveLog log;
    ASSERT_TRUE(drive(*server, engine, false, log));

    EXPECT_EQ(1, server->handshake_step());
    X509 *peer = SSL_get_peer_certificate(server->ssl());
    EXPECT_EQ(nullptr, peer); // empty chain accepted by the optional request
}

// =====================================================================
// §8.5 — failure matrix
// =====================================================================

namespace {

void expect_engine_failure(const ServerOptions &server_opt, const TlsClientConfig &cfg, IoBufNodePool &pool,
                           TlsAlertDesc expected) {
    auto server = BoringServer::make(server_opt);
    ASSERT_NE(nullptr, server);
    TlsClientHandshakeEngine engine(cfg, nullptr, pool);
    ASSERT_TRUE(engine.done() == false);

    DriveLog log;
    EXPECT_FALSE(drive(*server, engine, false, log));
    ASSERT_TRUE(engine.done());
    ASSERT_TRUE(engine.failed());
    EXPECT_EQ(expected, engine.failure_alert());

    const std::optional<int> desc = last_alert_desc(log.client_to_server);
    ASSERT_TRUE(desc.has_value());
    EXPECT_EQ(static_cast<int>(expected), *desc); // the fatal alert went on the wire
}

} // namespace

TEST(TlsClientHandshake13Failure, UntrustedRootSendsUnknownCa) {
    ClientMaterial material;
    auto unrelated =
            TlsTrustStore::from_pem_bundle({certfix::kRootUnrelatedPem, std::strlen(certfix::kRootUnrelatedPem)});
    ASSERT_TRUE(unrelated.has_value());
    material.trust = std::move(unrelated).value();
    const TlsClientConfig cfg = material.config("example.com", certfix::kRefNowMs);

    expect_engine_failure(ServerOptions{}, cfg, material.pool, TlsAlertDesc::UnknownCa);
}

TEST(TlsClientHandshake13Failure, ExpiredLeafSendsCertificateExpired) {
    ClientMaterial material;
    // The leaf's SAN is example.com (the CN alone would never reach the
    // window check — matching is SAN-only); this timestamp is past notAfter.
    const TlsClientConfig cfg = material.config("example.com", certfix::kRefNowMs + 3650LL * 24 * 3600 * 1000);

    expect_engine_failure(ServerOptions{.leaf_pem = certfix::kLeafExpiredPem}, cfg, material.pool,
                          TlsAlertDesc::CertificateExpired);
}

TEST(TlsClientHandshake13Failure, HostnameMismatchSendsBadCertificate) {
    ClientMaterial material;
    // The wrongname leaf's SAN is other.example.com — example.com matches
    // neither it nor the CN (no wildcard anywhere).
    const TlsClientConfig cfg = material.config("example.com", certfix::kRefNowMs);

    expect_engine_failure(ServerOptions{.leaf_pem = certfix::kLeafWrongNamePem}, cfg, material.pool,
                          TlsAlertDesc::BadCertificate);
}

TEST(TlsClientHandshake13Failure, TamperedSealedRecordFailsAuthentication) {
    auto server = BoringServer::make(ServerOptions{});
    ASSERT_NE(nullptr, server);
    ClientMaterial material;
    const TlsClientConfig cfg = material.config("example.com", certfix::kRefNowMs);

    TlsClientHandshakeEngine engine(cfg, nullptr, material.pool);
    ASSERT_TRUE(engine.done() == false);

    // Ship the first flight, take the server's complete flight, flip the LAST
    // byte (inside the sealed server-Finished AEAD tag) and feed it whole.
    DriveLog log;
    ASSERT_TRUE(server->ship(chain_bytes(engine.take_output())));
    ASSERT_EQ(0, server->handshake_step());
    std::vector<std::uint8_t> flight = server->drain_wbio();
    ASSERT_GE(flight.size(), 32u);
    flight.back() ^= 0x55;

    Event event = Event::None;
    ASSERT_TRUE(feed_bytes(engine, flight, false, event));
    ASSERT_TRUE(engine.done());
    ASSERT_TRUE(engine.failed());
    EXPECT_EQ(TlsAlertDesc::BadRecordMac, engine.failure_alert());

    const std::optional<int> desc = last_alert_desc(chain_bytes(engine.take_output()));
    ASSERT_TRUE(desc.has_value());
    EXPECT_EQ(static_cast<int>(TlsAlertDesc::BadRecordMac), *desc);
}

// =====================================================================
// §8.2 — TLS 1.2 sub-flow (06 §4.3): SH(1.2) → Cert → SKE → [CR] → SHD →
// client flight → [NST…] → server CCS → server Fin. The 1.2 suite IS
// server-pinnable (SSL_CTX_set_cipher_list), so every offered 1.2 suite is
// driven against the peer that selected it.
// =====================================================================

TEST(TlsClientHandshake12Full, HandshakeAndAppRoundTrip) {
    auto server = BoringServer::make(ServerOptions{.tls12_cipher = "ECDHE-RSA-AES128-GCM-SHA256"});
    ASSERT_NE(nullptr, server);
    ClientMaterial material;
    const TlsClientConfig cfg = material.config("example.com", certfix::kRefNowMs);

    TlsClientHandshakeEngine engine(cfg, nullptr, material.pool);
    ASSERT_FALSE(engine.done());

    DriveLog log;
    ASSERT_TRUE(drive(*server, engine, false, log));

    TlsConnectedState state = engine.take_state();
    EXPECT_EQ(fiber::tls::TlsProtocolVersion::Tls12, state.version);
    EXPECT_EQ(static_cast<std::uint16_t>(TlsCipherSuiteId::EcdheRsaAes128GcmSha256),
              static_cast<std::uint16_t>(state.suite));
    const SSL_CIPHER *negotiated = SSL_get_current_cipher(server->ssl());
    ASSERT_NE(nullptr, negotiated);
    EXPECT_EQ(SSL_CIPHER_get_protocol_id(negotiated), static_cast<std::uint16_t>(state.suite));
    ASSERT_EQ(2, state.alpn_len);
    EXPECT_EQ(0, std::memcmp("h2", state.alpn.data(), 2));
    EXPECT_FALSE(state.session_resumed);
    EXPECT_FALSE(state.early_data_accepted);
    EXPECT_GE(state.peer_chain.size(), 2u); // leaf + intermediate
    EXPECT_TRUE(state.tls12_master.len() == 48); // AES128 suite PRF is SHA-256
    EXPECT_TRUE(state.client_app_secret.empty() && state.resumption_master.empty()); // 1.3-only fields

    // ---- post-handshake app data both ways over the moved key_block ciphers ----
    EXPECT_EQ(1, server->handshake_step());

    ASSERT_EQ(4, SSL_write(server->ssl(), "ping", 4));
    const std::vector<std::uint8_t> sealed_in = server->drain_wbio();
    ASSERT_GE(sealed_in.size(), fiber::tls::kTlsRecordHeaderSize + 24);
    ASSERT_EQ(static_cast<int>(fiber::tls::TlsContentType::ApplicationData), sealed_in[0]);
    const std::size_t rec_len = (static_cast<std::size_t>(sealed_in[3]) << 8) | sealed_in[4];
    ASSERT_EQ(fiber::tls::kTlsRecordHeaderSize + rec_len, sealed_in.size());

    std::array<std::uint8_t, fiber::tls::kTlsMaxPlaintextSize> open_dst{};
    const auto opened = state.read_cipher.open(
            fiber::tls::TlsContentType::ApplicationData, (static_cast<std::uint16_t>(sealed_in[1]) << 8) | sealed_in[2],
            static_cast<std::uint16_t>(rec_len), {sealed_in.data() + fiber::tls::kTlsRecordHeaderSize, rec_len},
            open_dst);
    ASSERT_EQ(fiber::tls::TlsRecordCipher::Status::Ok, opened.status);
    ASSERT_EQ(4u, opened.plain_len);
    EXPECT_EQ(0, std::memcmp("ping", open_dst.data(), 4));

    std::array<std::uint8_t, 64> seal_dst{};
    const auto sealed_out = state.write_cipher.seal(fiber::tls::TlsContentType::ApplicationData,
                                                    {reinterpret_cast<const std::uint8_t *>("y"), 1}, seal_dst);
    ASSERT_EQ(fiber::tls::TlsRecordCipher::Status::Ok, sealed_out.status);
    std::array<std::uint8_t, fiber::tls::kTlsRecordHeaderSize + 64> wire{};
    wire[0] = static_cast<std::uint8_t>(fiber::tls::TlsContentType::ApplicationData);
    wire[1] = 0x03;
    wire[2] = 0x03;
    wire[3] = static_cast<std::uint8_t>(sealed_out.out_len >> 8);
    wire[4] = static_cast<std::uint8_t>(sealed_out.out_len);
    std::memcpy(wire.data() + fiber::tls::kTlsRecordHeaderSize, seal_dst.data(), sealed_out.out_len);
    ASSERT_TRUE(server->ship(wire));

    char back = '\0';
    ASSERT_EQ(1, SSL_read(server->ssl(), &back, 1));
    EXPECT_EQ('y', back);
}

TEST(TlsClientHandshake12Full, Aes256Sha384SuiteAgrees) {
    // 0xC030 — the IANA value for ECDHE-RSA-AES256-GCM-SHA384 (a historical
    // 0x0030 typo in the suite table would have dropped this offer entirely).
    auto server = BoringServer::make(ServerOptions{.tls12_cipher = "ECDHE-RSA-AES256-GCM-SHA384"});
    ASSERT_NE(nullptr, server);
    ClientMaterial material;
    const TlsClientConfig cfg = material.config("example.com", certfix::kRefNowMs);

    TlsClientHandshakeEngine engine(cfg, nullptr, material.pool);
    DriveLog log;
    ASSERT_TRUE(drive(*server, engine, false, log));

    TlsConnectedState state = engine.take_state();
    EXPECT_EQ(fiber::tls::TlsProtocolVersion::Tls12, state.version);
    EXPECT_EQ(static_cast<std::uint16_t>(TlsCipherSuiteId::EcdheRsaAes256GcmSha384),
              static_cast<std::uint16_t>(state.suite));
    EXPECT_TRUE(state.tls12_master.len() == 48); // SHA-384 suite PRF is SHA-384
}

TEST(TlsClientHandshake12Full, EcdsaSuiteAgrees) {
    // P-256 leaf (signed by the RSA intermediate) + ECDHE-ECDSA-AES128-GCM:
    // exercises the SKE signature verification path with an ECDSA key.
    auto server = BoringServer::make(ServerOptions{
            .leaf_pem = certfix::kLeafEcP256Pem,
            .key_pem = certfix::kP256KeyPem,
            .tls12_cipher = "ECDHE-ECDSA-AES128-GCM-SHA256",
    });
    ASSERT_NE(nullptr, server);
    ClientMaterial material;
    const TlsClientConfig cfg = material.config("example.com", certfix::kRefNowMs);

    TlsClientHandshakeEngine engine(cfg, nullptr, material.pool);
    DriveLog log;
    ASSERT_TRUE(drive(*server, engine, false, log));

    TlsConnectedState state = engine.take_state();
    EXPECT_EQ(fiber::tls::TlsProtocolVersion::Tls12, state.version);
    EXPECT_EQ(static_cast<std::uint16_t>(TlsCipherSuiteId::EcdheEcdsaAes128GcmSha256),
              static_cast<std::uint16_t>(state.suite));
    EXPECT_GE(state.peer_chain.size(), 1u);
}

TEST(TlsClientHandshake12ByteFeed, OneByteAtATimeCompletes) {
    auto server = BoringServer::make(ServerOptions{.tls12_cipher = "ECDHE-RSA-AES128-GCM-SHA256"});
    ASSERT_NE(nullptr, server);
    ClientMaterial material;
    const TlsClientConfig cfg = material.config("example.com", certfix::kRefNowMs);

    TlsClientHandshakeEngine engine(cfg, nullptr, material.pool);
    DriveLog log;
    ASSERT_TRUE(drive(*server, engine, true, log)); // every flight fed 1 byte at a time

    TlsConnectedState state = engine.take_state();
    EXPECT_EQ(fiber::tls::TlsProtocolVersion::Tls12, state.version);
    EXPECT_EQ(static_cast<std::uint16_t>(TlsCipherSuiteId::EcdheRsaAes128GcmSha256),
              static_cast<std::uint16_t>(state.suite));
}

TEST(TlsClientHandshake12Mtls, ServerRequiresClientCertificate) {
    auto server = BoringServer::make(ServerOptions{
            .client_trust_pem = certfix::kRootRsaPem,
            .require_client_cert = true,
            .tls12_cipher = "ECDHE-RSA-AES128-GCM-SHA256",
    });
    ASSERT_NE(nullptr, server);
    ClientMaterial material;
    ASSERT_NO_FATAL_FAILURE(material.load_client_credential());
    const TlsClientConfig cfg = material.config("example.com", certfix::kRefNowMs);

    TlsClientHandshakeEngine engine(cfg, nullptr, material.pool);
    DriveLog log;
    ASSERT_TRUE(drive(*server, engine, false, log));

    EXPECT_EQ(1, server->handshake_step());
    X509 *peer = SSL_get_peer_certificate(server->ssl());
    EXPECT_NE(nullptr, peer); // the 1.2 CV over the raw transcript verified
    X509_free(peer);
}

// =====================================================================
// §8.7 — ServerHello(1.2) boundary rejections, via in-flight mutation of a
// real BoringSSL first flight. Record-relative offsets (the SH is the first
// message of the first record): header 5B, then message header 4B, then
// legacy_version(2) random(32) sid_len(1) sid cipher(2).
// =====================================================================

namespace {

void expect_reject_after_sh_mutation(const ServerOptions &opt, const TlsClientConfig &cfg, IoBufNodePool &pool,
                                     void (*mutate)(std::vector<std::uint8_t> &), TlsAlertDesc expected) {
    auto server = BoringServer::make(opt);
    ASSERT_NE(nullptr, server);
    TlsClientHandshakeEngine engine(cfg, nullptr, pool);
    ASSERT_FALSE(engine.done());

    ASSERT_TRUE(server->ship(chain_bytes(engine.take_output())));
    ASSERT_EQ(0, server->handshake_step());
    std::vector<std::uint8_t> flight = server->drain_wbio();
    ASSERT_GE(flight.size(), fiber::tls::kTlsRecordHeaderSize + 4u + 2u + 32u + 1u + 32u + 2u);
    ASSERT_EQ(22, flight[0]); // plaintext handshake record
    ASSERT_EQ(2, flight[5]); // ... whose first message is a ServerHello
    mutate(flight);

    Event event = Event::None;
    ASSERT_TRUE(feed_bytes(engine, flight, false, event));
    ASSERT_TRUE(engine.done());
    ASSERT_TRUE(engine.failed());
    EXPECT_EQ(expected, engine.failure_alert());

    const std::optional<int> desc = last_alert_desc(chain_bytes(engine.take_output()));
    ASSERT_TRUE(desc.has_value());
    EXPECT_EQ(static_cast<int>(expected), *desc); // the fatal alert went on the wire
}

} // namespace

TEST(TlsClientHandshake12Reject, DowngradeSentinelAborts) {
    ClientMaterial material;
    const TlsClientConfig cfg = material.config("example.com", certfix::kRefNowMs);
    // random[24..31] = DOWNGRD||0x01 (record-relative 35..42).
    expect_reject_after_sh_mutation(
            ServerOptions{.tls12_cipher = "ECDHE-RSA-AES128-GCM-SHA256"}, cfg, material.pool,
            [](std::vector<std::uint8_t> &flight) {
                const std::array<std::uint8_t, 8> sentinel{0x44, 0x4F, 0x57, 0x4E, 0x47, 0x52, 0x44, 0x01};
                std::memcpy(flight.data() + 35, sentinel.data(), sentinel.size());
            },
            TlsAlertDesc::IllegalParameter);
}

TEST(TlsClientHandshake12Reject, SuiteNotOfferedAborts) {
    ClientMaterial material;
    const TlsClientConfig cfg = material.config("example.com", certfix::kRefNowMs);
    expect_reject_after_sh_mutation(
            ServerOptions{.tls12_cipher = "ECDHE-RSA-AES128-GCM-SHA256"}, cfg, material.pool,
            [](std::vector<std::uint8_t> &flight) {
                const std::size_t sid_len = flight[43];
                flight[44 + sid_len] = 0x00; // TLS_RSA_WITH_AES_128_CBC_SHA — never offered
                flight[45 + sid_len] = 0x2F;
            },
            TlsAlertDesc::IllegalParameter);
}

TEST(TlsClientHandshake12Reject, LegacyVersionBelowTls12Aborts) {
    ClientMaterial material;
    const TlsClientConfig cfg = material.config("example.com", certfix::kRefNowMs);
    expect_reject_after_sh_mutation(
            ServerOptions{.tls12_cipher = "ECDHE-RSA-AES128-GCM-SHA256"}, cfg, material.pool,
            [](std::vector<std::uint8_t> &flight) {
                flight[9] = 0x03; // legacy_version 0x0301 (TLS 1.0)
                flight[10] = 0x01;
            },
            TlsAlertDesc::ProtocolVersion);
}

// =====================================================================
// §8.6 — PSK resumption + 0-RTT against BoringSSL. Hop 1 is a full
// handshake on a 0-RTT-capable ctx; the test opens the post-handshake
// NewSessionTicket flight with the live session's read cipher and derives
// the resumption PSK (Expand-Label(resumption_master, "resumption", nonce))
// — replicating the future 08 logic the engine only consumes via
// TlsSessionOffer. Hop 2 shares hop 1's ctx: BoringSSL mints per-ctx ticket
// keys, so a fresh ctx cannot decrypt the ticket.
// =====================================================================

namespace {

struct ResumptionTicket {
    std::vector<std::uint8_t> identity; // opaque ticket bytes
    std::vector<std::uint8_t> nonce; // ticket_nonce from the NST
    std::uint32_t age_add = 0;
};

// Feeds `wire` (post-handshake NST records) to nothing — the engine is Done
// and post-handshake traffic belongs to the session layer — and instead
// decrypts it directly with the connected state's server_app0 read cipher,
// returning the FIRST NewSessionTicket parsed (BoringSSL may emit several).
bool collect_first_ticket(TlsConnectedState &state, const std::vector<std::uint8_t> &wire, ResumptionTicket &out) {
    namespace ft = fiber::tls;
    std::array<std::uint8_t, ft::kTlsMaxPlaintextSize> dst{};
    std::size_t off = 0;
    while (off + ft::kTlsRecordHeaderSize <= wire.size()) {
        const std::size_t len = (static_cast<std::size_t>(wire[off + 3]) << 8) | wire[off + 4];
        if (off + ft::kTlsRecordHeaderSize + len > wire.size() || len < 5) {
            return false;
        }
        if (wire[off] != static_cast<std::uint8_t>(ft::TlsContentType::ApplicationData)) {
            return false; // NSTs fly as sealed app-data records
        }
        const auto opened = state.read_cipher.open(
                ft::TlsContentType::ApplicationData, (static_cast<std::uint16_t>(wire[off + 1]) << 8) | wire[off + 2],
                static_cast<std::uint16_t>(len), {wire.data() + off + ft::kTlsRecordHeaderSize, len}, dst);
        if (opened.status != ft::TlsRecordCipher::Status::Ok || opened.inner_type != ft::TlsContentType::Handshake) {
            return false;
        }
        // Handshake messages inside the opened record (usually exactly one).
        std::size_t msg = 0;
        while (msg + 4 <= opened.plain_len) {
            const std::size_t body_len = (static_cast<std::size_t>(dst[msg + 1]) << 16) |
                                         (static_cast<std::size_t>(dst[msg + 2]) << 8) |
                                         static_cast<std::size_t>(dst[msg + 3]);
            if (msg + 4 + body_len > opened.plain_len || body_len < 11) {
                return false;
            }
            if (dst[msg] == static_cast<std::uint8_t>(ft::TlsHandshakeType::NewSessionTicket)) {
                const std::uint8_t *body = dst.data() + msg + 4;
                out.age_add = (static_cast<std::uint32_t>(body[4]) << 24) |
                              (static_cast<std::uint32_t>(body[5]) << 16) | (static_cast<std::uint32_t>(body[6]) << 8) |
                              static_cast<std::uint32_t>(body[7]);
                const std::size_t nonce_len = body[8];
                if (9 + nonce_len + 2 > body_len) {
                    return false;
                }
                out.nonce.assign(body + 9, body + 9 + nonce_len);
                const std::size_t ticket_len = (static_cast<std::size_t>(body[9 + nonce_len]) << 8) |
                                               static_cast<std::size_t>(body[10 + nonce_len]);
                if (11 + nonce_len + ticket_len > body_len) {
                    return false;
                }
                out.identity.assign(body + 11 + nonce_len, body + 11 + nonce_len + ticket_len);
                return !out.identity.empty();
            }
            msg += 4 + body_len;
        }
        off += ft::kTlsRecordHeaderSize + len;
    }
    return false;
}

// Hop 1: full handshake on an early-data-capable ctx, completes the server
// (tickets are written at handshake completion), returns the first ticket
// plus the PSK derived from the connected state's resumption master.
struct Hop1Result {
    std::unique_ptr<BoringServer> server; // keep alive: owns the shared ctx
    ResumptionTicket ticket;
    fiber::tls::TlsSecret psk; // resumption PSK, feeds the hop-2 offer
    fiber::tls::TlsCipherSuiteId suite = fiber::tls::TlsCipherSuiteId::TlsAes128GcmSha256;
};

bool drive_hop1(Hop1Result &out) {
    out.server = BoringServer::make(ServerOptions{.early_data = true});
    if (out.server == nullptr) {
        std::fprintf(stderr, "[HOP1] server make failed\n");
        return false;
    }
    ClientMaterial material;
    const TlsClientConfig cfg = material.config("example.com", certfix::kRefNowMs);
    TlsClientHandshakeEngine engine(cfg, nullptr, material.pool);
    DriveLog log;
    if (!drive(*out.server, engine, false, log)) {
        return false;
    }
    TlsConnectedState state = engine.take_state();
    EXPECT_FALSE(state.session_resumed);
    if (state.resumption_master.empty()) {
        return false; // no resumption base — nothing to offer hop 2
    }
    // Finish the server side: consuming the client Fin is what mints the
    // NewSessionTicket flight. A TCP BoringSSL server DEFERS those records
    // inside the SSL until its next write — the zero-byte SSL_write flushes
    // them into the wbio without emitting an app-data record.
    if (out.server->handshake_step() != 1) {
        return false;
    }
    (void) SSL_write(out.server->ssl(), "", 0);
    const std::vector<std::uint8_t> nst_wire = out.server->drain_wbio();
    if (!collect_first_ticket(state, nst_wire, out.ticket)) {
        return false;
    }
    auto psk = fiber::tls::tls13_resumption_psk(state.resumption_master, out.ticket.nonce);
    if (!psk.has_value()) {
        return false;
    }
    out.psk = std::move(psk).value();
    out.suite = state.suite;
    return true;
}

// Assembles the hop-2 offer from the hop-1 result. The spans borrow
// ticket/psk storage that must outlive the hop-2 engine (the Hop1Result
// does, when kept alive in the test scope).
TlsSessionOffer make_offer(const Hop1Result &hop1, std::size_t max_early_data) {
    TlsSessionOffer offer;
    offer.identity = hop1.ticket.identity;
    offer.obfuscated_ticket_age = hop1.ticket.age_add; // zero elapsed — immediate hop
    offer.suite = hop1.suite;
    offer.psk = hop1.psk.bytes();
    offer.max_early_data = max_early_data;
    return offer;
}

} // namespace

TEST(TlsClientHandshake13Psk, ResumesWithoutCertificates) {
    Hop1Result hop1;
    ASSERT_TRUE(drive_hop1(hop1));
    auto server2 = BoringServer::make_on_ctx(hop1.server->ctx());
    ASSERT_NE(nullptr, server2);

    ClientMaterial material;
    const TlsClientConfig cfg = material.config("example.com", certfix::kRefNowMs);
    TlsSessionOffer offer = make_offer(hop1, 0); // no early_data extension
    TlsClientHandshakeEngine engine(cfg, &offer, material.pool);
    DriveLog log;
    ASSERT_TRUE(drive(*server2, engine, false, log));

    TlsConnectedState state = engine.take_state();
    EXPECT_EQ(fiber::tls::TlsProtocolVersion::Tls13, state.version);
    EXPECT_TRUE(state.session_resumed); // SH selected_identity == 0
    EXPECT_FALSE(state.early_data_accepted);
    EXPECT_TRUE(state.peer_chain.empty()); // PSK flow: no Cert/CV flight
    EXPECT_FALSE(state.resumption_master.empty()); // hop 3's base

    // The server resumed: no certificate flight on its side either, and the
    // app-data path works on the resumed keys.
    EXPECT_EQ(1, server2->handshake_step());
    EXPECT_EQ(1, SSL_session_reused(server2->ssl()));
    // Zero-byte write: a TCP BoringSSL server defers its NewSessionTicket
    // flight inside the SSL until the next write — flush it out so the ping
    // record below drains alone.
    (void) SSL_write(server2->ssl(), "", 0);
    open_and_discard_records(state, server2->drain_wbio());

    ASSERT_EQ(4, SSL_write(server2->ssl(), "ping", 4));
    const std::vector<std::uint8_t> sealed_in = server2->drain_wbio();
    ASSERT_GT(sealed_in.size(), fiber::tls::kTlsRecordHeaderSize);
    const std::size_t rec_len = (static_cast<std::size_t>(sealed_in[3]) << 8) | sealed_in[4];
    std::array<std::uint8_t, fiber::tls::kTlsMaxPlaintextSize> open_dst{};
    const auto opened = state.read_cipher.open(
            fiber::tls::TlsContentType::ApplicationData, (static_cast<std::uint16_t>(sealed_in[1]) << 8) | sealed_in[2],
            static_cast<std::uint16_t>(rec_len), {sealed_in.data() + fiber::tls::kTlsRecordHeaderSize, rec_len},
            open_dst);
    ASSERT_EQ(fiber::tls::TlsRecordCipher::Status::Ok, opened.status);
    ASSERT_EQ(4u, opened.plain_len);
    EXPECT_EQ(0, std::memcmp("ping", open_dst.data(), 4));
}

TEST(TlsClientHandshake13Psk, EarlyDataAccepted) {
    Hop1Result hop1;
    ASSERT_TRUE(drive_hop1(hop1));
    // Same ctx, early data on: the ticket was issued 0-RTT-capable and the
    // ALPN ("h2") matches the one bound into the ticket.
    auto server2 = BoringServer::make_on_ctx(hop1.server->ctx());
    ASSERT_NE(nullptr, server2);

    ClientMaterial material;
    const TlsClientConfig cfg = material.config("example.com", certfix::kRefNowMs);
    TlsSessionOffer offer = make_offer(hop1, 14336); // BoringSSL kMaxEarlyDataAccepted
    TlsClientHandshakeEngine engine(cfg, &offer, material.pool);

    // 0-RTT window: early records ride out with the ClientHello, sealed
    // under the early-traffic keys (engine ctor already queued CH + CCS).
    const std::string_view request = "GET /early HTTP/1.1\r\nhost: example.com\r\n\r\n";
    ASSERT_TRUE(engine.write_early_data({reinterpret_cast<const std::uint8_t *>(request.data()), request.size()})
                        .has_value());

    DriveLog log;
    ASSERT_TRUE(drive(*server2, engine, false, log));

    TlsConnectedState state = engine.take_state();
    EXPECT_TRUE(state.session_resumed);
    EXPECT_TRUE(state.early_data_accepted); // EE carried early_data
    // The window is closed at Done: no more early writes on this engine.
    EXPECT_FALSE(engine.write_early_data({reinterpret_cast<const std::uint8_t *>("x"), 1}).has_value());

    // The server consumed EndOfEarlyData + the client Finished when this
    // handshake_step processed the rbio (a decrypt failure on either would
    // fail the step): EOED opened under the EARLY keys (RFC 8446 §4.5 — the
    // sequence continues the early-data records) and Fin under the fresh
    // client_hs sequence. With the second flight consumed the handshake is
    // genuinely complete, not the 0-RTT early-return.
    EXPECT_EQ(1, server2->handshake_step());
    EXPECT_EQ(1, SSL_early_data_accepted(server2->ssl()));
    EXPECT_EQ(1, SSL_session_reused(server2->ssl()));

    // The server hands the accepted early data back through SSL_read.
    std::array<char, 128> early{};
    const int got = SSL_read(server2->ssl(), early.data(), static_cast<int>(early.size()));
    ASSERT_GT(got, 0);
    EXPECT_EQ(0, std::memcmp(request.data(), early.data(), request.size()));
    // No further app data was pending: only EOED/Fin rode in the rbio.
    std::array<char, 64> sink{};
    ASSERT_EQ(SSL_ERROR_WANT_READ, SSL_get_error(server2->ssl(), SSL_read(server2->ssl(), sink.data(), sizeof sink)));

    // The half-RTT NST rode out with the SH flight; the engine finished at
    // the server Finished and never fed it. Open it through the connected
    // state's read cipher — this advances the application-key sequence to
    // where the server's app records start.
    open_and_discard_records(state, log.server_tail);

    ASSERT_EQ(4, SSL_write(server2->ssl(), "ping", 4)); // server_app0 write: post-EOED keys
    const std::vector<std::uint8_t> sealed = server2->drain_wbio();
    ASSERT_GT(sealed.size(), fiber::tls::kTlsRecordHeaderSize);
    const std::size_t rec_len = (static_cast<std::size_t>(sealed[3]) << 8) | sealed[4];
    std::array<std::uint8_t, fiber::tls::kTlsMaxPlaintextSize> open_dst{};
    const auto opened = state.read_cipher.open(
            fiber::tls::TlsContentType::ApplicationData, (static_cast<std::uint16_t>(sealed[1]) << 8) | sealed[2],
            static_cast<std::uint16_t>(rec_len), {sealed.data() + fiber::tls::kTlsRecordHeaderSize, rec_len}, open_dst);
    ASSERT_EQ(fiber::tls::TlsRecordCipher::Status::Ok, opened.status);
    ASSERT_EQ(4u, opened.plain_len);
    EXPECT_EQ(0, std::memcmp("ping", open_dst.data(), 4));
}

TEST(TlsClientHandshake13Psk, EarlyDataRejectedStillResumes) {
    Hop1Result hop1;
    ASSERT_TRUE(drive_hop1(hop1));
    // Same ctx (ticket decrypts, PSK resumes) but early data disabled on the
    // connection: the server skips the early records and omits early_data
    // from EE — resumption stands, 0-RTT does not.
    auto server2 = BoringServer::make_on_ctx(hop1.server->ctx());
    ASSERT_NE(nullptr, server2);
    SSL_set_early_data_enabled(server2->ssl(), 0);

    ClientMaterial material;
    const TlsClientConfig cfg = material.config("example.com", certfix::kRefNowMs);
    TlsSessionOffer offer = make_offer(hop1, 14336);
    TlsClientHandshakeEngine engine(cfg, &offer, material.pool);

    // The early write itself succeeds — rejection is only learnable from the
    // SH/EE, exactly the 06 contract (write_early_data flips to Invalid at
    // the SH read point, before the handshake finishes).
    const std::string_view request = "GET /early HTTP/1.1\r\nhost: example.com\r\n\r\n";
    ASSERT_TRUE(engine.write_early_data({reinterpret_cast<const std::uint8_t *>(request.data()), request.size()})
                        .has_value());

    DriveLog log;
    ASSERT_TRUE(drive(*server2, engine, false, log));

    TlsConnectedState state = engine.take_state();
    EXPECT_EQ(fiber::tls::TlsProtocolVersion::Tls13, state.version);
    EXPECT_TRUE(state.session_resumed); // the ticket itself was honored
    EXPECT_FALSE(state.early_data_accepted);
    EXPECT_FALSE(engine.write_early_data({reinterpret_cast<const std::uint8_t *>("x"), 1}).has_value());

    // Server completes the full resumed handshake (no Cert/CV either way)
    // and reports 0-RTT rejected while the session itself was reused.
    EXPECT_EQ(1, server2->handshake_step());
    EXPECT_EQ(0, SSL_early_data_accepted(server2->ssl()));
    EXPECT_EQ(1, SSL_session_reused(server2->ssl()));
}
