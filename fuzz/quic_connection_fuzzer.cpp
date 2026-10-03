// Fuzzes an established QUIC connection through its 1-RTT receive path with
// packets the harness SEALS under the peer's real keys, so frames reach the
// connection's state machine instead of dying at AEAD: streams and flow
// control, ACK processing against packets our side really sent, loss and PTO
// recovery, NEW/RETIRE_CONNECTION_ID, path validation and migration, peer key
// updates, HANDSHAKE_DONE / NEW_TOKEN on the client, and every close path.
// Our side also acts between packets (sends, stream reads/writes/resets,
// timers), so the peer's frames meet live send state.
//
// The fixture is the one the unit tests use (tests/QuicPacketProcessorTest):
// a standalone connection with Application secrets installed directly and the
// handshake marked complete. Sends go through the endpoint's real
// build/commit path, minus the socket.
//
// Input: [config][record...]
//   config bit 0: our side is the client (else the server); bit 1: small
//          local flow-control windows; bit 2: small peer limits; bit 3:
//          ChaCha20-Poly1305 (else AES-128-GCM); bit 4: client starts with
//          the handshake confirmed.
//   record: [ctl][len:u16][payload]
//     ctl & 0x03: 0 1-RTT packet, 1 1-RTT packet after a peer key update,
//                 2 raw datagram (unsealed), 3 local action script
//     packets: ctl bits 2-3 packet number: 0 next, 1 repeat the last,
//              2 skip ahead, 3 step back (reordering); bits 4-5 peer
//              address: 0 original, 1 new port (rebinding), 2 new IP,
//              3 original; bit 6 1-byte packet number encoding; bit 7 our
//              side flushes its sends afterwards.
//   action script: one action per byte (low nibble), argument in the high
//   nibble or the following byte -- see run_actions().

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#include "QuicFuzzCommon.h"

#include <fiber/quic/QuicPacketProcessor.h>
#include <fiber/quic/QuicSendScheduler.h>
#include <fiber/quic/QuicStream.h>
#include "quic/QuicCrypto.h"
#include "quic/QuicLossRecovery.h"
#include "quic/QuicTransportParamsCodec.h"

namespace {

using namespace fiber::quic;
using fiber::fuzz::FuzzInput;
using Access = QuicUdpEndpointTestAccess;

constexpr std::size_t kCidLen = 8;

void destroy_stream(void *, QuicStream &stream) noexcept { delete &stream; }

QuicStream::Lease create_stream(void *, std::uint64_t) noexcept {
    return QuicStream::Lease::adopt(new (std::nothrow) QuicStream(nullptr, destroy_stream));
}

void fixed_secret(std::uint8_t seed, std::array<std::uint8_t, 48> &out) {
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint8_t>(seed + i * 13);
    }
}

struct Harness {
    Harness(bool client, bool small_windows, bool small_peer, bool chacha, bool confirmed) :
        client(client), suite(chacha ? QuicCryptoSuite::ChaCha20Poly1305Sha256 : QuicCryptoSuite::Aes128GcmSha256),
        local_cid(fiber::fuzz::make_cid(0x10, kCidLen)), remote_cid(fiber::fuzz::make_cid(0x40, kCidLen)),
        conn(fiber::fuzz::quic_host(), options(client, small_windows)) {
        base = fiber::fuzz::quic_loop().now();
        now = base;
        ok = install_keys() && start(small_peer, confirmed);
    }

    QuicConnection::Options options(bool is_client, bool small_windows) const {
        QuicConnection::Options opts{};
        opts.role = is_client ? QuicConnectionRole::Client : QuicConnectionRole::Server;
        opts.local_addr = fiber::fuzz::loopback(8443);
        opts.remote_addr = fiber::fuzz::loopback(4433);
        opts.local_connection_id = local_cid;
        opts.remote_connection_id = remote_cid;
        opts.original_destination_connection_id = fiber::fuzz::make_cid(0x70, kCidLen);
        opts.initial_path_validated = true;
        opts.ops.create_stream = create_stream;
        opts.graceful_shutdown_grace = std::chrono::milliseconds(1000);
        if (small_windows) {
            opts.recv_flow.conn_recv_limit = 4096;
            opts.recv_flow.conn_recv_low_water = 1024;
            opts.recv_flow.stream_buffer_limit = 2048;
            opts.recv_flow.stream_low_water = 512;
            opts.recv_flow.retained_storage_limit = 16 * 1024;
            opts.transport.initial_max_data = 4096;
            opts.transport.initial_max_stream_data_bidi_local = 2048;
            opts.transport.initial_max_stream_data_bidi_remote = 2048;
            opts.transport.initial_max_stream_data_uni = 2048;
            opts.transport.initial_max_streams_bidi = 4;
            opts.transport.initial_max_streams_uni = 3;
            opts.max_peer_bidirectional_streams = 4;
            opts.max_peer_unidirectional_streams = 3;
        }
        return opts;
    }

    // Our read secret is the peer's write secret and vice versa; the peer's
    // half lives in a standalone crypto state the harness seals with.
    bool install_keys() {
        std::array<std::uint8_t, 48> peer_write{};
        std::array<std::uint8_t, 48> our_write{};
        fixed_secret(0x21, peer_write);
        fixed_secret(0x93, our_write);
        const std::size_t len = 32;
        return quic_set_encryption_secret(conn.crypto(), QuicEncryptionLevel::Application, false, suite,
                                          peer_write.data(), len)
                       .has_value() &&
               quic_set_encryption_secret(conn.crypto(), QuicEncryptionLevel::Application, true, suite,
                                          our_write.data(), len)
                       .has_value() &&
               quic_set_encryption_secret(peer, QuicEncryptionLevel::Application, true, suite, peer_write.data(), len)
                       .has_value() &&
               quic_set_encryption_secret(peer, QuicEncryptionLevel::Application, false, suite, our_write.data(), len)
                       .has_value() &&
               quic_derive_next_key_pair(conn.crypto()).has_value() && quic_derive_next_key_pair(peer).has_value();
    }

    bool start(bool small_peer, bool confirmed) {
        QuicTransportSettings limits{};
        if (small_peer) {
            limits.initial_max_data = 3000;
            limits.initial_max_stream_data_bidi_local = 1500;
            limits.initial_max_stream_data_bidi_remote = 1500;
            limits.initial_max_stream_data_uni = 1500;
            limits.initial_max_streams_bidi = 2;
            limits.initial_max_streams_uni = 2;
        }
        if (client) {
            // A client learns the server's limits from its transport
            // parameters; the 0-RTT memory path installs the same numbers.
            if (!conn.remember_peer_transport(limits)) {
                return false;
            }
        } else {
            QuicTransportParams params{};
            params.has_initial_source_connection_id = true;
            params.initial_source_connection_id = remote_cid;
            params.initial_max_data = limits.initial_max_data;
            params.initial_max_stream_data_bidi_local = limits.initial_max_stream_data_bidi_local;
            params.initial_max_stream_data_bidi_remote = limits.initial_max_stream_data_bidi_remote;
            params.initial_max_stream_data_uni = limits.initial_max_stream_data_uni;
            params.initial_max_streams_bidi = limits.initial_max_streams_bidi;
            params.initial_max_streams_uni = limits.initial_max_streams_uni;
            params.active_connection_id_limit = 4;
            params.max_idle_timeout = 30000;
            if (!conn.apply_peer_transport_params(params)) {
                return false;
            }
        }
        if (!conn.mark_established()) {
            return false;
        }
        if (!client || confirmed) {
            conn.confirm_handshake();
        }
        return true;
    }

    // RFC 9001 6: the peer rotates to its next keys and flips the phase; the
    // following generation is derived right away so it can update again.
    void peer_key_update() {
        if (!peer.next_application_keys_ready() && !quic_derive_next_key_pair(peer)) {
            return;
        }
        peer.promote_application_keys();
        peer_phase = !peer_phase;
        (void) quic_derive_next_key_pair(peer);
    }

    std::uint64_t pick_packet_number(std::uint8_t mode) {
        switch (mode) {
            case 1:
                return last_pn;
            case 2:
                next_pn += 1 + (next_pn % 61);
                break;
            case 3:
                return last_pn >= 3 ? last_pn - 2 : last_pn;
            default:
                break;
        }
        last_pn = next_pn++;
        return last_pn;
    }

    fiber::net::SocketAddress peer_address(std::uint8_t mode) const {
        switch (mode) {
            case 1:
                return fiber::fuzz::loopback(4434);
            case 2:
                return {fiber::net::IpAddress::v4({127, 0, 0, 2}), 4433};
            default:
                return fiber::fuzz::loopback(4433);
        }
    }

    std::size_t seal(std::uint8_t ctl, std::span<const std::uint8_t> payload) {
        QuicPacketHeader packet{};
        packet.long_header = false;
        packet.type = QuicPacketType::Short;
        packet.level = QuicEncryptionLevel::Application;
        packet.flags = static_cast<std::uint8_t>(kPacketFlagFixed | (peer_phase ? kPacketFlagKeyPhase : 0));
        packet.dcid = local_cid;
        packet.pn_len = (ctl & 0x40) != 0 ? 1 : 4;
        packet.packet_number = pick_packet_number((ctl >> 2) & 0x03);
        return fiber::fuzz::seal_packet(packet, peer.application_write(), payload, datagram.data(), datagram.size());
    }

    void deliver(std::size_t len, std::uint8_t address_mode) {
        if (len == 0) {
            return;
        }
        QuicReceivedDatagram received{};
        received.data = datagram.data();
        received.len = len;
        received.peer = peer_address(address_mode);
        received.local = fiber::fuzz::loopback(8443);
        received.ecn = static_cast<fiber::net::UdpEcn>(static_cast<int>(len % 5) - 1);
        received.received_at = now;
        auto result = quic_process_datagram(conn, received, static_cast<std::uint8_t>(kCidLen));
        FIBER_FUZZ_TRACE("  datagram len=%zu -> %s err=%d frames=%u state=%d close=0x%llx\n", len,
                         result ? "ok" : "fail", result ? 0 : static_cast<int>(result.error()),
                         result ? result->frame_count : 0U, static_cast<int>(conn.state()),
                         static_cast<unsigned long long>(conn.close_info().error_code));
        if (!result) {
            return;
        }
        // The endpoint's handle_receive_result, minus scheduling and NEW_TOKEN.
        auto applied = quic_apply_receive_result(conn, *result, quic_time_ms(now));
        if (!applied) {
            conn.close(QuicErrorCode::InternalError);
        }
        if (!conn.closing()) {
            conn.on_packet_processed();
        }
    }

    // ACK every packet our side has sent so far, as a real peer eventually
    // would; random ACK ranges rarely line up with what is in flight.
    void ack_all_sent() {
        const QuicPacketNumberSpace &space = conn.packet_number_space(QuicEncryptionLevel::Application);
        if (space.next_packet_number == 0) {
            return;
        }
        QuicOutputFrame frame{};
        frame.type = QuicFrameType::Ack;
        frame.u.ack.largest = space.next_packet_number - 1;
        frame.u.ack.first_range = space.next_packet_number - 1;
        std::array<std::uint8_t, 64> payload{};
        QuicWriteCursor writer(payload.data(), payload.size());
        auto written = quic_create_output_frame(&writer, frame);
        if (written) {
            deliver(seal(0, {payload.data(), *written}), 0);
        }
    }

    bool client;
    QuicCryptoSuite suite;
    QuicConnectionId local_cid;
    QuicConnectionId remote_cid;
    QuicConnection conn;
    QuicCryptoState peer{};
    bool peer_phase = false;
    bool ok = false;
    std::uint64_t next_pn = 0;
    std::uint64_t last_pn = 0;
    std::chrono::steady_clock::time_point base{};
    std::chrono::steady_clock::time_point now{};
    std::uint32_t pto_count = 0;
    std::array<std::uint8_t, 2048> datagram{};
};

QuicStream *pick_stream(Harness &h, std::uint8_t arg) {
    // Low two bits are the stream type bits, the rest picks one of the first
    // 64 ids of that type: covers local and peer streams of both kinds.
    return h.conn.find_stream(static_cast<std::uint64_t>(arg));
}

// Actions (low nibble of each byte; high nibble = argument):
//   0 flush sends     1 open bidi + write     2 open uni + write
//   3 write to [id]   4 read from [id]        5 reset [id]
//   6 stop_read [id]  7 close [id]            8 advance time + loss detection
//   9 PTO probe       10 shutdown              11 close (transport/app)
//   12 peer ACKs everything sent               13 drain every stream
// [id] actions read the stream id from the next byte.
void run_actions(Harness &h, std::span<const std::uint8_t> script) {
    static const std::uint8_t kData[8192] = {};
    FuzzInput in(script.data(), script.size());
    while (!in.empty() && !h.conn.closed()) {
        const std::uint8_t action = in.u8();
        const std::uint8_t arg = action >> 4;
        switch (action & 0x0f) {
            case 0: {
                const std::size_t sent = Access::flush(fiber::fuzz::quic_host(), h.conn, 1 + arg);
                FIBER_FUZZ_TRACE(
                        "  flush -> %zu datagrams, next_pn=%llu\n", sent,
                        static_cast<unsigned long long>(
                                h.conn.packet_number_space(QuicEncryptionLevel::Application).next_packet_number));
                break;
            }
            case 1:
            case 2: {
                const QuicStreamType type =
                        (action & 0x0f) == 1 ? QuicStreamType::Bidirectional : QuicStreamType::Unidirectional;
                auto stream = h.conn.try_attach_local_stream(
                        QuicStream::Lease::adopt(new (std::nothrow) QuicStream(nullptr, destroy_stream)), type);
                if (stream && *stream != nullptr) {
                    (void) (*stream)->try_write(kData, static_cast<std::size_t>(arg & 0x7) * 600, (arg & 0x8) != 0);
                }
                break;
            }
            case 3:
                if (QuicStream *stream = pick_stream(h, in.u8())) {
                    (void) stream->try_write(kData, static_cast<std::size_t>(arg & 0x7) * 512, (arg & 0x8) != 0);
                }
                break;
            case 4:
                if (QuicStream *stream = pick_stream(h, in.u8())) {
                    fiber::mem::IoBufChain out;
                    (void) stream->try_read(arg == 0 ? 65536 : arg * 256, out);
                }
                break;
            case 5:
                if (QuicStream *stream = pick_stream(h, in.u8())) {
                    (void) stream->reset(arg);
                }
                break;
            case 6:
                if (QuicStream *stream = pick_stream(h, in.u8())) {
                    (void) stream->stop_read(arg);
                }
                break;
            case 7:
                if (QuicStream *stream = pick_stream(h, in.u8())) {
                    stream->close(arg);
                }
                break;
            case 8:
                h.now += std::chrono::milliseconds(static_cast<int>(arg) * 40 + 1);
                (void) quic_detect_lost(h.conn, quic_time_ms(h.now), nullptr);
                break;
            case 9:
                h.now += std::chrono::milliseconds(static_cast<int>(arg) * 100 + 1);
                // Each PTO doubles the wait; past ~10 the connection's idle
                // timeout has long fired, so higher counts are unreachable.
                h.pto_count = std::min<std::uint32_t>(h.pto_count + 1, 10);
                (void) quic_queue_pto_probe_frames(h.conn, quic_time_ms(h.now), h.pto_count);
                break;
            case 10:
                if ((arg & 1) != 0) {
                    h.conn.shutdown_application(arg);
                } else {
                    h.conn.shutdown(QuicErrorCode::NoError);
                }
                break;
            case 11:
                if ((arg & 1) != 0) {
                    h.conn.close_application(arg);
                } else {
                    h.conn.close(QuicErrorCode::ProtocolViolation);
                }
                break;
            case 12:
                h.ack_all_sent();
                break;
            case 13:
                for (std::uint64_t id = 0; id < 64; ++id) {
                    if (QuicStream *stream = h.conn.find_stream(id)) {
                        fiber::mem::IoBufChain out;
                        (void) stream->try_read(65536, out);
                    }
                }
                break;
            default:
                break;
        }
    }
}

void run(const std::uint8_t *data, std::size_t size) {
    FuzzInput in(data, size);
    const std::uint8_t config = in.u8();
    {
        Harness h((config & 0x01) != 0, (config & 0x02) != 0, (config & 0x04) != 0, (config & 0x08) != 0,
                  (config & 0x10) != 0);
        if (!h.ok) {
            return;
        }
        for (int records = 0; !in.empty() && records < 128 && !h.conn.closed(); ++records) {
            const std::uint8_t ctl = in.u8();
            const std::size_t want = in.u16();
            const std::span<const std::uint8_t> payload = in.bytes(want);
            FIBER_FUZZ_TRACE("record ctl=0x%02x len=%zu\n", ctl, payload.size());
            switch (ctl & 0x03) {
                case 0:
                case 1:
                    if ((ctl & 0x03) == 1) {
                        h.peer_key_update();
                    }
                    h.deliver(h.seal(ctl, payload), (ctl >> 4) & 0x03);
                    break;
                case 2: {
                    const std::size_t len = std::min(payload.size(), h.datagram.size());
                    std::memcpy(h.datagram.data(), payload.data(), len);
                    h.deliver(len, (ctl >> 4) & 0x03);
                    break;
                }
                default:
                    run_actions(h, payload);
                    continue;
            }
            if ((ctl & 0x80) != 0) {
                (void) Access::flush(fiber::fuzz::quic_host(), h.conn, 4);
            }
        }
        // Last chance for queued work to meet the send path.
        (void) Access::flush(fiber::fuzz::quic_host(), h.conn, 4);
    }
    // Every byte a connection retained for reassembly is charged to the
    // endpoint's budget and must be returned when the connection goes.
    FIBER_FUZZ_CHECK(fiber::fuzz::quic_host().retained_recv_storage_capacity() == 0);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size < 1) {
        return 0;
    }
    fiber::fuzz::run_in_quic_loop([&] { run(data, size); });
    return 0;
}
