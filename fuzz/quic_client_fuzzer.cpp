// Fuzzes the client side of the QUIC handshake: a real client connection
// (QuicConnection::connect on an endpoint, its ClientHello queued) receives
// what the fuzzer sends as the server -- Retry and Version Negotiation,
// server Initial packets (ACK, CRYPTO carrying the ServerHello), Handshake
// packets (EncryptedExtensions with the server's transport parameters,
// Certificate, CertificateVerify, Finished), coalesced datagrams and 1-RTT.
// Everything goes through the client endpoint's datagram path, so DCID
// routing, Retry integrity checks, Initial key re-derivation after Retry,
// server CID adoption and the client's send path run as in production.
//
// Server Initial packets are sealed under the keys derived from the client's
// current Initial DCID; Handshake and 1-RTT packets under the read keys the
// client itself installed after processing the ServerHello.
//
// Input: [config][record...]
//   config bit 0: connect with an address-validation token; bit 1: TLS 1.3
//   only with a single ALPN (else h3 + an extra protocol).
//   record: [kind][flags][len:u16][payload]
//     kind & 0x07: 0 server Initial, 1 Handshake, 2 1-RTT, 3 Retry (payload
//       = token; flags bit 3 corrupts the integrity tag), 4 Version
//       Negotiation (payload = version list), 5 Initial + Handshake
//       coalesced (payload split in half), 6 raw datagram
//     Initial/Handshake payloads start with [scid_len][scid] -- the server's
//       Source CID, which the client adopts and checks against the
//       server's initial_source_connection_id transport parameter.
//     flags bits 0-1: server address (0/3 the one connected to, 1 another
//       port, 2 another IP); bit 2: pad the datagram to 1200 bytes; bit 4:
//       skip the client's send pump afterwards.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string_view>

#include "QuicFuzzCommon.h"

#include <fiber/net/TlsParams.h>
#include "quic/QuicCrypto.h"
#include "quic/QuicPacketCodec.h"

namespace {

using namespace fiber::quic;
using fiber::fuzz::FuzzInput;
using Access = QuicUdpEndpointTestAccess;

constexpr std::string_view kAlpnH3[] = {"h3"};
constexpr std::string_view kAlpnTwo[] = {"h3", "hq-interop"};

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

void destroy_connection(void *, QuicConnection &connection) noexcept { delete &connection; }

fiber::net::SocketAddress server_address(std::uint8_t flags) {
    switch (flags & 0x03) {
        case 1:
            return fiber::fuzz::loopback(4434);
        case 2:
            return {fiber::net::IpAddress::v4({127, 0, 0, 2}), 4433};
        default:
            return fiber::fuzz::loopback(4433);
    }
}

// Splits [scid_len][scid] off a server packet payload.
QuicConnectionId take_scid(std::span<const std::uint8_t> &payload) {
    if (payload.empty()) {
        return fiber::fuzz::make_cid(0x5e, 8);
    }
    const std::size_t len = std::min<std::size_t>(payload[0] % (kMaxConnectionIdLength + 1), payload.size() - 1);
    auto cid = QuicConnectionId::from_bytes(payload.data() + 1, len);
    payload = payload.subspan(1 + len);
    return cid.value_or(QuicConnectionId{});
}

struct Server {
    std::array<std::uint64_t, 4> next_pn{};

    std::uint64_t pn(QuicEncryptionLevel level) { return next_pn[static_cast<std::size_t>(level)]++; }
};

std::size_t seal_server_long(QuicConnection &conn, Server &server, QuicPacketType type,
                             std::span<const std::uint8_t> payload, std::size_t pad_to, std::uint8_t *out,
                             std::size_t offset, std::size_t cap) {
    const QuicConnectionId scid = take_scid(payload);
    if (type == QuicPacketType::Initial) {
        QuicCryptoState keys{};
        if (!quic_init_initial_crypto(keys, QuicConnectionRole::Server,
                                      conn.current_initial_destination_connection_id())) {
            return 0;
        }
        return fiber::fuzz::seal_long_packet(type, kQuicVersion1, conn.local_connection_id(), scid, {},
                                             keys.initial_write(), server.pn(QuicEncryptionLevel::Initial), payload,
                                             pad_to, out, offset, cap);
    }
    return fiber::fuzz::seal_long_packet(type, kQuicVersion1, conn.local_connection_id(), scid, {},
                                         conn.crypto().handshake_read(), server.pn(QuicEncryptionLevel::Handshake),
                                         payload, pad_to, out, offset, cap);
}

fiber::async::Task<void> run(const std::uint8_t *data, std::size_t size) {
    FuzzInput in(data, size);
    const std::uint8_t config = in.u8();
    QuicUdpEndpoint &endpoint = client_endpoint();

    QuicConnection::Options options{};
    options.role = QuicConnectionRole::Client;
    options.local_addr = endpoint.local_addr();
    options.remote_addr = fiber::fuzz::loopback(4433);
    // Fixed ODCID and client CID (the endpoint holds no other connection
    // between inputs): seeded server flights carry transport parameters that
    // name them.
    options.original_destination_connection_id = fiber::fuzz::make_cid(0xA0, 8);
    options.initial_destination_connection_id = options.original_destination_connection_id;
    options.remote_connection_id = options.original_destination_connection_id;
    options.local_connection_id = fiber::fuzz::client_scid();
    options.on_destroy = destroy_connection;
    auto *raw = new (std::nothrow) QuicConnection(endpoint, options);
    if (raw == nullptr) {
        co_return;
    }
    QuicConnection::Lease conn = QuicConnection::Lease::adopt(raw);

    QuicClientConnectParams params{};
    params.tls.min_version = 0x0304;
    params.tls.max_version = 0x0304;
    params.tls.alpn = (config & 0x02) != 0 ? std::span<const std::string_view>(kAlpnH3)
                                           : std::span<const std::string_view>(kAlpnTwo);
    params.tls.server_name = "localhost";
    params.allow_insecure = true;
    static constexpr std::uint8_t kToken[] = {0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x80, 0x81};
    if ((config & 0x01) != 0) {
        params.token = kToken;
        params.token_len = sizeof(kToken);
    }
    if (conn->connect(params)) {
        Access::pump(endpoint);
        Server server{};
        auto now = fiber::fuzz::quic_loop().now();
        static std::array<std::uint8_t, kQuicMaxUdpPayloadSize> datagram{};

        for (int records = 0; !in.empty() && records < 64 && !conn->closed(); ++records) {
            const std::uint8_t kind = in.u8();
            const std::uint8_t flags = in.u8();
            std::span<const std::uint8_t> payload = in.bytes(in.u16());
            FIBER_FUZZ_TRACE("record kind=0x%02x flags=0x%02x len=%zu\n", kind, flags, payload.size());
            const std::size_t pad_to = (flags & 0x04) != 0 ? kMinInitialDatagramSize : 0;
            now += std::chrono::milliseconds(1);

            std::size_t len = 0;
            switch (kind & 0x07) {
                case 0:
                    len = seal_server_long(*conn, server, QuicPacketType::Initial, payload, pad_to, datagram.data(), 0,
                                           datagram.size());
                    break;
                case 1:
                    len = seal_server_long(*conn, server, QuicPacketType::Handshake, payload, pad_to, datagram.data(),
                                           0, datagram.size());
                    break;
                case 2: {
                    QuicPacketHeader packet{};
                    packet.type = QuicPacketType::Short;
                    packet.level = QuicEncryptionLevel::Application;
                    packet.flags =
                            static_cast<std::uint8_t>(kPacketFlagFixed | (conn->key_phase() ? kPacketFlagKeyPhase : 0));
                    packet.dcid = conn->local_connection_id();
                    packet.pn_len = 4;
                    packet.packet_number = server.pn(QuicEncryptionLevel::Application);
                    const auto keys = conn->crypto().application_read();
                    if (keys.ready()) {
                        len = fiber::fuzz::seal_packet(packet, keys, payload, datagram.data(), datagram.size());
                    }
                    break;
                }
                case 3: {
                    QuicRetryPacketSpec spec{};
                    spec.original_dcid = conn->original_destination_connection_id();
                    spec.dcid = conn->local_connection_id();
                    spec.scid = fiber::fuzz::make_cid(payload.empty() ? 0x33 : payload[0], 8);
                    spec.token = {payload.data(), std::min<std::size_t>(payload.size(), 1024)};
                    QuicWriteCursor writer(datagram.data(), datagram.size());
                    auto built = quic_create_retry_packet(spec, writer);
                    if (built) {
                        len = *built;
                        if ((flags & 0x08) != 0) {
                            datagram[len - 1] ^= 0x01;
                        }
                    }
                    break;
                }
                case 4: {
                    // [long|random][version 0][dcid][scid][versions...]
                    QuicWriteCursor writer(datagram.data(), datagram.size());
                    const QuicConnectionId dcid = conn->local_connection_id();
                    const QuicConnectionId scid = conn->original_destination_connection_id();
                    (void) writer.write_u8(static_cast<std::uint8_t>(kPacketFlagLong | (flags & 0x7f)));
                    (void) writer.write_be32(0);
                    (void) writer.write_u8(dcid.size());
                    (void) writer.write_bytes(dcid.data(), dcid.size());
                    (void) writer.write_u8(scid.size());
                    (void) writer.write_bytes(scid.data(), scid.size());
                    (void) writer.write_bytes(payload.data(), std::min(payload.size(), writer.remaining()));
                    len = writer.offset();
                    break;
                }
                case 5: {
                    std::span<const std::uint8_t> first = payload.first(payload.size() / 2);
                    std::span<const std::uint8_t> second = payload.subspan(payload.size() / 2);
                    len = seal_server_long(*conn, server, QuicPacketType::Initial, first, 0, datagram.data(), 0,
                                           datagram.size());
                    if (len != 0) {
                        len += seal_server_long(*conn, server, QuicPacketType::Handshake, second, pad_to,
                                                datagram.data(), len, datagram.size());
                    }
                    break;
                }
                default:
                    len = std::min(payload.size(), datagram.size());
                    std::memcpy(datagram.data(), payload.data(), len);
                    break;
            }
            if (len == 0) {
                continue;
            }
            auto result =
                    Access::receive(endpoint, datagram.data(), len, server_address(flags), endpoint.local_addr(), now);
            FIBER_FUZZ_TRACE("  datagram len=%zu -> %s err=%d type=%d frames=%u state=%d\n", len,
                             result ? "ok" : "fail", result ? 0 : static_cast<int>(result.error()),
                             result ? static_cast<int>(result->packet.packet_type) : -1,
                             result ? result->packet.frame_count : 0U, static_cast<int>(conn->state()));
            if ((flags & 0x10) == 0) {
                Access::pump(endpoint);
            }
        }
    }
    if (!conn->closed()) {
        conn->close_immediately();
    }
    co_await conn->wait_closed();
    conn.reset();
    FIBER_FUZZ_CHECK(endpoint.active_connection_count() == 0);
    FIBER_FUZZ_CHECK(endpoint.retained_recv_storage_capacity() == 0);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size < 1) {
        return 0;
    }
    fiber::fuzz::run_in_quic_loop_task([&] { return run(data, size); });
    return 0;
}
