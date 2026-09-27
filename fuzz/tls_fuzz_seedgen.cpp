// Seed-corpus generator for the TLS fuzzers. Drives the in-tree engines
// against BoringSSL peers (memory BIOs) and writes real handshake streams:
//
//   tls_server_engine_fuzzer  — everything a BoringSSL client sent to our
//                               server engine (the harness's own material and
//                               ticket service, so resumption tickets open)
//   tls_client_engine_fuzzer  — everything a BoringSSL server sent to our
//                               client engine
//   tls_handshake_codec_fuzzer — the plaintext message bodies of those
//                               streams, plus encoder output for the 1.3
//                               messages that only ever fly encrypted
//   tls_connection_fuzzer     — hand-built record sequences
//
// Usage: tls_fuzz_seedgen <corpus-root>   (writes <root>/<fuzzer>/<name>)
// Regenerate after protocol changes; the files are committed under
// fuzz/corpus/.

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "TlsFuzzCommon.h"

#include <fiber/common/mem/IoBufChain.h>
#include <fiber/tls/handshake/TlsClientHandshakeEngine.h>
#include <fiber/tls/handshake/TlsHandshakeCodec.h>
#include <fiber/tls/handshake/TlsServerHandshakeEngine.h>

namespace {

namespace fs = std::filesystem;
namespace certfix = fiber::tls::certfix;
using Bytes = std::vector<std::uint8_t>;

fs::path g_root;
int g_written = 0;

void write_seed(const char *fuzzer, const std::string &name, const Bytes &bytes) {
    const fs::path dir = g_root / fuzzer;
    fs::create_directories(dir);
    std::ofstream out(dir / name, std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    ++g_written;
}

Bytes prefixed(std::uint8_t control, const Bytes &stream) {
    Bytes out{control};
    out.insert(out.end(), stream.begin(), stream.end());
    return out;
}

Bytes chain_bytes(fiber::mem::IoBufChain chain) {
    Bytes out;
    while (const fiber::mem::IoBuf *front = chain.first_readable()) {
        out.insert(out.end(), front->readable_data(), front->readable_data() + front->readable());
        chain.consume_and_compact(front->readable());
    }
    return out;
}

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

// ---- a BoringSSL endpoint over memory BIOs ----

SSL_SESSION *g_last_session = nullptr;

int keep_session(SSL *, SSL_SESSION *session) {
    if (g_last_session != nullptr) {
        SSL_SESSION_free(g_last_session);
    }
    g_last_session = session; // we take the reference
    return 1;
}

int select_first_alpn(SSL *, const unsigned char **out, unsigned char *out_len, const unsigned char *in,
                      unsigned in_len, void *) {
    if (in_len < 1 || in[0] + 1u > in_len) {
        return SSL_TLSEXT_ERR_NOACK;
    }
    *out = in + 1;
    *out_len = in[0];
    return SSL_TLSEXT_ERR_OK;
}

struct PeerOptions {
    bool server = false;
    bool tls12_only = false;
    const char *groups = nullptr;
    const char *tls12_cipher = nullptr;
    bool grease = false;
    bool client_cert = false; // client: send one / server: require one
    bool collect_session = false;
    SSL_SESSION *resume = nullptr;
    bool early_data = false;
    const char *leaf_pem = certfix::kLeafRsaPem;
    const char *key_pem = certfix::kRsa2048KeyPem;
};

class Peer {
public:
    explicit Peer(const PeerOptions &opt) {
        ctx_ = SSL_CTX_new(opt.server ? TLS_server_method() : TLS_client_method());
        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
        SSL_CTX_set_max_proto_version(ctx_, opt.tls12_only ? TLS1_2_VERSION : TLS1_3_VERSION);
        if (opt.grease) {
            SSL_CTX_set_grease_enabled(ctx_, 1);
            SSL_CTX_set_permute_extensions(ctx_, 1);
        }
        if (opt.tls12_cipher != nullptr) {
            SSL_CTX_set_cipher_list(ctx_, opt.tls12_cipher);
        }
        if (opt.groups != nullptr) {
            SSL_CTX_set1_groups_list(ctx_, opt.groups);
        }
        if (opt.early_data) {
            SSL_CTX_set_early_data_enabled(ctx_, 1);
        }
        X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx_), load_cert(certfix::kRootRsaPem));
        if (opt.server) {
            SSL_CTX_use_certificate(ctx_, load_cert(opt.leaf_pem));
            SSL_CTX_add0_chain_cert(ctx_, load_cert(certfix::kIntermediateRsaPem));
            SSL_CTX_use_PrivateKey(ctx_, load_key(opt.key_pem));
            SSL_CTX_set_alpn_select_cb(ctx_, select_first_alpn, nullptr);
            if (opt.client_cert) {
                SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
            }
        } else {
            SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
            if (opt.collect_session) {
                SSL_CTX_set_session_cache_mode(ctx_, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
                SSL_CTX_sess_set_new_cb(ctx_, keep_session);
            }
        }
        ssl_ = SSL_new(ctx_);
        rbio_ = BIO_new(BIO_s_mem());
        wbio_ = BIO_new(BIO_s_mem());
        SSL_set_bio(ssl_, rbio_, wbio_);
        if (opt.server) {
            SSL_set_accept_state(ssl_);
        } else {
            SSL_set_connect_state(ssl_);
            SSL_set_tlsext_host_name(ssl_, "example.com");
            SSL_set1_host(ssl_, "example.com");
            static const unsigned char kProtos[] = "\x02"
                                                   "h2"
                                                   "\x08"
                                                   "http/1.1";
            SSL_set_alpn_protos(ssl_, kProtos, sizeof(kProtos) - 1);
            if (opt.client_cert) {
                SSL_use_certificate(ssl_, load_cert(certfix::kClientRsaPem));
                SSL_add0_chain_cert(ssl_, load_cert(certfix::kIntermediateRsaPem));
                SSL_use_PrivateKey(ssl_, load_key(certfix::kRsa2048KeyPem));
            }
            if (opt.resume != nullptr) {
                SSL_set_session(ssl_, opt.resume);
            }
        }
    }
    ~Peer() {
        SSL_free(ssl_);
        SSL_CTX_free(ctx_);
    }

    SSL *ssl() const { return ssl_; }

    void ship(const Bytes &bytes) {
        if (!bytes.empty()) {
            BIO_write(rbio_, bytes.data(), static_cast<int>(bytes.size()));
        }
    }

    Bytes drain() {
        Bytes out;
        const int pending = static_cast<int>(BIO_pending(wbio_));
        if (pending > 0) {
            out.resize(static_cast<std::size_t>(pending));
            BIO_read(wbio_, out.data(), pending);
        }
        return out;
    }

    // Drives the handshake; once it completes, a read pulls any
    // post-handshake messages (NewSessionTicket).
    void step() {
        // In the early-data state SSL_do_handshake keeps driving the real
        // handshake (it returns 1 at once, before any server byte, which is
        // when the 0-RTT write happens).
        if (!SSL_is_init_finished(ssl_)) {
            (void) SSL_do_handshake(ssl_);
        }
        if (SSL_in_early_data(ssl_) && !early_written_) {
            static const char kEarly[] = "GET /early HTTP/1.1\r\n\r\n";
            early_written_ = SSL_write(ssl_, kEarly, sizeof(kEarly) - 1) > 0;
            (void) SSL_do_handshake(ssl_);
        }
        if (SSL_is_init_finished(ssl_)) {
            char sink[256];
            (void) SSL_read(ssl_, sink, sizeof(sink));
        }
        ERR_clear_error();
    }

private:
    SSL_CTX *ctx_ = nullptr;
    SSL *ssl_ = nullptr;
    BIO *rbio_ = nullptr;
    BIO *wbio_ = nullptr;
    bool early_written_ = false;
};

// ---- drives ----

const fiber::fuzz::ServerMaterial &server_material() {
    static const fiber::fuzz::ServerMaterial material;
    return material;
}

// BoringSSL client -> our server engine; returns the client's byte stream.
Bytes client_stream_to_our_server(const char *label, const PeerOptions &opt, bool require_client_cert) {
    Bytes stream;
    fiber::fuzz::run_in_loop([&] {
        const auto &material = server_material();
        fiber::tls::TlsServerConfig cfg = material.config();
        if (require_client_cert) {
            material.with_client_auth(cfg);
        }
        fiber::tls::TlsServerHandshakeEngine engine(cfg, &material.lookup, &material.minter);
        Peer client(opt);
        for (int round = 0; round < 16; ++round) {
            client.ship(chain_bytes(engine.take_output()));
            client.step();
            const Bytes flight = client.drain();
            if (flight.empty()) {
                if (engine.done()) {
                    break;
                }
                continue;
            }
            stream.insert(stream.end(), flight.begin(), flight.end());
            if (!engine.done()) {
                (void) engine.feed(fiber::fuzz::to_iobuf(flight));
            }
        }
        client.ship(chain_bytes(engine.take_output()));
        client.step(); // consumes the NST (session callback)
        // Report how far our engine got: seeds are only as good as the paths
        // they reach.
        std::cerr << "server-engine " << label << ": done=" << engine.done() << " failed=" << engine.failed()
                  << " alert=" << (engine.failed() ? static_cast<int>(engine.failure_alert()) : -1)
                  << " client_finished=" << SSL_is_init_finished(client.ssl())
                  << " reused=" << SSL_session_reused(client.ssl())
                  << " early_accepted=" << SSL_early_data_accepted(client.ssl()) << '\n';
    });
    return stream;
}

// BoringSSL server -> our client engine; returns the server's byte stream.
Bytes server_stream_to_our_client(const char *label, const PeerOptions &opt, bool tls12_client) {
    Bytes stream;
    fiber::fuzz::run_in_loop([&] {
        static const fiber::fuzz::ClientMaterial material;
        fiber::tls::TlsClientConfig cfg = material.config();
        if (tls12_client) {
            cfg.max_version = fiber::tls::kTlsVersionTls12;
        }
        fiber::tls::TlsClientHandshakeEngine engine(cfg, nullptr);
        Peer server(opt);
        for (int round = 0; round < 16 && !engine.done(); ++round) {
            server.ship(chain_bytes(engine.take_output()));
            server.step();
            const Bytes flight = server.drain();
            if (flight.empty()) {
                break;
            }
            stream.insert(stream.end(), flight.begin(), flight.end());
            (void) engine.feed(fiber::fuzz::to_iobuf(flight));
        }
        std::cerr << "client-engine " << label << ": done=" << engine.done() << " failed=" << engine.failed()
                  << " alert=" << (engine.failed() ? static_cast<int>(engine.failure_alert()) : -1) << '\n';
    });
    return stream;
}

// Plaintext handshake messages of a stream (records before the first CCS or
// sealed record), as (type, body).
std::vector<std::pair<std::uint8_t, Bytes>> plaintext_messages(const Bytes &stream) {
    std::vector<std::pair<std::uint8_t, Bytes>> out;
    Bytes hs;
    std::size_t off = 0;
    while (off + 5 <= stream.size()) {
        const std::uint8_t type = stream[off];
        const std::size_t len = (static_cast<std::size_t>(stream[off + 3]) << 8) | stream[off + 4];
        if (off + 5 + len > stream.size() || type != 22) {
            break; // CCS / sealed data: the plaintext window is over
        }
        hs.insert(hs.end(), stream.begin() + static_cast<std::ptrdiff_t>(off + 5),
                  stream.begin() + static_cast<std::ptrdiff_t>(off + 5 + len));
        off += 5 + len;
    }
    std::size_t pos = 0;
    while (pos + 4 <= hs.size()) {
        const std::size_t len = (static_cast<std::size_t>(hs[pos + 1]) << 16) |
                                (static_cast<std::size_t>(hs[pos + 2]) << 8) | hs[pos + 3];
        if (pos + 4 + len > hs.size()) {
            break;
        }
        out.emplace_back(hs[pos], Bytes(hs.begin() + static_cast<std::ptrdiff_t>(pos + 4),
                                        hs.begin() + static_cast<std::ptrdiff_t>(pos + 4 + len)));
        pos += 4 + len;
    }
    return out;
}

// Codec selector per handshake type (see tls_handshake_codec_fuzzer.cpp).
int codec_selector(std::uint8_t type, bool tls12) {
    switch (type) {
        case 1:
            return 0; // ClientHello
        case 2:
            return 1; // ServerHello / HRR
        case 11:
            return tls12 ? 4 : 3; // Certificate
        case 12:
            return 9; // ServerKeyExchange
        case 13:
            return tls12 ? 6 : 5; // CertificateRequest
        case 16:
            return 10; // ClientKeyExchange
        default:
            return -1;
    }
}

void codec_seeds_from(const std::string &tag, const Bytes &stream, bool tls12) {
    int n = 0;
    for (const auto &[type, body]: plaintext_messages(stream)) {
        const int selector = codec_selector(type, tls12);
        if (selector >= 0) {
            write_seed("tls_handshake_codec_fuzzer", tag + "-" + std::to_string(n++) + "-t" + std::to_string(type),
                       prefixed(static_cast<std::uint8_t>(selector), body));
        }
    }
}

void encoded_codec_seed(const std::string &name, int selector, fiber::common::IoResult<std::size_t> len,
                        const std::array<std::uint8_t, 4096> &scratch) {
    if (len.has_value() && *len >= 4) {
        write_seed("tls_handshake_codec_fuzzer", name,
                   prefixed(static_cast<std::uint8_t>(selector), Bytes(scratch.begin() + 4, scratch.begin() + *len)));
    }
}

// ---- corpora ----

void server_engine_corpus() {
    struct Case {
        const char *name;
        PeerOptions opt;
        bool require_client_cert;
    };
    const Case cases[] = {
            {"tls13-default", PeerOptions{}, false},
            {"tls12-only", PeerOptions{.tls12_only = true}, false},
            {"tls13-grease", PeerOptions{.grease = true}, false},
            {"tls13-hrr-x25519", PeerOptions{.groups = "P-256:X25519"}, false}, // share P-256, we prefer X25519
            {"tls13-p384", PeerOptions{.groups = "P-384"}, false},
            {"tls13-mtls", PeerOptions{.client_cert = true}, true},
            {"tls12-mtls", PeerOptions{.tls12_only = true, .client_cert = true}, true},
            {"tls12-ecdsa-offer", PeerOptions{.tls12_only = true, .tls12_cipher = "ECDHE-ECDSA-AES128-GCM-SHA256"},
             false},
    };
    for (const Case &c: cases) {
        const Bytes stream = client_stream_to_our_server(c.name, c.opt, c.require_client_cert);
        const std::uint8_t mtls = c.require_client_cert ? 0x40 : 0x00;
        write_seed("tls_server_engine_fuzzer", c.name, prefixed(mtls, stream));
        codec_seeds_from(std::string("client-") + c.name, stream, c.opt.tls12_only);
    }
    // One byte-at-a-time variant: record and handshake reassembly.
    write_seed("tls_server_engine_fuzzer", "tls13-default-bytewise",
               prefixed(0x01, client_stream_to_our_server("bytewise", PeerOptions{}, false)));

    // Resumption: hop 1 collects a ticket our own ticket service minted;
    // hop 2 offers it (1.3: PSK + binder + 0-RTT; 1.2: session ticket).
    for (const bool tls12: {false, true}) {
        (void) client_stream_to_our_server(
                tls12 ? "hop1-1.2" : "hop1-1.3",
                PeerOptions{.tls12_only = tls12, .collect_session = true, .early_data = true}, false);
        if (g_last_session == nullptr) {
            std::cerr << "no session collected (" << (tls12 ? "1.2" : "1.3") << ")\n";
            continue;
        }
        SSL_SESSION *session = g_last_session;
        g_last_session = nullptr;
        const Bytes stream = client_stream_to_our_server(
                tls12 ? "hop2-1.2" : "hop2-1.3",
                PeerOptions{.tls12_only = tls12, .resume = session, .early_data = !tls12}, false);
        SSL_SESSION_free(session);
        write_seed("tls_server_engine_fuzzer", tls12 ? "tls12-resume-ticket" : "tls13-resume-psk-0rtt",
                   prefixed(0x00, stream));
        codec_seeds_from(tls12 ? "client-tls12-resume" : "client-tls13-resume", stream, tls12);
    }
}

void client_engine_corpus() {
    struct Case {
        const char *name;
        PeerOptions opt;
        bool tls12_client;
    };
    const Case cases[] = {
            {"tls13-default", PeerOptions{.server = true}, false},
            {"tls13-hrr-p384", PeerOptions{.server = true, .groups = "P-384"}, false},
            {"tls12-rsa",
             PeerOptions{.server = true, .tls12_only = true, .tls12_cipher = "ECDHE-RSA-AES128-GCM-SHA256"}, false},
            {"tls12-ecdsa-p384",
             PeerOptions{.server = true,
                         .tls12_only = true,
                         .groups = "P-384",
                         .tls12_cipher = "ECDHE-ECDSA-AES128-GCM-SHA256",
                         .leaf_pem = certfix::kLeafEcP384Pem,
                         .key_pem = certfix::kP384KeyPem},
             false},
            // A legacy-only server (feature/tls/11): the CBC suite from the client's tail.
            {"tls12-rsa-cbc", PeerOptions{.server = true, .tls12_only = true, .tls12_cipher = "ECDHE-RSA-AES128-SHA"},
             false},
            // ... and a server without ECDHE: the static-RSA key exchange.
            {"tls12-static-rsa", PeerOptions{.server = true, .tls12_only = true, .tls12_cipher = "AES128-SHA"}, false},
            {"tls12-client-cert-request", PeerOptions{.server = true, .tls12_only = true, .client_cert = true}, false},
            {"tls12-client-only", PeerOptions{.server = true}, true},
    };
    for (const Case &c: cases) {
        const Bytes stream = server_stream_to_our_client(c.name, c.opt, c.tls12_client);
        const std::uint8_t control = c.tls12_client ? 0x40 : 0x00;
        write_seed("tls_client_engine_fuzzer", c.name, prefixed(control, stream));
        codec_seeds_from(std::string("server-") + c.name, stream, c.opt.tls12_only || c.tls12_client);
    }
}

void codec_encoder_corpus() {
    using namespace fiber::tls;
    std::array<std::uint8_t, 4096> scratch{};
    TlsEncryptedExtensionsInput ee;
    ee.acknowledge_server_name = true;
    ee.alpn = "h2";
    ee.early_data = true;
    encoded_codec_seed("enc-ee", 2, tls_encode_encrypted_extensions(ee, scratch), scratch);

    const Bytes der_a(300, 0x30);
    const Bytes der_b(200, 0x31);
    const std::span<const std::uint8_t> certs[] = {der_a, der_b};
    encoded_codec_seed("enc-cert13", 3, tls_encode_certificate_13({}, certs, scratch), scratch);

    const std::uint16_t sigalgs[] = {0x0804, 0x0403, 0x0503, 0x0807, 0x0401};
    encoded_codec_seed("enc-cr13", 5, tls_encode_certificate_request_13(sigalgs, scratch), scratch);
    encoded_codec_seed("enc-cr12", 6, tls_encode_certificate_request_12(sigalgs, scratch), scratch);

    const Bytes sig(256, 0x5A);
    encoded_codec_seed("enc-cv", 7, tls_encode_certificate_verify(0x0804, sig, scratch), scratch);
    const Bytes verify(32, 0x77);
    encoded_codec_seed("enc-fin", 8, tls_encode_finished(verify, scratch), scratch);
}

void connection_corpus() {
    // [config][ctl][len_hi][len_lo][payload]... (tls_connection_fuzzer.cpp)
    const auto record = [](Bytes &out, std::uint8_t ctl, const Bytes &payload) {
        out.push_back(ctl);
        out.push_back(static_cast<std::uint8_t>(payload.size() >> 8));
        out.push_back(static_cast<std::uint8_t>(payload.size()));
        out.insert(out.end(), payload.begin(), payload.end());
    };
    const Bytes app{'h', 'e', 'l', 'l', 'o'};
    const Bytes nst{4, 0, 0, 15, 0, 0, 0x1C, 0x20, 0, 0, 0, 1, 0, 0, 2, 0xAB, 0xCD, 0, 0};
    const Bytes ku_requested{24, 0, 0, 1, 1};
    const Bytes ku_not_requested{24, 0, 0, 1, 0};
    const Bytes warning{1, 90};
    const Bytes close_notify{1, 0};
    const Bytes hello_request{0, 0, 0, 0};
    // Configs 0-3: 1.3/1.2 x client/server (1.2 = AES128-GCM). Then the 1.2
    // CBC record suites (config bits 2-3 = 1..3, feature/tls/11).
    constexpr std::array<std::uint8_t, 10> kConfigs{0, 1, 2, 3, 0x05, 0x07, 0x09, 0x0B, 0x0D, 0x0F};
    constexpr std::array<const char *, 4> kSuiteTags{"", "-aes128-sha", "-aes256-sha", "-aes128-sha256"};
    for (const std::uint8_t config: kConfigs) {
        const bool tls12 = (config & 1) != 0;
        const std::string tag = std::string(tls12 ? "tls12" : "tls13") + ((config & 2) ? "-server" : "-client") +
                                kSuiteTags[(config >> 2) & 3];
        Bytes flow{config};
        record(flow, 0x80 | 3, app); // app data, drain
        if (!tls12) {
            record(flow, 2, nst); // NewSessionTicket
            record(flow, 0x40 | 2, ku_requested); // KeyUpdate(requested), then we write
            record(flow, 0x80 | 3, app); // under the rotated keys
            record(flow, 2, ku_not_requested);
        } else {
            record(flow, 2, hello_request); // refused renegotiation
        }
        record(flow, 1, warning);
        record(flow, 1, close_notify);
        write_seed("tls_connection_fuzzer", tag + "-flow", flow);

        Bytes raw{config};
        record(raw, 0xC0, {23, 3, 3, 0, 20, 1, 2, 3}); // a raw, truncated, unauthentic record
        write_seed("tls_connection_fuzzer", tag + "-raw", raw);

        Bytes big{config};
        record(big, 0x80 | 3, Bytes(16385, 0x41)); // one byte over the plaintext limit
        write_seed("tls_connection_fuzzer", tag + "-overflow", big);
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: tls_fuzz_seedgen <corpus-root>\n";
        return 1;
    }
    g_root = argv[1];
    server_engine_corpus();
    client_engine_corpus();
    codec_encoder_corpus();
    connection_corpus();
    std::cout << "wrote " << g_written << " seeds under " << g_root << '\n';
    return 0;
}
