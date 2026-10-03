// Fuzzes a real in-tree QUIC client and a real in-tree server talking to
// each other, with the fuzzer as the network between them. Both sides run
// their real handshake (TLS 1.3, transport parameters, HANDSHAKE_DONE,
// NEW_TOKEN, NewSessionTicket), so this reaches what single-sided harnesses
// cannot: an authenticated handshake under loss, duplication, reordering
// and corruption; 1-RTT streams in both directions; session resumption and
// 0-RTT (accepted early data, replay-safe streams) on a second connection;
// NAT rebinding of the client; and frames injected under either side's real
// keys at any encryption level, including a key-phase flip.
//
// Datagrams are captured from each side's real build/commit send path into
// two wire queues (client->server, server->client); the fuzzer decides what
// each queue delivers.
//
// Input: [config][op...]
//   config bit 0: server requires Retry; bit 1: server accepts 0-RTT.
//   op: one byte, low nibble = action, high nibble = argument:
//     0 client sends (up to arg+1 datagrams per connection)
//     1 server sends (likewise), plus its stateless replies (Retry, VN,
//       stateless reset), which leave through its real socket
//     2 client->server wire, 3 server->client wire: [index] then arg
//       chooses deliver / drop / duplicate / corrupt [pos] / truncate /
//       deliver all / deliver all reversed
//     4 inject client->server, 5 inject server->client:
//       [len][frames]; arg & 3 = encryption level, arg & 4 = next key phase
//     6 client app: open a stream (arg & 1 uni) and write (arg >> 1) * 700
//       bytes with FIN; 0-RTT-capable while early keys exist
//     7 server app: read every stream, answer peer bidi streams, open a uni
//     8 client app: read every stream
//     9 advance time by arg * 50 ms: loss detection + PTO on both sides
//     10 client close / shutdown, 11 server close / shutdown (arg picks)
//     12 new client connection (max 3): arg & 1 resume the stored session
//        with 0-RTT, arg & 2 present the stored NEW_TOKEN token
//     13 client NAT rebinding: later client datagrams arrive from port
//        5000 + arg

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string_view>
#include <vector>

#include "../tests/TlsCertFixtures.h"
#include "QuicFuzzCommon.h"

#include <fiber/net/TlsCredential.h>
#include <fiber/net/TlsParams.h>
#include <fiber/net/TlsServerHandshakeConfig.h>
#include <fiber/quic/QuicStream.h>
#include <fiber/tls/TlsConfig.h>
#include "quic/QuicCrypto.h"
#include "quic/QuicLossRecovery.h"

namespace {

using namespace fiber::quic;
using fiber::fuzz::FuzzInput;
using Access = QuicUdpEndpointTestAccess;
using Bytes = std::vector<std::uint8_t>;

constexpr std::string_view kAlpn[] = {"h3"};
constexpr std::size_t kMaxServerConnections = 16;
constexpr std::size_t kMaxClients = 3;
constexpr std::size_t kMaxWire = 48;

void destroy_stream(void *, QuicStream &stream) noexcept { delete &stream; }

QuicStream::Lease create_stream(void *, std::uint64_t) noexcept {
    return QuicStream::Lease::adopt(new (std::nothrow) QuicStream(nullptr, destroy_stream));
}

void destroy_connection(void *, QuicConnection &connection) noexcept { delete &connection; }

struct ServerTracker {
    std::array<QuicConnection::Lease, kMaxServerConnections> leases{};
    std::size_t count = 0;
};

QuicConnection::Lease create_server_connection(void *owner, QuicUdpEndpoint &endpoint,
                                               const QuicConnection::Options &options) noexcept {
    auto *tracker = static_cast<ServerTracker *>(owner);
    if (tracker->count == kMaxServerConnections) {
        return {};
    }
    QuicConnection::Options opts = options;
    opts.on_destroy = destroy_connection;
    opts.ops.create_stream = create_stream;
    auto *connection = new (std::nothrow) QuicConnection(endpoint, opts);
    if (connection == nullptr) {
        return {};
    }
    tracker->leases[tracker->count++] = connection->lease();
    return QuicConnection::Lease::adopt(connection);
}

struct ServerTls {
    fiber::net::TlsCredential credential;
    fiber::net::TlsServerParam param{};

    explicit ServerTls(bool early_data) {
        fiber::net::TlsCredentialOptions options{};
        options.certificate_chain = fiber::net::TlsPemSource::from_content(fiber::tls::certfix::kLeafEcP256Pem);
        options.private_key = fiber::net::TlsPemSource::from_content(fiber::tls::certfix::kP256KeyPem);
        auto created = fiber::net::TlsCredential::create(options);
        FIBER_ASSERT(created.has_value());
        credential = std::move(*created);
        param.configure_callback = &fiber::net::configure_tls_with_credential;
        param.configure_ctx = &credential;
        param.min_version = 0x0304;
        param.max_version = 0x0304;
        param.alpn = kAlpn;
        param.enable_early_data = early_data;
    }
};

struct Server {
    ServerTls tls;
    ServerTracker tracker{};
    QuicUdpEndpoint endpoint;

    Server(bool retry, bool early_data) : tls(early_data), endpoint(fiber::fuzz::quic_loop()) {
        QuicUdpEndpoint::EndpointOptions endpoint_options{};
        endpoint_options.bind_addr = fiber::fuzz::loopback(0);
        endpoint_options.max_connections = kMaxServerConnections;
        endpoint_options.stateless_reset_secret_set = true;
        endpoint_options.stateless_reset_secret.fill(0x6b);
        QuicUdpEndpoint::ServerAdmissionOptions admission{};
        admission.tls = &tls.param;
        admission.retry = retry;
        admission.issue_new_token = true;
        admission.address_validation_key_set = true;
        admission.address_validation_key = fiber::fuzz::kTokenKey;
        admission.stateless_response_limits = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
        admission.connection_owner = &tracker;
        admission.create_connection = create_server_connection;
        auto ok = endpoint.init(endpoint_options, admission);
        FIBER_ASSERT(ok.has_value());
    }
};

Server &server(std::uint8_t variant) {
    static Server *servers[4] = {};
    if (servers[variant] == nullptr) {
        servers[variant] = new Server((variant & 1) != 0, (variant & 2) != 0);
    }
    return *servers[variant];
}

QuicUdpEndpoint &client_endpoint() {
    static QuicUdpEndpoint *endpoint = [] {
        auto *ep = new QuicUdpEndpoint(fiber::fuzz::quic_loop());
        QuicUdpEndpoint::EndpointOptions options{};
        options.bind_addr = fiber::fuzz::loopback(0);
        auto ok = ep->init(options);
        FIBER_ASSERT(ok.has_value());
        return ep;
    }();
    return *endpoint;
}

// What the client remembers between its connections within one input.
struct ClientCache {
    fiber::tls::TlsSessionState session{};
    QuicTransportSettings remembered{};
    Bytes token{};
};

bool store_session(void *owner, QuicConnection &connection, fiber::tls::TlsSessionState &&session) noexcept {
    auto *cache = static_cast<ClientCache *>(owner);
    cache->session = std::move(session);
    cache->remembered = connection.peer_transport().params;
    return true;
}

void store_token(void *owner, QuicConnection &, const std::uint8_t *token, std::size_t len) noexcept {
    auto *cache = static_cast<ClientCache *>(owner);
    cache->token.assign(token, token + len);
}

struct Net {
    Server &srv;
    QuicUdpEndpoint &cli;
    ClientCache cache{};
    std::array<QuicConnection::Lease, kMaxClients> clients{};
    std::size_t client_count = 0;
    std::vector<Bytes> c2s{};
    std::vector<Bytes> s2c{};
    fiber::net::SocketAddress client_from;
    std::chrono::steady_clock::time_point now;
    std::uint32_t pto_count = 0;

    explicit Net(Server &server) :
        srv(server), cli(client_endpoint()), client_from(cli.local_addr()), now(fiber::fuzz::quic_loop().now()) {
        // Leftovers from an earlier input's teardown must not leak into this one.
        Access::drain_socket(cli, [](const std::uint8_t *, std::size_t) {});
    }

    void connect(bool resume, bool with_token) {
        if (client_count == kMaxClients) {
            return;
        }
        auto identity = cli.allocate_client_identity();
        if (!identity) {
            return;
        }
        QuicConnection::Options options{};
        options.role = QuicConnectionRole::Client;
        options.local_addr = cli.local_addr();
        options.remote_addr = srv.endpoint.local_addr();
        options.original_destination_connection_id = identity->original_destination_connection_id;
        options.initial_destination_connection_id = identity->original_destination_connection_id;
        options.remote_connection_id = identity->original_destination_connection_id;
        options.local_connection_id = identity->local_connection_id;
        options.on_destroy = destroy_connection;
        options.owner = &cache;
        options.ops.create_stream = create_stream;
        options.ops.on_new_tls_session = store_session;
        options.ops.on_new_token = store_token;
        auto *raw = new (std::nothrow) QuicConnection(cli, options);
        if (raw == nullptr) {
            return;
        }
        QuicConnection::Lease lease = QuicConnection::Lease::adopt(raw);
        QuicClientConnectParams params{};
        params.tls.min_version = 0x0304;
        params.tls.max_version = 0x0304;
        params.tls.alpn = kAlpn;
        params.tls.server_name = "example.com";
        params.allow_insecure = true;
        if (resume && !cache.session.empty()) {
            params.resumption_session = &cache.session;
            params.enable_early_data = true;
            params.remembered_peer_transport = &cache.remembered;
        }
        if (with_token && !cache.token.empty()) {
            params.token = cache.token.data();
            params.token_len = cache.token.size();
        }
        (void) lease->connect(params);
        clients[client_count++] = std::move(lease);
    }

    void send(QuicUdpEndpoint &endpoint, QuicConnection &conn, std::size_t max, std::vector<Bytes> &wire) {
        (void) Access::flush(endpoint, conn, max, [&](const QuicSendDatagram &datagram) {
            if (wire.size() < kMaxWire) {
                wire.emplace_back(datagram.data, datagram.data + datagram.length);
            }
        });
    }

    void deliver(Bytes &bytes, bool to_server) {
        if (bytes.empty()) {
            return;
        }
        if (to_server) {
            (void) Access::receive(srv.endpoint, bytes.data(), bytes.size(), client_from, srv.endpoint.local_addr(),
                                   now);
        } else {
            (void) Access::receive(cli, bytes.data(), bytes.size(), srv.endpoint.local_addr(), cli.local_addr(), now);
        }
    }

    void wire_op(std::uint8_t arg, std::uint8_t index_byte, std::uint8_t pos_byte, bool to_server) {
        std::vector<Bytes> &wire = to_server ? c2s : s2c;
        if (wire.empty()) {
            return;
        }
        const std::size_t index = index_byte % wire.size();
        switch (arg & 0x07) {
            case 1: // drop
                wire.erase(wire.begin() + static_cast<std::ptrdiff_t>(index));
                return;
            case 2: { // duplicate: deliver a copy, keep the original queued
                Bytes copy = wire[index];
                deliver(copy, to_server);
                return;
            }
            case 3: { // corrupt one byte
                Bytes bytes = std::move(wire[index]);
                wire.erase(wire.begin() + static_cast<std::ptrdiff_t>(index));
                bytes[pos_byte % bytes.size()] ^= static_cast<std::uint8_t>(1U << (arg >> 1 & 7));
                deliver(bytes, to_server);
                return;
            }
            case 4: { // truncate
                Bytes bytes = std::move(wire[index]);
                wire.erase(wire.begin() + static_cast<std::ptrdiff_t>(index));
                bytes.resize(bytes.size() / 2);
                deliver(bytes, to_server);
                return;
            }
            case 5:
            case 6: { // everything, in order or reversed
                std::vector<Bytes> all = std::move(wire);
                wire.clear();
                if ((arg & 0x07) == 6) {
                    std::reverse(all.begin(), all.end());
                }
                for (Bytes &bytes: all) {
                    deliver(bytes, to_server);
                }
                return;
            }
            default: {
                Bytes bytes = std::move(wire[index]);
                wire.erase(wire.begin() + static_cast<std::ptrdiff_t>(index));
                deliver(bytes, to_server);
                return;
            }
        }
    }

    // Seals `frames` as if from the other side, under the receiver's own
    // read keys, and delivers it.
    void inject(QuicConnection &receiver, std::uint8_t arg, std::span<const std::uint8_t> frames, bool to_server) {
        static std::array<std::uint8_t, kQuicMaxUdpPayloadSize> datagram{};
        const auto level = static_cast<QuicEncryptionLevel>(arg & 0x03);
        const QuicPacketNumberSpace &space = receiver.packet_number_space(level);
        const std::uint64_t pn = space.largest_received_packet_number == kUnsetPacketNumber
                                         ? 0
                                         : space.largest_received_packet_number + 1;
        const QuicConnectionId &dcid = receiver.local_connection_id();
        const QuicConnectionId &scid = receiver.remote_connection_id();
        std::size_t len = 0;
        switch (level) {
            case QuicEncryptionLevel::Initial: {
                QuicCryptoState keys{};
                const QuicConnectionId &initial = to_server ? receiver.initial_destination_connection_id()
                                                            : receiver.current_initial_destination_connection_id();
                if (quic_init_initial_crypto(keys, to_server ? QuicConnectionRole::Client : QuicConnectionRole::Server,
                                             initial)) {
                    len = fiber::fuzz::seal_long_packet(QuicPacketType::Initial, kQuicVersion1,
                                                        to_server ? initial : dcid, scid, {}, keys.initial_write(), pn,
                                                        frames, to_server ? kMinInitialDatagramSize : 0,
                                                        datagram.data(), 0, datagram.size());
                }
                break;
            }
            case QuicEncryptionLevel::EarlyData:
                len = fiber::fuzz::seal_long_packet(
                        QuicPacketType::ZeroRtt, kQuicVersion1, receiver.initial_destination_connection_id(), scid, {},
                        receiver.crypto().early_read(), pn, frames, 0, datagram.data(), 0, datagram.size());
                break;
            case QuicEncryptionLevel::Handshake:
                len = fiber::fuzz::seal_long_packet(QuicPacketType::Handshake, kQuicVersion1, dcid, scid, {},
                                                    receiver.crypto().handshake_read(), pn, frames, 0, datagram.data(),
                                                    0, datagram.size());
                break;
            case QuicEncryptionLevel::Application: {
                const bool next = (arg & 0x04) != 0;
                const bool phase = receiver.key_phase() != next;
                QuicPacketHeader packet{};
                packet.type = QuicPacketType::Short;
                packet.level = QuicEncryptionLevel::Application;
                packet.flags = static_cast<std::uint8_t>(kPacketFlagFixed | (phase ? kPacketFlagKeyPhase : 0));
                packet.dcid = dcid;
                packet.pn_len = 4;
                packet.packet_number = pn;
                const auto keys =
                        next ? receiver.crypto().next_application_read() : receiver.crypto().application_read();
                if (keys.ready()) {
                    len = fiber::fuzz::seal_packet(packet, keys, frames, datagram.data(), datagram.size());
                }
                break;
            }
        }
        if (len != 0) {
            Bytes bytes(datagram.data(), datagram.data() + len);
            deliver(bytes, to_server);
        }
    }

    QuicConnection *newest_client() { return client_count == 0 ? nullptr : clients[client_count - 1].get(); }
    QuicConnection *newest_server() {
        return srv.tracker.count == 0 ? nullptr : srv.tracker.leases[srv.tracker.count - 1].get();
    }

    template<typename F>
    void each_client(F &&f) {
        for (std::size_t i = 0; i < client_count; ++i) {
            if (!clients[i]->closed()) {
                f(*clients[i]);
            }
        }
    }

    template<typename F>
    void each_server(F &&f) {
        for (std::size_t i = 0; i < srv.tracker.count; ++i) {
            if (!srv.tracker.leases[i]->closed()) {
                f(*srv.tracker.leases[i]);
            }
        }
    }

    static void read_all(QuicConnection &conn) {
        for (std::uint64_t id = 0; id < 32; ++id) {
            if (QuicStream *stream = conn.find_stream(id)) {
                fiber::mem::IoBufChain out;
                (void) stream->try_read(1 << 20, out);
            }
        }
    }

    static void open_and_write(QuicConnection &conn, bool uni, std::size_t len, QuicStreamEarlyDataMode mode) {
        static const std::uint8_t kData[16 * 1024] = {};
        auto stream = conn.try_attach_local_stream(
                QuicStream::Lease::adopt(new (std::nothrow) QuicStream(nullptr, destroy_stream)),
                uni ? QuicStreamType::Unidirectional : QuicStreamType::Bidirectional, mode);
        if (stream && *stream != nullptr) {
            (void) (*stream)->try_write(kData, std::min(len, sizeof(kData)), true);
        }
    }

    void timers(std::uint8_t arg) {
        now += std::chrono::milliseconds(static_cast<int>(arg) * 50 + 1);
        pto_count = std::min<std::uint32_t>(pto_count + 1, 10);
        const QuicTime t = quic_time_ms(now);
        auto run = [&](QuicConnection &conn) {
            (void) quic_detect_lost(conn, t, nullptr);
            (void) quic_queue_pto_probe_frames(conn, t, pto_count);
        };
        each_client(run);
        each_server(run);
    }

    fiber::async::Task<void> teardown() {
        for (std::size_t i = 0; i < client_count; ++i) {
            if (!clients[i]->closed()) {
                clients[i]->close_immediately();
            }
            co_await clients[i]->wait_closed();
            clients[i].reset();
        }
        client_count = 0;
        for (std::size_t i = 0; i < srv.tracker.count; ++i) {
            if (!srv.tracker.leases[i]->closed()) {
                srv.tracker.leases[i]->close_immediately();
            }
            co_await srv.tracker.leases[i]->wait_closed();
            srv.tracker.leases[i].reset();
        }
        srv.tracker.count = 0;
        FIBER_FUZZ_CHECK(cli.active_connection_count() == 0);
        FIBER_FUZZ_CHECK(srv.endpoint.active_connection_count() == 0);
        FIBER_FUZZ_CHECK(cli.retained_recv_storage_capacity() == 0);
        FIBER_FUZZ_CHECK(srv.endpoint.retained_recv_storage_capacity() == 0);
    }
};

fiber::async::Task<void> run(const std::uint8_t *data, std::size_t size) {
    FuzzInput in(data, size);
    Net net(server(in.u8() & 0x03));
    net.connect(false, false);

    for (int ops = 0; !in.empty() && ops < 256; ++ops) {
        const std::uint8_t op = in.u8();
        const std::uint8_t arg = op >> 4;
        FIBER_FUZZ_TRACE("op %x arg %x  c2s=%zu s2c=%zu\n", op & 0x0f, arg, net.c2s.size(), net.s2c.size());
        switch (op & 0x0f) {
            case 0:
                net.each_client([&](QuicConnection &c) { net.send(net.cli, c, arg + 1U, net.c2s); });
                break;
            case 1:
                net.each_server([&](QuicConnection &c) { net.send(net.srv.endpoint, c, arg + 1U, net.s2c); });
                // Stateless replies (Retry, VN, reset) left through the
                // server's socket; they wait in the client's.
                Access::drain_socket(net.cli, [&](const std::uint8_t *bytes, std::size_t len) {
                    if (net.s2c.size() < kMaxWire) {
                        net.s2c.emplace_back(bytes, bytes + len);
                    }
                });
                break;
            case 2:
            case 3: {
                const std::uint8_t index = in.u8();
                const std::uint8_t pos = in.u8();
                net.wire_op(arg, index, pos, (op & 0x0f) == 2);
                break;
            }
            case 4:
            case 5: {
                const std::span<const std::uint8_t> frames = in.bytes(in.u8());
                QuicConnection *receiver = (op & 0x0f) == 4 ? net.newest_server() : net.newest_client();
                if (receiver != nullptr && !receiver->closed()) {
                    net.inject(*receiver, arg, frames, (op & 0x0f) == 4);
                }
                break;
            }
            case 6:
                if (QuicConnection *c = net.newest_client(); c != nullptr && !c->closed()) {
                    const auto mode = c->crypto().early_write_ready() ? QuicStreamEarlyDataMode::ReplaySafe
                                                                      : QuicStreamEarlyDataMode::OneRttOnly;
                    Net::open_and_write(*c, (arg & 1) != 0, static_cast<std::size_t>(arg >> 1) * 700, mode);
                }
                break;
            case 7:
                net.each_server([&](QuicConnection &c) {
                    Net::read_all(c);
                    for (std::uint64_t id = 0; id < 32; id += 4) {
                        if (QuicStream *stream = c.find_stream(id)) {
                            static const std::uint8_t kReply[512] = {};
                            (void) stream->try_write(kReply, static_cast<std::size_t>(arg) * 32, true);
                        }
                    }
                    if ((arg & 1) != 0) {
                        Net::open_and_write(c, true, 300, QuicStreamEarlyDataMode::OneRttOnly);
                    }
                });
                break;
            case 8:
                net.each_client([](QuicConnection &c) { Net::read_all(c); });
                break;
            case 9:
                net.timers(arg);
                break;
            case 10:
                if (QuicConnection *c = net.newest_client(); c != nullptr && !c->terminal_closing()) {
                    if ((arg & 3) == 0) {
                        c->close(QuicErrorCode::NoError);
                    } else if ((arg & 3) == 1) {
                        c->close_application(arg);
                    } else {
                        c->shutdown_application(arg);
                    }
                }
                break;
            case 11:
                if (QuicConnection *c = net.newest_server(); c != nullptr && !c->terminal_closing()) {
                    if ((arg & 3) == 0) {
                        c->close(QuicErrorCode::NoError);
                    } else if ((arg & 3) == 1) {
                        c->close_application(arg);
                    } else {
                        c->shutdown(QuicErrorCode::NoError);
                    }
                }
                break;
            case 12:
                net.connect((arg & 1) != 0, (arg & 2) != 0);
                break;
            case 13:
                net.client_from = fiber::fuzz::loopback(static_cast<std::uint16_t>(5000 + arg));
                break;
            default:
                break;
        }
    }
    if (fiber::fuzz::trace_enabled()) {
        net.each_client([](QuicConnection &c) {
            FIBER_FUZZ_TRACE("client state=%d confirmed=%d early_accepted=%d streams=%zu\n",
                             static_cast<int>(c.state()), static_cast<int>(c.handshake_confirmed()),
                             static_cast<int>(c.early_data_accepted()), c.active_stream_count());
        });
        net.each_server([](QuicConnection &c) {
            FIBER_FUZZ_TRACE("server state=%d confirmed=%d early_accepted=%d streams=%zu\n",
                             static_cast<int>(c.state()), static_cast<int>(c.handshake_confirmed()),
                             static_cast<int>(c.early_data_accepted()), c.active_stream_count());
        });
    }
    co_await net.teardown();
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size < 1) {
        return 0;
    }
    fiber::fuzz::run_in_quic_loop_task([&] { return run(data, size); });
    return 0;
}
