// Seed generator for the QUIC fuzz harnesses. Usage: quic_fuzz_seedgen <dir>
//
// Every seed is built with the in-tree encoders (frames, transport
// parameters, packet headers, Retry packets, address tokens under the
// harnesses' fixed key), so each starts a fuzzer on a well-formed path. The
// endpoint seeds carry a real ClientHello: the in-tree client runs connect()
// and its first CRYPTO flight is lifted out of the Initial send queue. That
// part is random per run (key shares), the rest is deterministic.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "QuicFuzzCommon.h"

#include <fiber/async/Task.h>

#include "../tests/QuicTestTlsCertificate.h"

#include <fiber/net/TlsCredential.h>
#include <fiber/net/TlsParams.h>
#include <fiber/net/TlsServerHandshakeConfig.h>
#include <fiber/quic/QuicToken.h>
#include "quic/QuicCrypto.h"
#include "quic/QuicPacketCodec.h"
#include "quic/QuicTransportParamsCodec.h"

namespace {

using namespace fiber::quic;
using Bytes = std::vector<std::uint8_t>;

std::filesystem::path g_out;
std::size_t g_written = 0;

void write_seed(const char *fuzzer, const std::string &name, const Bytes &bytes) {
    const std::filesystem::path dir = g_out / fuzzer;
    std::filesystem::create_directories(dir);
    std::ofstream out(dir / name, std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    ++g_written;
}

void put_u8(Bytes &b, std::uint8_t v) { b.push_back(v); }
void put_u16(Bytes &b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v >> 8U));
    b.push_back(static_cast<std::uint8_t>(v));
}
void put(Bytes &b, const Bytes &more) { b.insert(b.end(), more.begin(), more.end()); }
void put(Bytes &b, const std::uint8_t *data, std::size_t len) { b.insert(b.end(), data, data + len); }
void put_varint(Bytes &b, std::uint64_t v) {
    std::array<std::uint8_t, 8> buf{};
    QuicWriteCursor w(buf.data(), buf.size());
    (void) quic_write_varint(w, v);
    put(b, buf.data(), w.offset());
}

// ---------------------------------------------------------------- frames

Bytes encode(QuicOutputFrame &frame) {
    std::array<std::uint8_t, 4096> buf{};
    QuicWriteCursor w(buf.data(), buf.size());
    auto n = quic_create_output_frame(&w, frame);
    FIBER_ASSERT(n.has_value());
    return {buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n)};
}

Bytes stream_frame(std::uint64_t id, std::uint64_t offset, std::string_view data, bool fin, bool explicit_len = true) {
    Bytes b;
    std::uint8_t type = 0x08;
    type |= offset != 0 ? 0x04 : 0;
    type |= explicit_len ? 0x02 : 0;
    type |= fin ? 0x01 : 0;
    put_u8(b, type);
    put_varint(b, id);
    if (offset != 0) {
        put_varint(b, offset);
    }
    if (explicit_len) {
        put_varint(b, data.size());
    }
    put(b, reinterpret_cast<const std::uint8_t *>(data.data()), data.size());
    return b;
}

Bytes crypto_frame(std::uint64_t offset, const std::uint8_t *data, std::size_t len) {
    Bytes b;
    put_u8(b, 0x06);
    put_varint(b, offset);
    put_varint(b, len);
    put(b, data, len);
    return b;
}

Bytes ack_frame(std::uint64_t largest, std::uint64_t first_range, std::uint64_t delay = 0) {
    QuicOutputFrame f{};
    f.type = QuicFrameType::Ack;
    f.u.ack.largest = largest;
    f.u.ack.first_range = first_range;
    f.u.ack.delay = delay;
    return encode(f);
}

Bytes ack_ecn_frame(std::uint64_t largest, std::uint64_t first_range) {
    // One gap: acknowledges [largest-first_range, largest] and [0, 1].
    Bytes ranges;
    put_varint(ranges, largest > first_range + 4 ? largest - first_range - 4 : 0);
    put_varint(ranges, 1);
    QuicOutputFrame f{};
    f.type = QuicFrameType::AckEcn;
    f.u.ack.largest = largest;
    f.u.ack.first_range = first_range;
    f.u.ack.range_count = 1;
    FIBER_ASSERT(quic_output_frame_set_owned_data(f, ranges.data(), ranges.size()).has_value());
    f.u.ack.ect0 = 3;
    f.u.ack.ce = 1;
    return encode(f);
}

Bytes simple(QuicFrameType type) {
    QuicOutputFrame f{};
    f.type = type;
    return encode(f);
}

Bytes close_frame(bool app, std::uint64_t code, std::string_view reason) {
    QuicOutputFrame f{};
    f.type = app ? QuicFrameType::ConnectionCloseApp : QuicFrameType::ConnectionClose;
    f.u.close.error_code = code;
    f.u.close.frame_type = app ? 0 : 0x08;
    FIBER_ASSERT(
            quic_output_frame_set_owned_data(f, reinterpret_cast<const std::uint8_t *>(reason.data()), reason.size())
                    .has_value());
    return encode(f);
}

Bytes new_cid_frame(std::uint64_t seq, std::uint64_t retire_prior_to, std::uint8_t seed) {
    QuicOutputFrame f{};
    f.type = QuicFrameType::NewConnectionId;
    f.u.new_connection_id.sequence_number = seq;
    f.u.new_connection_id.retire_prior_to = retire_prior_to;
    f.u.new_connection_id.cid_len = 8;
    for (std::uint8_t i = 0; i < 8; ++i) {
        f.u.new_connection_id.cid[i] = static_cast<std::uint8_t>(seed + i);
    }
    for (std::uint8_t i = 0; i < kStatelessResetTokenLength; ++i) {
        f.u.new_connection_id.stateless_reset_token[i] = static_cast<std::uint8_t>(seed ^ (i * 3));
    }
    return encode(f);
}

Bytes id_frame(QuicFrameType type, std::uint64_t a, std::uint64_t b = 0, std::uint64_t c = 0) {
    QuicOutputFrame f{};
    f.type = type;
    switch (type) {
        case QuicFrameType::ResetStream:
            f.u.reset_stream = {a, b, c};
            break;
        case QuicFrameType::StopSending:
            f.u.stop_sending = {a, b};
            break;
        case QuicFrameType::MaxData:
            f.u.max_data.max_data = a;
            break;
        case QuicFrameType::MaxStreamData:
            f.u.max_stream_data = {a, b};
            break;
        case QuicFrameType::MaxStreamsBidi:
        case QuicFrameType::MaxStreamsUni:
            f.u.max_streams.limit = a;
            break;
        case QuicFrameType::DataBlocked:
            f.u.data_blocked.limit = a;
            break;
        case QuicFrameType::StreamDataBlocked:
            f.u.stream_data_blocked = {a, b};
            break;
        case QuicFrameType::StreamsBlockedBidi:
        case QuicFrameType::StreamsBlockedUni:
            f.u.streams_blocked.limit = a;
            break;
        case QuicFrameType::RetireConnectionId:
            f.u.retire_connection_id.sequence_number = a;
            break;
        case QuicFrameType::PathChallenge:
        case QuicFrameType::PathResponse:
            for (std::uint8_t i = 0; i < 8; ++i) {
                f.u.path_challenge.data[i] = static_cast<std::uint8_t>(a + i);
            }
            break;
        default:
            break;
    }
    return encode(f);
}

Bytes new_token_frame() {
    static const std::uint8_t token[] = {'t', 'o', 'k', 'e', 'n', '-', 'b', 'y', 't', 'e', 's'};
    QuicOutputFrame f{};
    f.type = QuicFrameType::NewToken;
    FIBER_ASSERT(quic_output_frame_set_owned_data(f, token, sizeof(token)).has_value());
    return encode(f);
}

// ---------------------------------------------------------------- codec

void codec_seeds() {
    const char *fz = "quic_codec_fuzzer";
    const std::string_view hello = "hello";
    const std::uint8_t crypto_bytes[] = {0x01, 0x00, 0x00, 0x04, 0x03, 0x03, 0xaa, 0xbb};
    // Level x receiver role: a payload of every frame the level allows (the
    // parser stops at the first frame it refuses, so later frames still seed
    // the other branches through mutation).
    for (std::uint8_t level = 0; level < 4; ++level) {
        for (std::uint8_t role = 0; role < 2; ++role) {
            Bytes b;
            put_u8(b, static_cast<std::uint8_t>(0 | (role << 3) | (level << 4)));
            put(b, simple(QuicFrameType::Ping));
            if (level != 1) {
                put(b, ack_frame(20, 5, 7));
                put(b, ack_ecn_frame(40, 3));
                put(b, crypto_frame(0, crypto_bytes, sizeof(crypto_bytes)));
            }
            if (level == 3 || level == 1) {
                put(b, stream_frame(0, 0, hello, false));
                put(b, stream_frame(4, 100, hello, true));
                put(b, id_frame(QuicFrameType::ResetStream, 8, 1, 50));
                put(b, id_frame(QuicFrameType::StopSending, 4, 2));
                put(b, id_frame(QuicFrameType::MaxData, 1 << 20));
                put(b, id_frame(QuicFrameType::MaxStreamData, 0, 4096));
                put(b, id_frame(QuicFrameType::MaxStreamsBidi, 100));
                put(b, id_frame(QuicFrameType::MaxStreamsUni, 3));
                put(b, id_frame(QuicFrameType::DataBlocked, 77));
                put(b, id_frame(QuicFrameType::StreamDataBlocked, 0, 77));
                put(b, id_frame(QuicFrameType::StreamsBlockedBidi, 9));
                put(b, id_frame(QuicFrameType::StreamsBlockedUni, 9));
                put(b, new_cid_frame(1, 0, 0x30));
                put(b, id_frame(QuicFrameType::RetireConnectionId, 0));
                put(b, id_frame(QuicFrameType::PathChallenge, 0x11));
                put(b, id_frame(QuicFrameType::PathResponse, 0x22));
                put(b, close_frame(true, 0x101, "app"));
                if (role == 1 && level == 3) {
                    // Server-to-client only.
                    put(b, new_token_frame());
                    put(b, simple(QuicFrameType::HandshakeDone));
                }
            }
            put(b, close_frame(false, 0x0a, "bye"));
            put(b, stream_frame(0, 0, hello, true, false));
            b.resize(b.size() + 3, 0); // trailing PADDING
            write_seed(fz, "frames_l" + std::to_string(level) + "_r" + std::to_string(role), b);
        }
    }

    // Transport parameters, both owners, with every field the owner may send.
    for (std::uint8_t owner = 0; owner < 2; ++owner) {
        QuicTransportParams p{};
        p.max_idle_timeout = 30000;
        p.max_udp_payload_size = 1472;
        p.initial_max_data = 1 << 20;
        p.initial_max_stream_data_bidi_local = 65536;
        p.initial_max_stream_data_bidi_remote = 65536;
        p.initial_max_stream_data_uni = 65536;
        p.initial_max_streams_bidi = 100;
        p.initial_max_streams_uni = 3;
        p.ack_delay_exponent = 3;
        p.max_ack_delay = 25;
        p.active_connection_id_limit = 4;
        p.disable_active_migration = true;
        p.has_initial_source_connection_id = true;
        p.initial_source_connection_id = fiber::fuzz::make_cid(0x40, 8);
        if (owner == 1) {
            p.has_original_destination_connection_id = true;
            p.original_destination_connection_id = fiber::fuzz::make_cid(0x70, 8);
            p.has_retry_source_connection_id = true;
            p.retry_source_connection_id = fiber::fuzz::make_cid(0x80, 8);
            p.has_stateless_reset_token = true;
            std::memset(p.stateless_reset_token, 0x5c, sizeof(p.stateless_reset_token));
            p.has_preferred_address = true;
            p.preferred_address.ipv4 = {fiber::net::IpAddress::v4({10, 0, 0, 1}), 443};
            p.preferred_address.connection_id = fiber::fuzz::make_cid(0x90, 8);
        }
        std::array<std::uint8_t, 1024> buf{};
        QuicWriteCursor w(buf.data(), buf.size());
        const auto who = owner == 1 ? QuicTransportParamOwner::Server : QuicTransportParamOwner::Client;
        auto n = quic_create_transport_params(who, &w, p);
        FIBER_ASSERT(n.has_value());
        Bytes b;
        put_u8(b, static_cast<std::uint8_t>(1 | (owner << 3)));
        put(b, buf.data(), *n);
        // A reserved (greased) parameter the parser must skip.
        put_varint(b, 27 + 31 * 5);
        put_varint(b, 2);
        put_u8(b, 0xab);
        put_u8(b, 0xcd);
        write_seed(fz, owner == 1 ? "tp_server" : "tp_client", b);
    }

    // Packet headers, short-header DCID length 8 (selector bits 3-7 = 8).
    const std::uint8_t header_selector = static_cast<std::uint8_t>(2 | (8 << 3));
    const auto dcid = fiber::fuzz::make_cid(0x10, 8);
    const auto scid = fiber::fuzz::make_cid(0x40, 8);
    const std::uint8_t token[] = {1, 2, 3, 4};
    const struct {
        QuicPacketType type;
        std::uint8_t bits;
        const char *name;
    } longs[] = {{QuicPacketType::Initial, kLongPacketTypeInitial, "hdr_initial"},
                 {QuicPacketType::ZeroRtt, kLongPacketTypeZeroRtt, "hdr_0rtt"},
                 {QuicPacketType::Handshake, kLongPacketTypeHandshake, "hdr_handshake"},
                 {QuicPacketType::Short, 0, "hdr_short"}};
    for (const auto &l: longs) {
        QuicPacketHeader h{};
        h.long_header = l.type != QuicPacketType::Short;
        h.type = l.type;
        h.version = kQuicVersion1;
        h.flags = static_cast<std::uint8_t>((h.long_header ? kPacketFlagLong | l.bits : 0) | kPacketFlagFixed | 0x03);
        h.dcid = dcid;
        h.scid = scid;
        if (l.type == QuicPacketType::Initial) {
            h.token = {token, sizeof(token)};
        }
        h.pn_len = 4;
        h.length = 4 + 24;
        std::array<std::uint8_t, 256> buf{};
        QuicWriteCursor w(buf.data(), buf.size());
        std::uint8_t *pn = nullptr;
        auto n = quic_create_packet_header(w, h, &pn);
        FIBER_ASSERT(n.has_value());
        Bytes b;
        put_u8(b, header_selector);
        put(b, buf.data(), *n);
        b.resize(b.size() + 24, 0x5a);
        write_seed(fz, l.name, b);
    }
    {
        QuicPacketHeader req{};
        req.long_header = true;
        req.type = QuicPacketType::VersionNegotiation;
        req.flags = kPacketFlagLong | kPacketFlagFixed;
        req.dcid = scid;
        req.scid = dcid;
        std::array<std::uint8_t, 256> buf{};
        QuicWriteCursor w(buf.data(), buf.size());
        auto n = quic_create_version_negotiation_packet(req, w);
        FIBER_ASSERT(n.has_value());
        Bytes b;
        put_u8(b, header_selector);
        put(b, buf.data(), *n);
        write_seed(fz, "hdr_vn", b);
    }

    // Address tokens (selector 3): [port][now offset][token], minted for
    // loopback:port under the shared key and still valid at the harness clock.
    for (std::uint8_t kind = 0; kind < 2; ++kind) {
        const std::uint16_t port = 4433;
        const auto odcid = fiber::fuzz::make_cid(0x70, 8);
        auto minted = quic_create_address_token(
                fiber::fuzz::kTokenKey, fiber::fuzz::loopback(port), 1'800'000'000 + 600,
                kind == 1 ? QuicAddressTokenKind::Retry : QuicAddressTokenKind::NewToken, kind == 1 ? &odcid : nullptr);
        FIBER_ASSERT(minted.has_value());
        Bytes b;
        put_u8(b, 3);
        put_u16(b, port);
        put_u8(b, 5);
        put(b, minted->bytes.data(), minted->len);
        write_seed(fz, kind == 1 ? "token_retry" : "token_new", b);
    }

    // Retry (selector 4): cid seeds/lengths, then a Retry whose tag is valid
    // for that ODCID.
    {
        const std::uint8_t cids[6] = {0x70, 8, 0x40, 8, 0x91, 8};
        QuicRetryPacketSpec spec{};
        spec.original_dcid = fiber::fuzz::make_cid(cids[0], cids[1]);
        spec.dcid = fiber::fuzz::make_cid(cids[2], cids[3]);
        spec.scid = fiber::fuzz::make_cid(cids[4], cids[5]);
        spec.token = {token, sizeof(token)};
        std::array<std::uint8_t, 512> buf{};
        QuicWriteCursor w(buf.data(), buf.size());
        auto n = quic_create_retry_packet(spec, w);
        FIBER_ASSERT(n.has_value());
        Bytes b;
        put_u8(b, 4);
        put(b, cids, sizeof(cids));
        put(b, buf.data(), *n);
        write_seed(fz, "retry", b);
    }

    // Packet numbers + varints (selector 5).
    {
        Bytes b{5, 0x03, 0, 0, 0, 0, 0, 0, 0x10, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xff};
        put_varint(b, 37);
        put_varint(b, 15293);
        put_varint(b, 494878333);
        put_varint(b, 151288809941952652ULL);
        write_seed(fz, "pn_varint", b);
    }
}

// ---------------------------------------------------------------- reassembly

void reassembly_seeds() {
    const char *fz = "quic_reassembly_fuzzer";
    auto data_op = [](Bytes &b, std::uint8_t kind, std::uint16_t off, std::uint16_t len) {
        put_u8(b, kind);
        put_u16(b, off);
        put_u16(b, len);
    };
    auto take = [](Bytes &b, std::uint16_t max) {
        put_u8(b, 1);
        put_u16(b, max);
    };
    for (std::uint8_t config: {0x00, 0x02, 0x42, 0x01, 0x03, 0x43, 0x0d, 0x17, 0x2a}) {
        Bytes b{config};
        data_op(b, 0x00, 0, 100); // in order
        take(b, 0);
        data_op(b, 0x00, 300, 100); // gap
        data_op(b, 0x10, 350, 100); // conflicting overlap
        data_op(b, 0x00, 100, 250); // fills the gap, overlaps both sides
        take(b, 120);
        take(b, 0);
        data_op(b, 0x20, 450, 50); // FIN at 500 (stream)
        data_op(b, 0x00, 400, 100); // retransmission below FIN
        take(b, 0);
        data_op(b, 0x04, 10, 10); // far but in u32 range (+ u16 << 16)
        put_u16(b, 1);
        data_op(b, 0x08, 0, 64); // near 2^62
        take(b, 0);
        put_u8(b, 0x03); // raise MAX_STREAM_DATA
        put_u16(b, 0x400);
        put_u8(b, 0x06); // RESET_STREAM final 500
        put_u16(b, 500);
        put_u8(b, 0x02); // STOP_SENDING / discard
        take(b, 0);
        char name[32];
        std::snprintf(name, sizeof(name), "script_%02x", config);
        write_seed(fz, name, b);
    }
    // Many small out-of-order extents against the extent caps.
    for (std::uint8_t config: {0x10, 0x30, 0x01}) {
        Bytes b{config};
        for (std::uint16_t i = 0; i < 24; ++i) {
            data_op(b, 0x00, static_cast<std::uint16_t>(1000 - i * 40), 20);
        }
        data_op(b, 0x00, 0, 1000);
        take(b, 0);
        char name[32];
        std::snprintf(name, sizeof(name), "extents_%02x", config);
        write_seed(fz, name, b);
    }
}

// ---------------------------------------------------------------- connection

Bytes conn_record(std::uint8_t ctl, const Bytes &payload) {
    Bytes b;
    put_u8(b, ctl);
    put_u16(b, static_cast<std::uint16_t>(payload.size()));
    put(b, payload);
    return b;
}

void connection_seeds() {
    const char *fz = "quic_connection_fuzzer";
    // ctl: kind 0 = sealed 1-RTT packet, 1 = after peer key update,
    // 3 = actions. Action bytes: 0x?0 flush, 0x?1 open bidi+write,
    // 0x?2 open uni+write, 0x?3 [id] write, 0x?4 [id] read, 0x?5 [id] reset,
    // 0x?6 [id] stop, 0x?7 [id] close, 0x?8 time+loss, 0x?9 PTO,
    // 0x?a shutdown, 0x?b close, 0x0c peer acks all, 0x0d drain all.
    const Bytes flush_ack = {0x30, 0x0c};
    for (std::uint8_t role = 0; role < 2; ++role) {
        // Server our side: peer (client) streams are 0/4 (bidi), 2/6 (uni).
        // Client our side: peer (server) streams are 1/5 (bidi), 3/7 (uni).
        const std::uint64_t pb = role == 0 ? 0 : 1;
        const std::uint64_t pu = role == 0 ? 2 : 3;
        const std::uint64_t lb = role == 0 ? 1 : 0;
        const std::uint8_t config = role;
        const std::string r = role == 0 ? "srv_" : "cli_";

        Bytes s{config};
        put(s, conn_record(0x00, simple(QuicFrameType::Ping)));
        put(s, conn_record(0x00, stream_frame(pb, 0, "GET /index.html", false)));
        put(s, conn_record(0x00, stream_frame(pu, 0, "uni-data", false)));
        put(s, conn_record(0x03, {0x04, static_cast<std::uint8_t>(pb), 0x31, 0x72, 0x30}));
        put(s, conn_record(0x00, stream_frame(pb, 15, "-more", true)));
        put(s, conn_record(0x03, {0x0d, 0x83, static_cast<std::uint8_t>(pb), 0x40}));
        put(s, conn_record(0x03, flush_ack));
        put(s, conn_record(0x00, id_frame(QuicFrameType::MaxData, 1 << 22)));
        put(s, conn_record(0x00, id_frame(QuicFrameType::MaxStreamData, lb, 1 << 20)));
        put(s, conn_record(0x03, {0x83, static_cast<std::uint8_t>(lb), 0x30, 0x0c}));
        write_seed(fz, r + "streams", s);

        Bytes a{config};
        put(a, conn_record(0x03, {0x71, 0x72, 0x30}));
        put(a, conn_record(0x00, ack_frame(1, 1, 10)));
        put(a, conn_record(0x03, {0xf8, 0x39, 0x30}));
        put(a, conn_record(0x00, ack_ecn_frame(3, 0)));
        put(a, conn_record(0x03, {0x29, 0x30, 0x0c}));
        put(a, conn_record(0x04, simple(QuicFrameType::Ping))); // duplicate pn
        put(a, conn_record(0x08, simple(QuicFrameType::Ping))); // pn gap
        put(a, conn_record(0x0c, simple(QuicFrameType::Ping))); // reordered
        write_seed(fz, r + "ack_loss", a);

        Bytes k{static_cast<std::uint8_t>(config | 0x10)};
        put(k, conn_record(0x00, simple(QuicFrameType::Ping)));
        put(k, conn_record(0x01, simple(QuicFrameType::Ping))); // key update
        put(k, conn_record(0x0c, simple(QuicFrameType::Ping))); // old phase, reordered
        put(k, conn_record(0x01, stream_frame(pb, 0, "after-ku", true)));
        put(k, conn_record(0x03, flush_ack));
        write_seed(fz, r + "key_update", k);

        Bytes p{config};
        put(p, conn_record(0x00, new_cid_frame(1, 0, 0x30)));
        put(p, conn_record(0x00, new_cid_frame(2, 1, 0x50)));
        put(p, conn_record(0x10, id_frame(QuicFrameType::PathChallenge, 0x11))); // new port
        put(p, conn_record(0x90, stream_frame(pb, 0, "migrated", false))); // + flush
        put(p, conn_record(0x10, id_frame(QuicFrameType::PathResponse, 0)));
        put(p, conn_record(0x20, simple(QuicFrameType::Ping))); // new IP
        put(p, conn_record(0x00, id_frame(QuicFrameType::RetireConnectionId, 0)));
        put(p, conn_record(0x03, {0x30}));
        write_seed(fz, r + "paths", p);

        Bytes c{static_cast<std::uint8_t>(config | 0x06)}; // small windows + small peer limits
        put(c, conn_record(0x00, stream_frame(pb, 0, std::string(1500, 'x'), false)));
        put(c, conn_record(0x00, stream_frame(pb + 4, 0, std::string(1500, 'y'), false)));
        put(c, conn_record(0x00, id_frame(QuicFrameType::DataBlocked, 4096)));
        put(c, conn_record(0x00, id_frame(QuicFrameType::StreamDataBlocked, pb, 2048)));
        put(c, conn_record(0x00, id_frame(QuicFrameType::StreamsBlockedBidi, 4)));
        put(c, conn_record(0x03, {0x0d, 0x30}));
        put(c, conn_record(0x00, id_frame(QuicFrameType::ResetStream, pb + 4, 7, 1500)));
        put(c, conn_record(0x00, id_frame(QuicFrameType::StopSending, lb, 9)));
        put(c, conn_record(0x00, id_frame(QuicFrameType::MaxStreamsBidi, 10)));
        put(c, conn_record(0x00, id_frame(QuicFrameType::MaxStreamsUni, 10)));
        put(c, conn_record(0x03, {0x71, 0x82, 0x30, 0x0c}));
        write_seed(fz, r + "flow_control", c);

        Bytes e{config};
        put(e, conn_record(0x00, stream_frame(pb, 0, "req", false)));
        put(e, conn_record(0x03, {0x0a, 0x30}));
        put(e, conn_record(0x00, stream_frame(pb, 3, "-fin", true)));
        put(e, conn_record(0x00, close_frame(true, 0x100, "done")));
        write_seed(fz, r + "shutdown_close", e);

        Bytes h{config};
        put(h, conn_record(0x00, simple(QuicFrameType::HandshakeDone)));
        put(h, conn_record(0x00, new_token_frame()));
        put(h, conn_record(0x00, close_frame(false, 0x0a, "proto")));
        write_seed(fz, r + "handshake_done_token", h);
    }
}

// ---------------------------------------------------------------- endpoint

Bytes ep_record(std::uint8_t kind, std::uint8_t flags, const Bytes &payload) {
    Bytes b;
    put_u8(b, kind);
    put_u8(b, flags);
    put_u16(b, static_cast<std::uint16_t>(payload.size()));
    put(b, payload);
    return b;
}

// The ClientHello the in-tree client sends, lifted from its Initial queue.
fiber::async::Task<Bytes> capture_client_hello() {
    static fiber::quic::QuicUdpEndpoint *client_endpoint = [] {
        auto *ep = new fiber::quic::QuicUdpEndpoint(fiber::fuzz::quic_loop());
        fiber::quic::QuicUdpEndpoint::EndpointOptions options{};
        options.bind_addr = fiber::fuzz::loopback(0);
        auto ok = ep->init(options);
        FIBER_ASSERT(ok.has_value());
        return ep;
    }();
    static constexpr std::string_view kAlpn[] = {"h3"};
    Bytes hello;
    auto identity = client_endpoint->allocate_client_identity();
    FIBER_ASSERT(identity.has_value());
    QuicConnection::Options options{};
    options.role = QuicConnectionRole::Client;
    options.local_addr = client_endpoint->local_addr();
    options.remote_addr = fiber::fuzz::loopback(4433);
    options.original_destination_connection_id = identity->original_destination_connection_id;
    options.initial_destination_connection_id = identity->original_destination_connection_id;
    options.remote_connection_id = identity->original_destination_connection_id;
    // The ClientHello's initial_source_connection_id must match the SCID the
    // endpoint harness sends from (QuicFuzzCommon kClientScid).
    options.local_connection_id = fiber::fuzz::client_scid();
    QuicConnection connection(*client_endpoint, options);
    QuicClientConnectParams params{};
    params.tls.min_version = 0x0304;
    params.tls.max_version = 0x0304;
    params.tls.alpn = kAlpn;
    params.tls.server_name = "localhost";
    params.allow_insecure = true;
    auto connected = connection.connect(params);
    FIBER_ASSERT(connected.has_value());
    // CRYPTO frames queued at the Initial level, in offset order.
    auto &space = connection.packet_number_space(QuicEncryptionLevel::Initial);
    for (const QuicOutputFrame *f = space.pending_frames.front(); f != nullptr; f = space.pending_frames.next_of(*f)) {
        if (f->type == QuicFrameType::Crypto && f->data && f->u.crypto.offset == hello.size()) {
            put(hello, f->data.readable_data(), f->data.readable());
        }
    }
    connection.close_immediately();
    co_await connection.wait_closed();
    co_return hello;
}

struct ServerFlight {
    Bytes server_hello; // Initial CRYPTO
    Bytes handshake; // Handshake CRYPTO: EncryptedExtensions .. Finished
    QuicConnectionId scid{};
};

QuicConnection::Lease g_server_connection{};

void destroy_server_connection(void *, QuicConnection &connection) noexcept { delete &connection; }

QuicConnection::Lease create_server_connection(void *, QuicUdpEndpoint &endpoint,
                                               const QuicConnection::Options &options) noexcept {
    QuicConnection::Options opts = options;
    opts.on_destroy = destroy_server_connection;
    auto *connection = new QuicConnection(endpoint, opts);
    g_server_connection = connection->lease();
    return QuicConnection::Lease::adopt(connection);
}

Bytes crypto_bytes_at(QuicConnection &connection, QuicEncryptionLevel level) {
    Bytes out;
    auto &space = connection.packet_number_space(level);
    for (const QuicOutputFrame *f = space.pending_frames.front(); f != nullptr; f = space.pending_frames.next_of(*f)) {
        if (f->type == QuicFrameType::Crypto && f->data && f->u.crypto.offset == out.size()) {
            put(out, f->data.readable_data(), f->data.readable());
        }
    }
    return out;
}

// The in-tree server's answer to `hello`: feeds the ClientHello to a server
// endpoint (as the endpoint harness does, DCID 0xA0.. so the server's
// transport parameters name the ODCID the client harness connects with) and
// lifts the CRYPTO data it queues at the Initial and Handshake levels.
fiber::async::Task<ServerFlight> capture_server_flight(const Bytes &hello) {
    static fiber::net::TlsCredential credential = [] {
        fiber::net::TlsCredentialOptions options{};
        options.certificate_chain = fiber::net::TlsPemSource::from_content(fiber::test::kQuicTestCertificatePem);
        options.private_key = fiber::net::TlsPemSource::from_content(fiber::test::kQuicTestPrivateKeyPem);
        auto created = fiber::net::TlsCredential::create(options);
        FIBER_ASSERT(created.has_value());
        return std::move(*created);
    }();
    static constexpr std::string_view kAlpn[] = {"h3"};
    static fiber::net::TlsServerParam tls{};
    tls.configure_callback = &fiber::net::configure_tls_with_credential;
    tls.configure_ctx = &credential;
    tls.min_version = 0x0304;
    tls.max_version = 0x0304;
    tls.alpn = kAlpn;
    static QuicUdpEndpoint *server = [] {
        auto *ep = new QuicUdpEndpoint(fiber::fuzz::quic_loop());
        QuicUdpEndpoint::EndpointOptions endpoint_options{};
        endpoint_options.bind_addr = fiber::fuzz::loopback(0);
        QuicUdpEndpoint::ServerAdmissionOptions admission{};
        admission.tls = &tls;
        admission.create_connection = create_server_connection;
        auto ok = ep->init(endpoint_options, admission);
        FIBER_ASSERT(ok.has_value());
        return ep;
    }();

    const QuicConnectionId dcid = fiber::fuzz::make_cid(0xA0, 8);
    QuicCryptoState keys{};
    FIBER_ASSERT(quic_init_initial_crypto(keys, QuicConnectionRole::Client, dcid).has_value());
    static std::array<std::uint8_t, kQuicMaxUdpPayloadSize> datagram{};
    const Bytes frame = crypto_frame(0, hello.data(), hello.size());
    const std::size_t len = fiber::fuzz::seal_long_packet(
            QuicPacketType::Initial, kQuicVersion1, dcid, fiber::fuzz::client_scid(), {}, keys.initial_write(), 0,
            frame, kMinInitialDatagramSize, datagram.data(), 0, datagram.size());
    FIBER_ASSERT(len != 0);
    auto received = QuicUdpEndpointTestAccess::receive(*server, datagram.data(), len, fiber::fuzz::loopback(4433),
                                                       fiber::fuzz::loopback(8443), fiber::fuzz::quic_loop().now());
    FIBER_ASSERT(received.has_value() && g_server_connection);
    ServerFlight flight{};
    flight.server_hello = crypto_bytes_at(*g_server_connection, QuicEncryptionLevel::Initial);
    flight.handshake = crypto_bytes_at(*g_server_connection, QuicEncryptionLevel::Handshake);
    flight.scid = g_server_connection->local_connection_id();
    g_server_connection->close_immediately();
    co_await g_server_connection->wait_closed();
    g_server_connection.reset();
    co_return flight;
}

Bytes with_scid(const QuicConnectionId &scid, const Bytes &frames) {
    Bytes b;
    put_u8(b, static_cast<std::uint8_t>(scid.size()));
    put(b, scid.data(), scid.size());
    put(b, frames);
    return b;
}

void client_seeds(const ServerFlight &flight) {
    const char *fz = "quic_client_fuzzer";
    Bytes initial = ack_frame(0, 0);
    put(initial, crypto_frame(0, flight.server_hello.data(), flight.server_hello.size()));
    const Bytes initial_payload = with_scid(flight.scid, initial);
    const Bytes handshake_payload =
            with_scid(flight.scid, crypto_frame(0, flight.handshake.data(), flight.handshake.size()));
    Bytes one_rtt = simple(QuicFrameType::HandshakeDone);
    put(one_rtt, new_cid_frame(1, 0, 0x60));
    put(one_rtt, new_token_frame());
    put(one_rtt, stream_frame(3, 0, "server-uni", false));

    for (std::uint8_t config: {0x00, 0x01, 0x02}) {
        Bytes b{config};
        put(b, ep_record(0x00, 0x00, initial_payload));
        put(b, ep_record(0x01, 0x00, handshake_payload));
        put(b, ep_record(0x02, 0x00, one_rtt));
        write_seed(fz, "flight_c" + std::to_string(config), b);
    }
    {
        // Initial + Handshake coalesced: equal halves, the shorter one
        // padded with PADDING frames, so the harness splits at the boundary.
        Bytes first = initial_payload;
        Bytes second = handshake_payload;
        const std::size_t half = std::max(first.size(), second.size());
        first.resize(half, 0);
        second.resize(half, 0);
        Bytes both = first;
        put(both, second);
        Bytes b{0};
        put(b, ep_record(0x05, 0x04, both));
        put(b, ep_record(0x02, 0x00, one_rtt));
        write_seed(fz, "flight_coalesced", b);
    }
    {
        // Retry, then the flight (whose parameters lack the Retry SCID).
        Bytes b{0};
        const Bytes token{0x52, 0x45, 0x54, 0x52, 0x59, 0x2d, 0x54, 0x4f, 0x4b, 0x45, 0x4e};
        put(b, ep_record(0x03, 0x00, token));
        put(b, ep_record(0x03, 0x08, token)); // a second Retry, bad tag
        put(b, ep_record(0x00, 0x00, initial_payload));
        put(b, ep_record(0x01, 0x00, handshake_payload));
        write_seed(fz, "retry_flight", b);
    }
    {
        // Version Negotiation (v1 listed: must be ignored; then without v1).
        Bytes b{0};
        // flags = the Unused bits of the first byte: 0x40 set, then clear.
        put(b, ep_record(0x04, 0x40, Bytes{0x1a, 0x2a, 0x3a, 0x4a, 0x00, 0x00, 0x00, 0x01}));
        put(b, ep_record(0x04, 0x00, Bytes{0x1a, 0x2a, 0x3a, 0x4a, 0xff, 0x00, 0x00, 0x1d}));
        put(b, ep_record(0x04, 0x40, Bytes{0x1a, 0x2a, 0x3a, 0x4a, 0xff, 0x00, 0x00, 0x1d}));
        write_seed(fz, "version_negotiation", b);
    }
    {
        // Server CID changes between Initials; Handshake from a new address.
        Bytes b{0};
        put(b, ep_record(0x00, 0x00, initial_payload));
        put(b, ep_record(0x00, 0x00, with_scid(fiber::fuzz::make_cid(0x77, 8), simple(QuicFrameType::Ping))));
        put(b, ep_record(0x01, 0x01, handshake_payload));
        put(b, ep_record(0x00, 0x00, with_scid(flight.scid, close_frame(false, 0x0a, "bye"))));
        write_seed(fz, "scid_change", b);
    }
}

// ---------------------------------------------------------------- network

void network_seeds() {
    const char *fz = "quic_network_fuzzer";
    // One round trip: client sends, server receives everything, server
    // sends, client receives everything.
    const Bytes ladder{0xf0, 0x52, 0, 0, 0xf1, 0x53, 0, 0};
    auto ladders = [&](Bytes &b, int n) {
        for (int i = 0; i < n; ++i) {
            put(b, ladder);
        }
    };
    auto inject = [](Bytes &b, std::uint8_t op, const Bytes &frames) {
        put_u8(b, op);
        put_u8(b, static_cast<std::uint8_t>(frames.size()));
        put(b, frames);
    };
    {
        Bytes b{0x00};
        ladders(b, 4);
        put(b, Bytes{0x66, 0x67}); // client: bidi + uni streams
        ladders(b, 2);
        put(b, Bytes{0x47}); // server: read, answer, open uni
        ladders(b, 2);
        put(b, Bytes{0x08, 0x0a}); // client reads, closes
        ladders(b, 2);
        write_seed(fz, "handshake_streams", b);
    }
    {
        Bytes b{0x02}; // server accepts 0-RTT
        ladders(b, 4);
        put(b, Bytes{0x66, 0x47});
        ladders(b, 3);
        put(b, Bytes{0x1c, 0x66, 0x67}); // resume + 0-RTT streams before any reply
        ladders(b, 4);
        put(b, Bytes{0x47, 0x08});
        ladders(b, 2);
        write_seed(fz, "resume_0rtt", b);
    }
    {
        Bytes b{0x01}; // Retry
        ladders(b, 5);
        put(b, Bytes{0x66});
        ladders(b, 2);
        put(b, Bytes{0x3c}); // second connection with the NEW_TOKEN token
        ladders(b, 4);
        write_seed(fz, "retry_token", b);
    }
    {
        Bytes b{0x00};
        put(b, Bytes{0xf0, 0x12, 0, 0, 0x99}); // drop the Initial, PTO
        put(b, Bytes{0xf0, 0x22, 0, 0, 0x52, 0, 0}); // duplicate, deliver
        put(b, Bytes{0xf1, 0x63, 0, 0, 0x33, 0, 7, 0x43, 1, 0}); // reverse, corrupt, truncate
        put(b, Bytes{0x59});
        ladders(b, 4);
        put(b, Bytes{0x66, 0xf0, 0x12, 0, 0, 0x59}); // lose stream data, recover
        ladders(b, 3);
        write_seed(fz, "lossy", b);
    }
    {
        Bytes b{0x00};
        ladders(b, 4);
        put(b, Bytes{0x3d, 0x66}); // NAT rebinding, then data
        ladders(b, 4);
        put(b, Bytes{0x7d, 0x66});
        ladders(b, 3);
        write_seed(fz, "rebinding", b);
    }
    {
        Bytes b{0x00};
        ladders(b, 4);
        Bytes ping_cid = simple(QuicFrameType::Ping);
        put(ping_cid, new_cid_frame(5, 0, 0x44));
        inject(b, 0x34, ping_cid); // client->server 1-RTT
        inject(b, 0x74, simple(QuicFrameType::Ping)); // client->server, next key phase
        inject(b, 0x35, new_token_frame()); // server->client 1-RTT
        inject(b, 0x75, simple(QuicFrameType::Ping)); // server->client, next key phase
        inject(b, 0x24, ack_frame(0, 0)); // Handshake level after confirmation
        ladders(b, 3);
        put(b, Bytes{0x66, 0x47});
        ladders(b, 3);
        write_seed(fz, "inject", b);
    }
}

fiber::async::Task<void> endpoint_seeds() {
    const char *fz = "quic_endpoint_fuzzer";
    const Bytes hello = co_await capture_client_hello();
    const ServerFlight flight = co_await capture_server_flight(hello);
    std::printf("captured server flight: ServerHello %zu bytes, Handshake %zu bytes\n", flight.server_hello.size(),
                flight.handshake.size());
    FIBER_ASSERT(!flight.server_hello.empty() && !flight.handshake.empty());
    client_seeds(flight);
    std::printf("captured ClientHello: %zu bytes\n", hello.size());
    FIBER_ASSERT(!hello.empty());

    // The ClientHello in CRYPTO frames of at most 1000 bytes per Initial.
    auto hello_records = [&](std::uint8_t kind, std::uint8_t flags) {
        Bytes out;
        for (std::size_t off = 0; off < hello.size(); off += 1000) {
            const std::size_t n = std::min<std::size_t>(1000, hello.size() - off);
            put(out, ep_record(kind, flags, crypto_frame(off, hello.data() + off, n)));
        }
        return out;
    };

    // kind: low 3 bits record type, bits 3-4 DCID, bits 5-7 token mode.
    for (std::uint8_t variant = 0; variant < 4; ++variant) {
        for (std::uint8_t token_mode: {0, 1, 2, 3, 4, 5}) {
            Bytes b{variant};
            put(b, hello_records(static_cast<std::uint8_t>(token_mode << 5), 0x00));
            // Then the client's Handshake flight (garbage Finished) and an
            // Initial ACK, as a real client would follow up.
            put(b, ep_record(0x00, 0x00, ack_frame(0, 0)));
            Bytes finished{0x14, 0x00, 0x00, 0x20};
            finished.resize(finished.size() + 32, 0x42);
            put(b, ep_record(0x01, 0x00, crypto_frame(0, finished.data(), finished.size())));
            put(b, ep_record(0x01, 0x00, ack_frame(0, 0)));
            put(b, ep_record(0x02, 0x00, simple(QuicFrameType::Ping)));
            char name[48];
            std::snprintf(name, sizeof(name), "hello_v%u_tok%u", variant, token_mode);
            write_seed(fz, name, b);
        }
    }
    {
        // 20-byte DCID, coalesced Initial+Handshake, then an Initial to the
        // server's own CID, then 1-RTT CONNECTION_CLOSE.
        Bytes b{0};
        put(b, hello_records(0x08, 0x00));
        Bytes both = ack_frame(0, 0);
        put(both, ack_frame(0, 0));
        put(b, ep_record(0x03, 0x00, both));
        put(b, ep_record(0x18, 0x00, simple(QuicFrameType::Ping)));
        put(b, ep_record(0x02, 0x00, close_frame(true, 0x100, "")));
        write_seed(fz, "hello_coalesced", b);
    }
    {
        // Address change mid-handshake, repeated packet numbers, no padding.
        Bytes b{0};
        put(b, hello_records(0x00, 0x00));
        put(b, ep_record(0x00, 0x01, simple(QuicFrameType::Ping)));
        put(b, ep_record(0x00, 0x08, simple(QuicFrameType::Ping)));
        put(b, ep_record(0x00, 0x04, simple(QuicFrameType::Ping)));
        put(b, ep_record(0x00, 0x00, close_frame(false, 0x0a, "x")));
        write_seed(fz, "hello_migrate", b);
    }
    {
        // Stateless paths: unsupported version (VN), unknown short header
        // (stateless reset), too-short DCID, raw garbage, 0-RTT behind Initial.
        Bytes b{0};
        Bytes pad(1100, 0);
        put(b, ep_record(0x05, 0x00, simple(QuicFrameType::Ping)));
        put(b, ep_record(0x06, 0x00, pad));
        put(b, ep_record(0x10, 0x00, simple(QuicFrameType::Ping)));
        put(b, ep_record(0x04, 0x00, Bytes{0xc0, 0x00, 0x00, 0x00, 0x01, 0x08, 1, 2, 3, 4, 5, 6, 7, 8, 0x00}));
        put(b, ep_record(0x07, 0x00, stream_frame(0, 0, "early", true)));
        write_seed(fz, "stateless", b);
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <corpus-dir>\n", argv[0]);
        return 2;
    }
    g_out = argv[1];
    fiber::event::EventLoop &loop = fiber::fuzz::quic_loop();
    (void) fiber::fuzz::quic_host();
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        codec_seeds();
        reassembly_seeds();
        connection_seeds();
        network_seeds();
        co_await endpoint_seeds();
        loop.stop();
    });
    loop.run();
    std::printf("wrote %zu seeds under %s\n", g_written, g_out.c_str());
    return 0;
}
