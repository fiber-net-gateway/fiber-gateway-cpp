#ifndef FIBER_FUZZ_QUIC_FUZZ_COMMON_H
#define FIBER_FUZZ_QUIC_FUZZ_COMMON_H

// Shared helpers for the QUIC fuzz harnesses and their seed generator: a
// byte sink that makes ASan check every returned view, a cursor over the
// fuzzer input, the host loop + endpoint every QuicConnection needs, and the
// packet sealer the stateful harnesses use to put fuzzer bytes under real
// packet protection.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>
#include <fiber/net/IpAddress.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/quic/QuicConnection.h>
#include <fiber/quic/QuicCursor.h>
#include <fiber/quic/QuicPacketProcessor.h>
#include <fiber/quic/QuicProtocol.h>
#include <fiber/quic/QuicSendScheduler.h>
#include <fiber/quic/QuicUdpEndpoint.h>
#include "quic/QuicCrypto.h"
#include "quic/QuicTransportCodec.h"

namespace fiber::quic {

// Friend of QuicUdpEndpoint (declared for tests): the receive and send entry
// points the socket callbacks would drive, so harnesses inject datagrams and
// run the send path without real I/O timing.
struct QuicUdpEndpointTestAccess {
    // process_datagram: routing, stateless responses, admission, then the
    // connection's packet processor.
    static common::IoResult<QuicUdpReceiveResult> receive(QuicUdpEndpoint &endpoint, std::uint8_t *data,
                                                          std::size_t len, const net::SocketAddress &peer,
                                                          const net::SocketAddress &local,
                                                          std::chrono::steady_clock::time_point now) {
        net::UdpPacketRecvResult recv{};
        recv.size = len;
        recv.peer = peer;
        recv.local = local;
        recv.ecn = static_cast<net::UdpEcn>(static_cast<int>(len % 5) - 1);
        return endpoint.process_datagram(data, recv, now);
    }

    // Datagrams waiting in the endpoint's real socket -- what other sockets
    // (stateless Retry / Version Negotiation / reset / INVALID_TOKEN replies
    // go out directly) sent it. Non-blocking.
    template<typename Sink>
    static void drain_socket(QuicUdpEndpoint &endpoint, Sink &&sink) {
        static std::array<std::uint8_t, kQuicMaxUdpPayloadSize> buffer{};
        for (int i = 0; i < 64; ++i) {
            auto got = endpoint.socket_->try_recv_packet(buffer.data(), buffer.size());
            if (!got) {
                return;
            }
            sink(buffer.data(), got->size);
        }
    }

    // What the endpoint's posted I/O pump does next: flush every connection
    // the receive path scheduled, through the real send scheduler and socket.
    static void pump(QuicUdpEndpoint &endpoint) { endpoint.drive_io(); }

    // The send scheduler's per-datagram loop without the socket: build a
    // datagram, commit it as sent (commit is what records packets for the
    // peer's ACKs to act on), and hand the bytes to `sink` instead of the
    // socket.
    template<typename Sink>
    static std::size_t flush(QuicUdpEndpoint &endpoint, QuicConnection &connection, std::size_t max_datagrams,
                             Sink &&sink) {
        static std::array<std::uint8_t, kQuicMaxUdpPayloadSize> buffer{};
        std::size_t sent = 0;
        for (; sent < max_datagrams; ++sent) {
            QuicSendBuildState state{};
            state.congestion = connection.congestion();
            auto &paths = connection.paths().paths();
            for (std::size_t i = 0; i < paths.size(); ++i) {
                state.paths[i].path = &paths[i];
                state.paths[i].sent = paths[i].sent;
                state.paths[i].ecn_validation_sent = paths[i].ecn_validation_sent;
            }
            QuicSendDatagram datagram{};
            datagram.data = buffer.data();
            datagram.capacity = buffer.size();
            auto built = endpoint.build_send_datagram(connection, datagram, state);
            if (!built || built->status != QuicBuildSendStatus::Encoded) {
                break;
            }
            if (datagram.length == 0 || datagram.length > buffer.size()) {
                __builtin_abort();
            }
            endpoint.commit_send_datagram(connection, datagram);
            sink(datagram);
        }
        endpoint.finish_send_batch(connection);
        return sent;
    }

    static std::size_t flush(QuicUdpEndpoint &endpoint, QuicConnection &connection, std::size_t max_datagrams) {
        return flush(endpoint, connection, max_datagrams, [](const QuicSendDatagram &) noexcept {});
    }
};

} // namespace fiber::quic

namespace fiber::fuzz {

inline volatile std::uint8_t g_sink = 0;

inline void touch(const std::uint8_t *data, std::size_t len) {
    std::uint8_t acc = 0;
    for (std::size_t i = 0; i < len; ++i) {
        acc ^= data[i];
    }
    g_sink = g_sink ^ acc;
}

inline void touch(fiber::quic::QuicSlice slice) { touch(slice.data, slice.len); }

// Fuzz-safe invariant check: prints and aborts so libFuzzer records a crash
// even in builds where FIBER_ASSERT compiles out.
#define FIBER_FUZZ_CHECK(cond)                                                                                         \
    do {                                                                                                               \
        if (!(cond)) [[unlikely]] {                                                                                    \
            __builtin_printf("FIBER_FUZZ_CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);                      \
            __builtin_abort();                                                                                         \
        }                                                                                                              \
    } while (0)

// QUIC_FUZZ_TRACE=1 makes the stateful harnesses narrate what each record
// did -- for checking seeds and triaging, never while fuzzing.
inline bool trace_enabled() {
    static const bool enabled = [] {
        const char *env = std::getenv("QUIC_FUZZ_TRACE");
        return env != nullptr && env[0] == '1';
    }();
    return enabled;
}

#define FIBER_FUZZ_TRACE(...)                                                                                          \
    do {                                                                                                               \
        if (::fiber::fuzz::trace_enabled()) {                                                                          \
            std::fprintf(stderr, __VA_ARGS__);                                                                         \
        }                                                                                                              \
    } while (0)

// Sequential reader over the fuzzer input; reads past the end yield zeros
// so a harness never has to special-case a truncated tail.
class FuzzInput {
public:
    FuzzInput(const std::uint8_t *data, std::size_t size) noexcept : data_(data), size_(size) {}

    [[nodiscard]] bool empty() const noexcept { return off_ >= size_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return off_ < size_ ? size_ - off_ : 0; }

    std::uint8_t u8() noexcept { return off_ < size_ ? data_[off_++] : 0; }
    std::uint16_t u16() noexcept {
        const std::uint16_t hi = u8();
        return static_cast<std::uint16_t>((hi << 8U) | u8());
    }
    std::uint32_t u32() noexcept {
        const std::uint32_t hi = u16();
        return (hi << 16U) | u16();
    }
    // Up to `want` bytes, fewer at the end of the input.
    std::span<const std::uint8_t> bytes(std::size_t want) noexcept {
        const std::size_t n = std::min(want, remaining());
        std::span<const std::uint8_t> out(data_ + off_, n);
        off_ += n;
        return out;
    }

private:
    const std::uint8_t *data_;
    std::size_t size_;
    std::size_t off_ = 0;
};

// Address-validation key shared by the harnesses and the seed generator, so
// seeded tokens authenticate.
inline constexpr std::array<std::uint8_t, 32> kTokenKey = [] {
    std::array<std::uint8_t, 32> key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>(0x5a ^ (i * 5));
    }
    return key;
}();

inline fiber::net::SocketAddress loopback(std::uint16_t port) { return {fiber::net::IpAddress::loopback_v4(), port}; }

inline fiber::quic::QuicConnectionId make_cid(std::uint8_t seed, std::size_t len) {
    std::uint8_t bytes[fiber::quic::kMaxConnectionIdLength]{};
    for (std::size_t i = 0; i < len; ++i) {
        bytes[i] = static_cast<std::uint8_t>(seed + i);
    }
    auto cid = fiber::quic::QuicConnectionId::from_bytes(bytes, len);
    FIBER_ASSERT(cid.has_value());
    return *cid;
}

// The SCID the endpoint harness's client sends from; seeded ClientHellos
// carry it as initial_source_connection_id.
inline fiber::quic::QuicConnectionId client_scid() { return make_cid(0xC0, 8); }

// One loop and one host endpoint serve every input. The endpoint is
// initialized but never started: connections built on it are standalone
// (never indexed), it only supplies the loop, frame/crypto pools and the
// receive-storage budget -- the same arrangement as tests/QuicTestLoop.h.
inline fiber::event::EventLoop &quic_loop() {
    static fiber::event::EventLoop loop;
    return loop;
}

inline fiber::quic::QuicUdpEndpoint &quic_host() {
    static fiber::quic::QuicUdpEndpoint *endpoint = [] {
        auto *ep = new fiber::quic::QuicUdpEndpoint(quic_loop());
        fiber::quic::QuicUdpEndpoint::EndpointOptions options{};
        options.bind_addr = loopback(0);
        auto ok = ep->init(options);
        FIBER_ASSERT(ok.has_value());
        return ep;
    }();
    return *endpoint;
}

// Runs `body` as a task on quic_loop() so IoBufChain nodes and the
// connection's loop-affinity checks resolve that loop.
template<typename F>
void run_in_quic_loop(F &&body) {
    fiber::event::EventLoop &loop = quic_loop();
    (void) quic_host();
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        body();
        loop.stop();
        co_return;
    });
    loop.run();
}

// Coroutine variant for harnesses that must let the loop turn (deferred
// closes, detach on the next tick): `body` returns a Task<void>.
template<typename F>
void run_in_quic_loop_task(F &&body) {
    fiber::event::EventLoop &loop = quic_loop();
    (void) quic_host();
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        co_await body();
        loop.stop();
    });
    loop.run();
}

// Seals `payload` into one protected packet at `out`: builds the header from
// `packet` (type/flags/version/cids/token/pn already set), encrypts under
// `keys` and applies header protection. Returns the packet length, 0 on
// failure. Payloads shorter than the header-protection sample are padded
// with PADDING frames (zero bytes), like a real sender would.
inline std::size_t seal_packet(fiber::quic::QuicPacketHeader packet, fiber::quic::QuicPacketProtectionKeyView keys,
                               std::span<const std::uint8_t> payload, std::uint8_t *out, std::size_t out_cap) {
    using namespace fiber::quic;
    // Header protection samples 16 bytes starting 4 bytes after the packet
    // number, so the ciphertext (payload + tag) must reach that far.
    std::uint8_t padded[kHeaderProtectionSampleLength + 4]{};
    const std::size_t min_payload = kHeaderProtectionSampleLength + 4 - packet.pn_len;
    if (payload.size() < min_payload) {
        std::memcpy(padded, payload.data(), payload.size());
        payload = {padded, min_payload};
    }
    packet.flags = static_cast<std::uint8_t>((packet.flags & ~kPacketFlagPnLengthMask) | (packet.pn_len - 1));
    packet.length = packet.pn_len + payload.size() + kAeadTagLength;
    packet.truncated_pn = quic_truncate_packet_number(packet.packet_number, packet.pn_len);

    QuicWriteCursor writer(out, out_cap);
    std::uint8_t *pn = nullptr;
    auto header_len = quic_create_packet_header(writer, packet, &pn);
    if (!header_len || pn == nullptr) {
        return 0;
    }
    const std::size_t pn_offset = static_cast<std::size_t>(pn - out);
    if (pn_offset + packet.pn_len + payload.size() + kAeadTagLength > out_cap) {
        return 0;
    }
    packet.packet_data = out;
    packet.protected_pn = pn;
    packet.ciphertext = pn + packet.pn_len;
    packet.ciphertext_len = payload.size() + kAeadTagLength;
    auto sealed = quic_encrypt_packet_payload(packet, keys, payload.data(), payload.size(), pn + packet.pn_len,
                                              out_cap - pn_offset - packet.pn_len);
    if (!sealed) {
        return 0;
    }
    packet.packet_len = pn_offset + packet.pn_len + *sealed;
    if (!quic_apply_header_protection(packet, keys, out, packet.packet_len)) {
        return 0;
    }
    return packet.packet_len;
}

// Seals one long-header packet (Initial / 0-RTT / Handshake) into `out` at
// `offset`. `pad_to` grows the payload with PADDING so the datagram ends at
// that size (the 1200-byte Initial floor). Returns the bytes appended, 0 if
// the keys are not installed or nothing fits.
inline std::size_t seal_long_packet(fiber::quic::QuicPacketType type, std::uint32_t version,
                                    const fiber::quic::QuicConnectionId &dcid,
                                    const fiber::quic::QuicConnectionId &scid, fiber::quic::QuicSlice token,
                                    fiber::quic::QuicPacketProtectionKeyView keys, std::uint64_t pn,
                                    std::span<const std::uint8_t> payload, std::size_t pad_to, std::uint8_t *out,
                                    std::size_t offset, std::size_t cap) {
    using namespace fiber::quic;
    if (!keys.ready() || offset >= cap) {
        return 0;
    }
    QuicPacketHeader packet{};
    packet.long_header = true;
    packet.type = type;
    packet.version = version;
    packet.dcid = dcid;
    packet.scid = scid;
    packet.token = token;
    packet.pn_len = 4;
    packet.packet_number = pn;
    std::uint8_t type_bits = kLongPacketTypeInitial;
    if (type == QuicPacketType::Handshake) {
        packet.level = QuicEncryptionLevel::Handshake;
        type_bits = kLongPacketTypeHandshake;
    } else if (type == QuicPacketType::ZeroRtt) {
        packet.level = QuicEncryptionLevel::EarlyData;
        type_bits = kLongPacketTypeZeroRtt;
    } else {
        packet.level = QuicEncryptionLevel::Initial;
    }
    packet.flags = static_cast<std::uint8_t>(kPacketFlagLong | kPacketFlagFixed | type_bits);

    static std::array<std::uint8_t, kQuicMaxUdpPayloadSize> padded{};
    std::size_t len = std::min(payload.size(), padded.size() - 64);
    std::memcpy(padded.data(), payload.data(), len);
    // Header + 4-byte packet number + 2-byte length + tag around the payload.
    const std::size_t overhead = 1 + 4 + 1 + dcid.size() + 1 + scid.size() + 4 + 2 + kAeadTagLength +
                                 (type == QuicPacketType::Initial ? quic_varint_len(token.len) + token.len : 0);
    if (pad_to > offset + overhead + len) {
        const std::size_t target = std::min(pad_to - offset - overhead, padded.size() - 64);
        std::memset(padded.data() + len, 0, target - len);
        len = target;
    }
    return seal_packet(packet, keys, {padded.data(), len}, out + offset, cap - offset);
}

} // namespace fiber::fuzz

#endif // FIBER_FUZZ_QUIC_FUZZ_COMMON_H
