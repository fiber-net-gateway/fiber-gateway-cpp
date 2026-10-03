// Fuzzes a QUIC server endpoint from the first datagram on: DCID routing,
// Version Negotiation, Retry and address-token validation, INVALID_TOKEN
// closes, stateless resets, connection admission, Initial header protection
// and decryption, coalesced packets, and -- through CRYPTO frames -- the
// server's TLS 1.3 handshake with the QUIC transport-parameter extension.
//
// The harness plays the client. Initial packets are sealed under the keys
// RFC 9001 5.2 derives from the DCID, so fuzzer frames get past AEAD; once a
// connection exists, Handshake and 1-RTT packets are sealed with the read
// keys the server itself installed (the harness shares its process), which
// reaches Handshake-level CRYPTO/ACK processing without a real client.
// Responses go through the real send scheduler and socket to loopback ports
// nobody listens on.
//
// Input: [config][record...]
//   config bits 0-1: endpoint variant (bit 0 Retry required, bit 1 NEW_TOKEN
//   issued after validation).
//   record: [kind][flags][len:u16][payload]
//     kind & 0x07: 0 Initial, 1 Handshake, 2 1-RTT, 3 Initial + Handshake
//       coalesced (payload split in half), 4 raw datagram, 5 Initial with an
//       unsupported version, 6 short header to an unknown DCID, 7 Initial
//       carrying a 0-RTT packet behind it
//     kind bits 3-4: Initial DCID: 0 8-byte A, 1 20-byte B, 2 4-byte C (too
//       short), 3 the newest connection's server CID
//     kind bits 5-7: Initial token: 0 none, 1 valid Retry token, 2 valid
//       NEW_TOKEN token, 3 Retry token for another port, 4 expired Retry
//       token, 5 garbage, 6-7 none
//     flags bits 0-1: client address (0/3 default, 1 new port, 2 new IP);
//       bit 2: do not pad the datagram to 1200 bytes; bit 3: repeat the last
//       packet number of the level; bit 4: skip the send pump afterwards;
//       bits 5-6: which created connection Handshake/1-RTT records target

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <string_view>

#include "../tests/QuicTestTlsCertificate.h"
#include "QuicFuzzCommon.h"

#include <fiber/net/TlsCredential.h>
#include <fiber/net/TlsParams.h>
#include <fiber/net/TlsServerHandshakeConfig.h>
#include <fiber/quic/QuicPacketProcessor.h>
#include <fiber/quic/QuicSendScheduler.h>
#include <fiber/quic/QuicToken.h>
#include "quic/QuicCrypto.h"

namespace {

using namespace fiber::quic;
using fiber::fuzz::FuzzInput;
using Access = QuicUdpEndpointTestAccess;

constexpr std::string_view kAlpn[] = {"h3"};
// At most one admission per record (and at most 64 records), so the tracker
// sees every connection an input creates.
constexpr std::size_t kMaxRecords = 64;
constexpr std::size_t kMaxTracked = kMaxRecords;

using fiber::fuzz::kTokenKey;

// Connections the endpoint admitted during the current input. The endpoint
// holds its own lease while they are attached; ours keeps a connection
// alive (for its keys) until the input ends.
struct Tracker {
    std::array<QuicConnection::Lease, kMaxTracked> leases{};
    std::size_t count = 0;
};

void destroy_connection(void *, QuicConnection &connection) noexcept { delete &connection; }

QuicConnection::Lease create_connection(void *owner, QuicUdpEndpoint &endpoint,
                                        const QuicConnection::Options &options) noexcept {
    QuicConnection::Options opts = options;
    opts.destroy_owner = nullptr;
    opts.on_destroy = destroy_connection;
    auto *connection = new (std::nothrow) QuicConnection(endpoint, opts);
    if (connection == nullptr) {
        return {};
    }
    auto lease = QuicConnection::Lease::adopt(connection);
    auto *tracker = static_cast<Tracker *>(owner);
    if (tracker->count < kMaxTracked) {
        tracker->leases[tracker->count++] = connection->lease();
    }
    return lease;
}

struct ServerTls {
    fiber::net::TlsCredential credential;
    fiber::net::TlsServerParam param{};

    ServerTls() {
        fiber::net::TlsCredentialOptions options{};
        options.certificate_chain = fiber::net::TlsPemSource::from_content(fiber::test::kQuicTestCertificatePem);
        options.private_key = fiber::net::TlsPemSource::from_content(fiber::test::kQuicTestPrivateKeyPem);
        auto created = fiber::net::TlsCredential::create(options);
        FIBER_ASSERT(created.has_value());
        credential = std::move(*created);
        param.configure_callback = &fiber::net::configure_tls_with_credential;
        param.configure_ctx = &credential;
        param.min_version = 0x0304;
        param.max_version = 0x0304;
        param.alpn = kAlpn;
    }
};

// One persistent endpoint per variant: binding a socket per input would
// dominate the run time, and every connection is torn down between inputs.
struct Server {
    Tracker tracker{};
    QuicUdpEndpoint endpoint;

    Server(const ServerTls &tls, bool retry, bool new_token) : endpoint(fiber::fuzz::quic_loop()) {
        QuicUdpEndpoint::EndpointOptions endpoint_options{};
        endpoint_options.bind_addr = fiber::fuzz::loopback(0);
        endpoint_options.max_connections = 16;
        endpoint_options.stateless_reset_secret_set = true;
        endpoint_options.stateless_reset_secret.fill(0x5e);
        QuicUdpEndpoint::ServerAdmissionOptions admission{};
        admission.tls = &tls.param;
        admission.retry = retry;
        admission.issue_new_token = new_token;
        admission.address_validation_key_set = true;
        admission.address_validation_key = kTokenKey;
        // Rate limits are time based; disabled so an input replays the same.
        admission.stateless_response_limits = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
        admission.connection_owner = &tracker;
        admission.create_connection = create_connection;
        auto ok = endpoint.init(endpoint_options, admission);
        FIBER_ASSERT(ok.has_value());
    }

    // An immediate close detaches on the loop's next turn; wait for it so
    // the next input starts from an empty endpoint.
    fiber::async::Task<void> teardown() {
        for (std::size_t i = 0; i < tracker.count; ++i) {
            QuicConnection &connection = *tracker.leases[i];
            if (!connection.terminal_closing()) {
                connection.close_immediately();
            } else if (!connection.closed()) {
                // Closing/Draining wait out 3*PTO; cut that short.
                connection.close_immediately();
            }
            co_await connection.wait_closed();
            FIBER_FUZZ_TRACE("  teardown conn %zu state=%d refs=%u\n", i, static_cast<int>(connection.state()),
                             connection.ref_count());
            tracker.leases[i].reset();
        }
        tracker.count = 0;
        FIBER_FUZZ_TRACE("  teardown: active=%zu retained=%zu\n", endpoint.active_connection_count(),
                         endpoint.retained_recv_storage_capacity());
        FIBER_FUZZ_CHECK(endpoint.active_connection_count() == 0);
        FIBER_FUZZ_CHECK(endpoint.retained_recv_storage_capacity() == 0);
    }
};

Server &server(std::uint8_t variant) {
    static ServerTls tls;
    static Server *servers[4] = {};
    if (servers[variant] == nullptr) {
        servers[variant] = new Server(tls, (variant & 1) != 0, (variant & 2) != 0);
    }
    return *servers[variant];
}

struct Client {
    QuicConnectionId scid = fiber::fuzz::client_scid();
    std::array<std::uint64_t, 4> next_pn{};
    std::array<std::uint64_t, 4> last_pn{};
    std::chrono::steady_clock::time_point now{};

    std::uint64_t pick_pn(QuicEncryptionLevel level, bool repeat) {
        const auto index = static_cast<std::size_t>(level);
        if (repeat) {
            return last_pn[index];
        }
        last_pn[index] = next_pn[index]++;
        return last_pn[index];
    }
};

fiber::net::SocketAddress client_address(std::uint8_t flags) {
    switch (flags & 0x03) {
        case 1:
            return fiber::fuzz::loopback(4434);
        case 2:
            return {fiber::net::IpAddress::v4({127, 0, 0, 2}), 4433};
        default:
            return fiber::fuzz::loopback(4433);
    }
}

QuicConnection *target(Server &srv, std::uint8_t flags) {
    if (srv.tracker.count == 0) {
        return nullptr;
    }
    // The n-th newest connection, so records keep targeting live ones.
    const std::size_t back = std::min<std::size_t>((flags >> 5) & 0x03, srv.tracker.count - 1);
    return srv.tracker.leases[srv.tracker.count - 1 - back].get();
}

QuicConnectionId initial_dcid(Server &srv, std::uint8_t kind) {
    switch ((kind >> 3) & 0x03) {
        case 1:
            return fiber::fuzz::make_cid(0xB0, 20);
        case 2:
            return fiber::fuzz::make_cid(0xD0, 4);
        case 3:
            if (srv.tracker.count != 0) {
                return srv.tracker.leases[srv.tracker.count - 1]->local_connection_id();
            }
            [[fallthrough]];
        default:
            return fiber::fuzz::make_cid(0xA0, 8);
    }
}

// Token bytes for the Initial; `storage` backs the returned slice.
QuicSlice initial_token(std::uint8_t kind, const fiber::net::SocketAddress &peer, QuicAddressToken &storage) {
    static constexpr std::uint8_t kGarbage[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
                                                0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14,
                                                0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e};
    const QuicConnectionId odcid = fiber::fuzz::make_cid(0xE0, 8);
    const std::uint64_t now = quic_unix_seconds_now();
    fiber::common::IoResult<QuicAddressToken> minted = std::unexpected(fiber::common::IoErr::Invalid);
    switch ((kind >> 5) & 0x07) {
        case 1:
            minted = quic_create_address_token(kTokenKey, peer, now + 60, QuicAddressTokenKind::Retry, &odcid);
            break;
        case 2:
            minted = quic_create_address_token(kTokenKey, peer, now + 600, QuicAddressTokenKind::NewToken);
            break;
        case 3:
            minted = quic_create_address_token(kTokenKey, {peer.ip(), static_cast<std::uint16_t>(peer.port() + 7)},
                                               now + 60, QuicAddressTokenKind::Retry, &odcid);
            break;
        case 4:
            minted = quic_create_address_token(kTokenKey, peer, now - 60, QuicAddressTokenKind::Retry, &odcid);
            break;
        case 5:
            return {kGarbage, sizeof(kGarbage)};
        default:
            return {};
    }
    if (!minted) {
        return {};
    }
    storage = *minted;
    return storage.slice();
}

fiber::async::Task<void> run(const std::uint8_t *data, std::size_t size) {
    FuzzInput in(data, size);
    Server &srv = server(in.u8() & 0x03);
    Client client{};
    client.now = fiber::fuzz::quic_loop().now();
    static std::array<std::uint8_t, kQuicMaxUdpPayloadSize> datagram{};

    for (std::size_t records = 0; !in.empty() && records < kMaxRecords; ++records) {
        const std::uint8_t kind = in.u8();
        const std::uint8_t flags = in.u8();
        const std::span<const std::uint8_t> payload = in.bytes(in.u16());
        FIBER_FUZZ_TRACE("record kind=0x%02x flags=0x%02x len=%zu\n", kind, flags, payload.size());
        const fiber::net::SocketAddress peer = client_address(flags);
        const bool repeat = (flags & 0x08) != 0;
        const std::size_t pad_to = (flags & 0x04) != 0 ? 0 : kMinInitialDatagramSize;
        client.now += std::chrono::milliseconds(1);

        std::size_t len = 0;
        switch (kind & 0x07) {
            case 0:
            case 3:
            case 5:
            case 7: {
                const QuicConnectionId dcid = initial_dcid(srv, kind);
                QuicCryptoState keys{};
                if (!quic_init_initial_crypto(keys, QuicConnectionRole::Client, dcid)) {
                    break;
                }
                QuicAddressToken token_storage{};
                const QuicSlice token = initial_token(kind, peer, token_storage);
                const std::uint32_t version = (kind & 0x07) == 5 ? 0x1a2a3a4a : kQuicVersion1;
                std::span<const std::uint8_t> first = payload;
                std::span<const std::uint8_t> second{};
                if ((kind & 0x07) == 3 || (kind & 0x07) == 7) {
                    first = payload.first(payload.size() / 2);
                    second = payload.subspan(payload.size() / 2);
                }
                const bool coalesced = !second.empty() || (kind & 0x07) == 3 || (kind & 0x07) == 7;
                len = fiber::fuzz::seal_long_packet(QuicPacketType::Initial, version, dcid, client.scid, token,
                                                    keys.initial_write(),
                                                    client.pick_pn(QuicEncryptionLevel::Initial, repeat), first,
                                                    coalesced ? 0 : pad_to, datagram.data(), 0, datagram.size());
                if (len == 0 || !coalesced) {
                    break;
                }
                QuicConnection *conn = target(srv, flags);
                if ((kind & 0x07) == 3 && conn != nullptr) {
                    len += fiber::fuzz::seal_long_packet(
                            QuicPacketType::Handshake, kQuicVersion1, conn->local_connection_id(), client.scid, {},
                            conn->crypto().handshake_read(), client.pick_pn(QuicEncryptionLevel::Handshake, repeat),
                            second, pad_to, datagram.data(), len, datagram.size());
                } else if ((kind & 0x07) == 7) {
                    // 0-RTT keys only exist on a resumed session; sealing
                    // with Initial keys exercises the early-key-absent path.
                    len += fiber::fuzz::seal_long_packet(QuicPacketType::ZeroRtt, kQuicVersion1, dcid, client.scid, {},
                                                         keys.initial_write(),
                                                         client.pick_pn(QuicEncryptionLevel::EarlyData, repeat), second,
                                                         pad_to, datagram.data(), len, datagram.size());
                }
                break;
            }
            case 1:
                if (QuicConnection *conn = target(srv, flags)) {
                    len = fiber::fuzz::seal_long_packet(
                            QuicPacketType::Handshake, kQuicVersion1, conn->local_connection_id(), client.scid, {},
                            conn->crypto().handshake_read(), client.pick_pn(QuicEncryptionLevel::Handshake, repeat),
                            payload, 0, datagram.data(), 0, datagram.size());
                }
                break;
            case 2:
                if (QuicConnection *conn = target(srv, flags)) {
                    QuicPacketHeader packet{};
                    packet.type = QuicPacketType::Short;
                    packet.level = QuicEncryptionLevel::Application;
                    packet.flags =
                            static_cast<std::uint8_t>(kPacketFlagFixed | (conn->key_phase() ? kPacketFlagKeyPhase : 0));
                    packet.dcid = conn->local_connection_id();
                    packet.pn_len = 4;
                    packet.packet_number = client.pick_pn(QuicEncryptionLevel::Application, repeat);
                    const auto keys = conn->crypto().application_read();
                    if (keys.ready()) {
                        len = fiber::fuzz::seal_packet(packet, keys, payload, datagram.data(), datagram.size());
                    }
                }
                break;
            case 4:
                len = std::min(payload.size(), datagram.size());
                std::memcpy(datagram.data(), payload.data(), len);
                break;
            default: {
                // Short header to a DCID nobody owns: stateless-reset trigger.
                const QuicConnectionId dcid = fiber::fuzz::make_cid(payload.empty() ? 0x99 : payload[0], 20);
                datagram[0] = static_cast<std::uint8_t>(kPacketFlagFixed | (flags & 0x1f));
                std::memcpy(datagram.data() + 1, dcid.data(), dcid.size());
                const std::size_t body = std::min(payload.size(), datagram.size() - 1 - dcid.size());
                std::memcpy(datagram.data() + 1 + dcid.size(), payload.data(), body);
                len = 1 + dcid.size() + body;
                break;
            }
        }
        if (len == 0) {
            continue;
        }
        auto result =
                Access::receive(srv.endpoint, datagram.data(), len, peer, fiber::fuzz::loopback(8443), client.now);
        FIBER_FUZZ_TRACE("  datagram len=%zu -> %s err=%d type=%d frames=%u created=%d conns=%zu\n", len,
                         result ? "ok" : "fail", result ? 0 : static_cast<int>(result.error()),
                         result ? static_cast<int>(result->packet.packet_type) : -1,
                         result ? result->packet.frame_count : 0U, result ? static_cast<int>(result->created) : 0,
                         srv.endpoint.active_connection_count());
        if ((flags & 0x10) == 0) {
            Access::pump(srv.endpoint);
        }
    }
    co_await srv.teardown();
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size < 1) {
        return 0;
    }
    fiber::fuzz::run_in_quic_loop_task([&] { return run(data, size); });
    return 0;
}
