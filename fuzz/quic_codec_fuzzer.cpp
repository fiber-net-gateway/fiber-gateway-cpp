// Fuzzes the stateless QUIC wire codecs -- the parsing surface every
// datagram crosses before any connection state is touched. Input:
// [selector][bytes]; selector & 0x07 picks the target, the remaining bits
// parameterize it. Besides "no crash", each target checks a round trip
// through the matching encoder, so encoder/decoder drift (a field the
// encoder writes but the parser reads differently, or a length the encoder
// counts differently than it writes) fails here:
//
//   0 frames      bit 3 receiver role (server/client), bits 4-5 encryption
//                 level. Parses the payload frame by frame; each parsed frame
//                 is re-encoded (count mode and write mode must agree) and
//                 re-parsed to the same fields.
//   1 transport parameters   bit 3 owner. parse -> create -> parse must
//                 reproduce every field.
//   2 packet header          bits 3-7 short-header DCID length (mod 21).
//                 Also derives the Version Negotiation reply for a long
//                 header and parses it back.
//   3 address token          validates the bytes as a token under a fixed
//                 key, and checks a freshly minted token over fuzzer fields
//                 validates.
//   4 retry       checks the integrity tag of the bytes as a Retry packet,
//                 then builds a Retry from fuzzer fields, which must parse and
//                 carry a valid tag.
//   5 packet number          truncation/decoding window (RFC 9000 A.3) and
//                 varint round trip.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "QuicFuzzCommon.h"

#include <fiber/quic/QuicToken.h>
#include "quic/QuicCrypto.h"
#include "quic/QuicPacketCodec.h"
#include "quic/QuicTransportCodec.h"
#include "quic/QuicTransportParamsCodec.h"

namespace {

using namespace fiber::quic;
using fiber::fuzz::FuzzInput;
using fiber::fuzz::touch;

bool cid_equal(const QuicConnectionId &a, const QuicConnectionId &b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

bool slice_equal(QuicSlice a, QuicSlice b) {
    return a.len == b.len && (a.len == 0 || std::memcmp(a.data, b.data, a.len) == 0);
}

// The output form of a parsed frame, or false for frames the encoder does
// not produce from a parsed view (STREAM data is encoded by the send path,
// not by quic_create_output_frame).
bool to_output_frame(const QuicInputFrame &in, QuicOutputFrame &out) {
    out.type = in.type;
    switch (in.type) {
        case QuicFrameType::Padding:
            out.u.padding.length = in.u.padding.length;
            return true;
        case QuicFrameType::Ping:
        case QuicFrameType::HandshakeDone:
            return true;
        case QuicFrameType::Ack:
        case QuicFrameType::AckEcn:
            out.u.ack.largest = in.u.ack.largest;
            out.u.ack.delay = in.u.ack.delay;
            out.u.ack.range_count = in.u.ack.range_count;
            out.u.ack.first_range = in.u.ack.first_range;
            out.u.ack.ect0 = in.u.ack.ect0;
            out.u.ack.ect1 = in.u.ack.ect1;
            out.u.ack.ce = in.u.ack.ce;
            return quic_output_frame_set_owned_data(out, in.data.data, in.data.len).has_value();
        case QuicFrameType::Crypto:
            // Keep storage even for empty CRYPTO frames: the encoder requires
            // a valid buffer, while the wire payload may have zero length.
            out.data = fiber::mem::IoBuf::allocate(in.data.len + 1);
            FIBER_ASSERT(out.data.valid());
            if (in.data.len != 0) {
                std::memcpy(out.data.writable_data(), in.data.data, in.data.len);
            }
            out.data.commit(in.data.len);
            out.u.crypto.offset = in.u.crypto.offset;
            return true;
        case QuicFrameType::NewToken:
            return quic_output_frame_set_owned_data(out, in.data.data, in.data.len).has_value();
        case QuicFrameType::ConnectionClose:
        case QuicFrameType::ConnectionCloseApp:
            out.u.close.error_code = in.u.close.error_code;
            out.u.close.frame_type = in.u.close.frame_type;
            return quic_output_frame_set_owned_data(out, in.u.close.reason.data, in.u.close.reason.len).has_value();
        case QuicFrameType::ResetStream:
            out.u.reset_stream = in.u.reset_stream;
            return true;
        case QuicFrameType::StopSending:
            out.u.stop_sending = in.u.stop_sending;
            return true;
        case QuicFrameType::MaxData:
            out.u.max_data = in.u.max_data;
            return true;
        case QuicFrameType::MaxStreamData:
            out.u.max_stream_data = in.u.max_stream_data;
            return true;
        case QuicFrameType::MaxStreamsBidi:
        case QuicFrameType::MaxStreamsUni:
            out.u.max_streams.limit = in.u.max_streams.limit;
            return true;
        case QuicFrameType::DataBlocked:
            out.u.data_blocked = in.u.data_blocked;
            return true;
        case QuicFrameType::StreamDataBlocked:
            out.u.stream_data_blocked = in.u.stream_data_blocked;
            return true;
        case QuicFrameType::StreamsBlockedBidi:
        case QuicFrameType::StreamsBlockedUni:
            out.u.streams_blocked.limit = in.u.streams_blocked.limit;
            return true;
        case QuicFrameType::NewConnectionId:
            out.u.new_connection_id = in.u.new_connection_id;
            return true;
        case QuicFrameType::RetireConnectionId:
            out.u.retire_connection_id = in.u.retire_connection_id;
            return true;
        case QuicFrameType::PathChallenge:
            out.u.path_challenge = in.u.path_challenge;
            return true;
        case QuicFrameType::PathResponse:
            out.u.path_response = in.u.path_response;
            return true;
        default:
            return false;
    }
}

bool same_frame(const QuicInputFrame &a, const QuicInputFrame &b) {
    if (a.type != b.type || a.ack_eliciting != b.ack_eliciting) {
        return false;
    }
    switch (a.type) {
        case QuicFrameType::Padding:
            return a.u.padding.length == b.u.padding.length;
        case QuicFrameType::Ack:
        case QuicFrameType::AckEcn:
            return a.u.ack.largest == b.u.ack.largest && a.u.ack.delay == b.u.ack.delay &&
                   a.u.ack.range_count == b.u.ack.range_count && a.u.ack.first_range == b.u.ack.first_range &&
                   a.u.ack.ect0 == b.u.ack.ect0 && a.u.ack.ect1 == b.u.ack.ect1 && a.u.ack.ce == b.u.ack.ce &&
                   slice_equal(a.data, b.data);
        case QuicFrameType::Crypto:
            return a.u.crypto.offset == b.u.crypto.offset && a.u.crypto.length == b.u.crypto.length &&
                   slice_equal(a.data, b.data);
        case QuicFrameType::NewToken:
            return slice_equal(a.data, b.data);
        case QuicFrameType::ConnectionClose:
        case QuicFrameType::ConnectionCloseApp:
            return a.u.close.error_code == b.u.close.error_code && a.u.close.frame_type == b.u.close.frame_type &&
                   slice_equal(a.u.close.reason, b.u.close.reason);
        case QuicFrameType::ResetStream:
            return a.u.reset_stream.id == b.u.reset_stream.id &&
                   a.u.reset_stream.error_code == b.u.reset_stream.error_code &&
                   a.u.reset_stream.final_size == b.u.reset_stream.final_size;
        case QuicFrameType::StopSending:
            return a.u.stop_sending.id == b.u.stop_sending.id &&
                   a.u.stop_sending.error_code == b.u.stop_sending.error_code;
        case QuicFrameType::MaxData:
            return a.u.max_data.max_data == b.u.max_data.max_data;
        case QuicFrameType::MaxStreamData:
            return a.u.max_stream_data.id == b.u.max_stream_data.id &&
                   a.u.max_stream_data.limit == b.u.max_stream_data.limit;
        case QuicFrameType::MaxStreamsBidi:
        case QuicFrameType::MaxStreamsUni:
            return a.u.max_streams.limit == b.u.max_streams.limit &&
                   a.u.max_streams.bidirectional == b.u.max_streams.bidirectional;
        case QuicFrameType::DataBlocked:
            return a.u.data_blocked.limit == b.u.data_blocked.limit;
        case QuicFrameType::StreamDataBlocked:
            return a.u.stream_data_blocked.id == b.u.stream_data_blocked.id &&
                   a.u.stream_data_blocked.limit == b.u.stream_data_blocked.limit;
        case QuicFrameType::StreamsBlockedBidi:
        case QuicFrameType::StreamsBlockedUni:
            return a.u.streams_blocked.limit == b.u.streams_blocked.limit &&
                   a.u.streams_blocked.bidirectional == b.u.streams_blocked.bidirectional;
        case QuicFrameType::NewConnectionId:
            return std::memcmp(&a.u.new_connection_id, &b.u.new_connection_id, sizeof(a.u.new_connection_id)) == 0;
        case QuicFrameType::RetireConnectionId:
            return a.u.retire_connection_id.sequence_number == b.u.retire_connection_id.sequence_number;
        case QuicFrameType::PathChallenge:
            return std::memcmp(a.u.path_challenge.data, b.u.path_challenge.data, 8) == 0;
        case QuicFrameType::PathResponse:
            return std::memcmp(a.u.path_response.data, b.u.path_response.data, 8) == 0;
        default:
            return true;
    }
}

void frames(std::uint8_t selector, const std::uint8_t *data, std::size_t size) {
    const QuicConnectionRole role = (selector & 0x08) != 0 ? QuicConnectionRole::Client : QuicConnectionRole::Server;
    const auto level = static_cast<QuicEncryptionLevel>((selector >> 4) & 0x03);
    static std::array<std::uint8_t, 1 << 17> encoded{};

    QuicReadCursor payload(data, size);
    while (!payload.empty()) {
        const std::size_t before = payload.offset();
        auto parsed = quic_parse_frame_for_receiver(role, level, payload);
        if (!parsed) {
            return;
        }
        const QuicInputFrame &frame = parsed->frame;
        FIBER_FUZZ_CHECK(parsed->consumed == payload.offset() - before);
        FIBER_FUZZ_CHECK(parsed->consumed != 0);
        FIBER_FUZZ_CHECK(quic_frame_allowed_for_receiver(role, level, frame.type));
        touch(frame.data);
        if (frame.type == QuicFrameType::ConnectionClose || frame.type == QuicFrameType::ConnectionCloseApp) {
            touch(frame.u.close.reason);
        }

        QuicOutputFrame out{};
        if (!to_output_frame(frame, out)) {
            continue;
        }
        auto counted = quic_create_output_frame(nullptr, out);
        if (!counted) {
            // Only padding/len overflow style limits may refuse a parsed frame.
            continue;
        }
        if (*counted > encoded.size()) {
            continue;
        }
        QuicWriteCursor writer(encoded.data(), encoded.size());
        auto written = quic_create_output_frame(&writer, out);
        FIBER_FUZZ_CHECK(written.has_value());
        FIBER_FUZZ_CHECK(*written == *counted);
        FIBER_FUZZ_CHECK(writer.offset() == *written);

        QuicReadCursor reparse_in(encoded.data(), *written);
        auto reparsed = quic_parse_frame_for_receiver(role, level, reparse_in);
        FIBER_FUZZ_CHECK(reparsed.has_value());
        FIBER_FUZZ_CHECK(reparsed->consumed == *written);
        FIBER_FUZZ_CHECK(same_frame(frame, reparsed->frame));
    }
}

bool same_params(const QuicTransportParams &a, const QuicTransportParams &b) {
    bool same = a.max_idle_timeout == b.max_idle_timeout && a.max_udp_payload_size == b.max_udp_payload_size &&
                a.initial_max_data == b.initial_max_data &&
                a.initial_max_stream_data_bidi_local == b.initial_max_stream_data_bidi_local &&
                a.initial_max_stream_data_bidi_remote == b.initial_max_stream_data_bidi_remote &&
                a.initial_max_stream_data_uni == b.initial_max_stream_data_uni &&
                a.initial_max_streams_bidi == b.initial_max_streams_bidi &&
                a.initial_max_streams_uni == b.initial_max_streams_uni &&
                a.ack_delay_exponent == b.ack_delay_exponent && a.max_ack_delay == b.max_ack_delay &&
                a.active_connection_id_limit == b.active_connection_id_limit &&
                a.disable_active_migration == b.disable_active_migration &&
                a.has_original_destination_connection_id == b.has_original_destination_connection_id &&
                a.has_initial_source_connection_id == b.has_initial_source_connection_id &&
                a.has_retry_source_connection_id == b.has_retry_source_connection_id &&
                a.has_stateless_reset_token == b.has_stateless_reset_token &&
                a.has_preferred_address == b.has_preferred_address;
    if (!same) {
        return false;
    }
    if (a.has_original_destination_connection_id &&
        !cid_equal(a.original_destination_connection_id, b.original_destination_connection_id)) {
        return false;
    }
    if (a.has_initial_source_connection_id &&
        !cid_equal(a.initial_source_connection_id, b.initial_source_connection_id)) {
        return false;
    }
    if (a.has_retry_source_connection_id && !cid_equal(a.retry_source_connection_id, b.retry_source_connection_id)) {
        return false;
    }
    if (a.has_stateless_reset_token &&
        std::memcmp(a.stateless_reset_token, b.stateless_reset_token, kStatelessResetTokenLength) != 0) {
        return false;
    }
    if (a.has_preferred_address) {
        const QuicPreferredAddress &pa = a.preferred_address;
        const QuicPreferredAddress &pb = b.preferred_address;
        if (!cid_equal(pa.connection_id, pb.connection_id) ||
            std::memcmp(pa.stateless_reset_token, pb.stateless_reset_token, kStatelessResetTokenLength) != 0 ||
            pa.ipv4.port() != pb.ipv4.port() || pa.ipv6.port() != pb.ipv6.port() ||
            pa.ipv4.ip().v4_bytes() != pb.ipv4.ip().v4_bytes() || pa.ipv6.ip().v6_bytes() != pb.ipv6.ip().v6_bytes()) {
            return false;
        }
    }
    return true;
}

void transport_params(std::uint8_t selector, const std::uint8_t *data, std::size_t size) {
    const QuicTransportParamOwner owner =
            (selector & 0x08) != 0 ? QuicTransportParamOwner::Server : QuicTransportParamOwner::Client;
    QuicReadCursor in(data, size);
    QuicTransportParams params{};
    if (!quic_parse_transport_params(owner, in, params)) {
        return;
    }
    touch(params.original_destination_connection_id.data(), params.original_destination_connection_id.size());
    touch(params.initial_source_connection_id.data(), params.initial_source_connection_id.size());

    auto counted = quic_create_transport_params(owner, nullptr, params);
    FIBER_FUZZ_CHECK(counted.has_value());
    std::array<std::uint8_t, 1024> encoded{};
    FIBER_FUZZ_CHECK(*counted <= encoded.size());
    QuicWriteCursor writer(encoded.data(), encoded.size());
    std::size_t zero_rtt_len = 0;
    auto written = quic_create_transport_params(owner, &writer, params, &zero_rtt_len);
    FIBER_FUZZ_CHECK(written.has_value());
    FIBER_FUZZ_CHECK(*written == *counted && writer.offset() == *written);
    FIBER_FUZZ_CHECK(zero_rtt_len <= *written);

    QuicReadCursor reparse_in(encoded.data(), *written);
    QuicTransportParams reparsed{};
    FIBER_FUZZ_CHECK(quic_parse_transport_params(owner, reparse_in, reparsed).has_value());
    FIBER_FUZZ_CHECK(same_params(params, reparsed));
}

void packet_header(std::uint8_t selector, const std::uint8_t *data, std::size_t size) {
    const auto short_dcid_len = static_cast<std::uint8_t>((selector >> 3) % (kMaxConnectionIdLength + 1));
    auto dcid = quic_get_packet_dcid(data, size, short_dcid_len);
    auto packet = quic_parse_packet_header(data, size, short_dcid_len);
    if (!packet) {
        return;
    }
    if (dcid) {
        FIBER_FUZZ_CHECK(cid_equal(*dcid, packet->dcid));
    }
    FIBER_FUZZ_CHECK(packet->packet_data == data);
    FIBER_FUZZ_CHECK(packet->packet_len <= size);
    touch(packet->dcid.data(), packet->dcid.size());
    touch(packet->scid.data(), packet->scid.size());
    touch(packet->token);
    touch(packet->version_list);
    if (packet->ciphertext != nullptr) {
        FIBER_FUZZ_CHECK(packet->ciphertext >= data && packet->ciphertext + packet->ciphertext_len <= data + size);
        touch(packet->ciphertext, packet->ciphertext_len);
    }

    if (!packet->long_header) {
        return;
    }
    // The endpoint answers an unsupported version with VN built from the
    // parsed header: it must parse back with the CIDs swapped.
    QuicPacketHeader request{};
    request.long_header = true;
    request.type = QuicPacketType::VersionNegotiation;
    request.flags = kPacketFlagLong | kPacketFlagFixed;
    request.dcid = packet->scid;
    request.scid = packet->dcid;
    std::array<std::uint8_t, 256> out{};
    QuicWriteCursor writer(out.data(), out.size());
    auto vn = quic_create_version_negotiation_packet(request, writer);
    if (!vn) {
        return;
    }
    auto reply = quic_parse_packet_header(out.data(), *vn, 0);
    FIBER_FUZZ_CHECK(reply.has_value());
    FIBER_FUZZ_CHECK(reply->type == QuicPacketType::VersionNegotiation);
    FIBER_FUZZ_CHECK(cid_equal(reply->dcid, packet->scid) && cid_equal(reply->scid, packet->dcid));
    bool lists_v1 = false;
    for (std::size_t i = 0; i + 4 <= reply->version_list.len; i += 4) {
        const std::uint8_t *v = reply->version_list.data + i;
        lists_v1 = lists_v1 || (v[0] == 0 && v[1] == 0 && v[2] == 0 && v[3] == 1);
    }
    FIBER_FUZZ_CHECK(lists_v1);
}

using fiber::fuzz::kTokenKey;
constexpr std::uint64_t kTokenNow = 1'800'000'000;

void address_token(const std::uint8_t *data, std::size_t size) {
    FuzzInput in(data, size);
    const auto peer = fiber::fuzz::loopback(in.u16());
    const std::uint64_t now = kTokenNow + (in.u8() & 0x7f);
    const std::span<const std::uint8_t> token = in.bytes(in.remaining());

    auto checked = quic_validate_address_token(kTokenKey, peer, now, {token.data(), token.size()});
    if (checked && checked->status == QuicAddressTokenValidationStatus::Valid) {
        touch(checked->original_destination_connection_id.data(), checked->original_destination_connection_id.size());
    }

    // A minted token validates for its peer, kind and ODCID until it expires.
    const auto kind = (size & 1) != 0 ? QuicAddressTokenKind::Retry : QuicAddressTokenKind::NewToken;
    const QuicConnectionId odcid = fiber::fuzz::make_cid(static_cast<std::uint8_t>(size), size % 21);
    const bool with_odcid = kind == QuicAddressTokenKind::Retry && !odcid.empty();
    auto minted = quic_create_address_token(kTokenKey, peer, now + 10, kind, with_odcid ? &odcid : nullptr);
    if (!minted) {
        return;
    }
    auto valid = quic_validate_address_token(kTokenKey, peer, now, minted->slice());
    FIBER_FUZZ_CHECK(valid.has_value());
    FIBER_FUZZ_CHECK(valid->status == QuicAddressTokenValidationStatus::Valid);
    FIBER_FUZZ_CHECK(valid->kind == kind);
    if (with_odcid) {
        FIBER_FUZZ_CHECK(cid_equal(valid->original_destination_connection_id, odcid));
    }
}

void retry(const std::uint8_t *data, std::size_t size) {
    FuzzInput in(data, size);
    const QuicConnectionId odcid = fiber::fuzz::make_cid(in.u8(), in.u8() % 21);
    const QuicConnectionId dcid = fiber::fuzz::make_cid(in.u8(), in.u8() % 21);
    const QuicConnectionId scid = fiber::fuzz::make_cid(in.u8(), in.u8() % 21);
    const std::span<const std::uint8_t> rest = in.bytes(in.remaining());

    // The bytes as a Retry: validating the tag must stay in bounds.
    (void) quic_validate_retry_integrity_tag(odcid, rest.data(), rest.size());
    auto packet = quic_parse_packet_header(rest.data(), rest.size(), 0);
    if (packet && packet->type == QuicPacketType::Retry) {
        touch(packet->token);
    }

    // A Retry we build from fuzzer fields parses back and carries a good tag.
    QuicRetryPacketSpec spec{};
    spec.original_dcid = odcid;
    spec.dcid = dcid;
    spec.scid = scid;
    spec.token = {rest.data(), std::min<std::size_t>(rest.size(), 512)};
    std::array<std::uint8_t, 1024> out{};
    QuicWriteCursor writer(out.data(), out.size());
    auto built = quic_create_retry_packet(spec, writer);
    if (!built) {
        return;
    }
    auto parsed = quic_parse_packet_header(out.data(), *built, 0);
    FIBER_FUZZ_CHECK(parsed.has_value());
    FIBER_FUZZ_CHECK(parsed->type == QuicPacketType::Retry);
    FIBER_FUZZ_CHECK(cid_equal(parsed->dcid, dcid) && cid_equal(parsed->scid, scid));
    FIBER_FUZZ_CHECK(slice_equal(parsed->token, spec.token));
    auto tag = quic_validate_retry_integrity_tag(odcid, out.data(), *built);
    FIBER_FUZZ_CHECK(tag.has_value() && *tag);
}

void packet_number(const std::uint8_t *data, std::size_t size) {
    FuzzInput in(data, size);
    const std::uint8_t pn_len = static_cast<std::uint8_t>(1 + (in.u8() & 0x03));
    const std::uint64_t largest_raw = (static_cast<std::uint64_t>(in.u32()) << 32U) | in.u32();
    const std::uint64_t largest = (in.u8() & 1) != 0 ? kUnsetPacketNumber : largest_raw & kMaxVarint;
    const std::uint32_t truncated = quic_truncate_packet_number(in.u32(), pn_len);

    auto decoded = quic_decode_packet_number(truncated, pn_len, largest);
    FIBER_FUZZ_CHECK(decoded.has_value());
    // Whatever the window does, the low bits are the truncated number.
    FIBER_FUZZ_CHECK(quic_truncate_packet_number(*decoded, pn_len) == truncated);
    // Away from the top of the number space the candidate lies within half a
    // window of the expected packet number (RFC 9000 A.3).
    const std::uint64_t expected = largest == kUnsetPacketNumber ? 0 : largest + 1;
    const std::uint64_t hwin = (1ULL << (pn_len * 8U)) / 2;
    if (expected < kMaxVarint - (1ULL << 33U) && expected >= hwin) {
        FIBER_FUZZ_CHECK(*decoded + hwin > expected && *decoded <= expected + hwin);
    }

    // Varint round trip of the remaining bytes.
    const std::span<const std::uint8_t> rest = in.bytes(in.remaining());
    QuicReadCursor reader(rest.data(), rest.size());
    while (!reader.empty()) {
        const std::size_t before = reader.offset();
        auto value = quic_parse_varint(reader);
        if (!value) {
            break;
        }
        FIBER_FUZZ_CHECK(*value <= kMaxVarint);
        FIBER_FUZZ_CHECK(quic_varint_len(*value) <= reader.offset() - before);
        std::array<std::uint8_t, 8> buf{};
        QuicWriteCursor writer(buf.data(), buf.size());
        FIBER_FUZZ_CHECK(quic_write_varint(writer, *value).has_value());
        FIBER_FUZZ_CHECK(writer.offset() == quic_varint_len(*value));
        QuicReadCursor back(buf.data(), writer.offset());
        auto again = quic_parse_varint(back);
        FIBER_FUZZ_CHECK(again.has_value() && *again == *value);
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size < 1) {
        return 0;
    }
    const std::uint8_t selector = data[0];
    const std::uint8_t *body = data + 1;
    const std::size_t body_size = size - 1;
    switch (selector & 0x07) {
        case 0:
            frames(selector, body, body_size);
            break;
        case 1:
            transport_params(selector, body, body_size);
            break;
        case 2:
            packet_header(selector, body, body_size);
            break;
        case 3:
            address_token(body, body_size);
            break;
        case 4:
            retry(body, body_size);
            break;
        default:
            packet_number(body, body_size);
            break;
    }
    return 0;
}
