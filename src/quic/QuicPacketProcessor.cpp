#include "QuicPacketProcessor.h"

#include <algorithm>
#include <expected>

#include "QuicAckHandler.h"
#include "QuicCrypto.h"
#include "QuicCursor.h"
#include "QuicPacketCodec.h"
#include "QuicPacketNumberSpace.h"
#include "QuicTransportCodec.h"

namespace fiber::quic {

namespace {

[[nodiscard]] bool ip_address_equal(const net::IpAddress &lhs, const net::IpAddress &rhs) noexcept {
    if (lhs.family() != rhs.family()) {
        return false;
    }
    if (lhs.is_v4()) {
        return lhs.v4_bytes() == rhs.v4_bytes();
    }
    return lhs.scope_id() == rhs.scope_id() && lhs.v6_bytes() == rhs.v6_bytes();
}

[[nodiscard]] bool socket_address_equal(const net::SocketAddress &lhs, const net::SocketAddress &rhs) noexcept {
    return lhs.port() == rhs.port() && ip_address_equal(lhs.ip(), rhs.ip());
}

[[nodiscard]] bool connection_id_equal(const QuicConnectionId &lhs, const QuicConnectionId &rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    if (lhs.size() == 0) {
        return true;
    }
    return std::equal(lhs.data(), lhs.data() + lhs.size(), rhs.data());
}

[[nodiscard]] bool probing_frame(QuicFrameType type) noexcept {
    return type == QuicFrameType::Padding || type == QuicFrameType::PathChallenge ||
           type == QuicFrameType::PathResponse || type == QuicFrameType::NewConnectionId;
}

struct FrameParseClose {
    QuicErrorCode error = QuicErrorCode::FrameEncodingError;
    std::uint64_t frame_type = 0;
};

[[nodiscard]] FrameParseClose classify_frame_parse_failure(QuicConnectionRole role, QuicEncryptionLevel level,
                                                           const std::uint8_t *frame_data,
                                                           std::size_t frame_len) noexcept {
    QuicReadCursor type_reader(frame_data, frame_len);
    auto type_value = quic_parse_varint(type_reader);
    if (!type_value) {
        return {};
    }

    FrameParseClose close{};
    close.frame_type = *type_value;
    if (*type_value <= kLastFrameType &&
        !quic_frame_allowed_for_receiver(role, level, static_cast<QuicFrameType>(*type_value))) {
        close.error = QuicErrorCode::ProtocolViolation;
    }
    return close;
}

// Apply a peer-initiated key update after a packet was successfully decrypted
// with next_application_read. The current application_read/write become the
// "previous" generation (kept for a 3×PTO grace period), next_application_read/write
// become the new current keys, and the next-next pair is immediately pre-derived.
// RFC 9001 §6.1, §6.5.
[[nodiscard]] common::IoResult<void> apply_key_update(QuicConnection &conn) noexcept {
    QuicCryptoState &crypto = conn.crypto();

    // 1. Save current application_read as previous (grace period).
    //    previous starts empty (or held the prior-prior generation that has
    //    already passed the discard timer); resetting first guarantees we
    //    drop any stale AEAD context before swapping.
    crypto.previous_application_read.reset();
    crypto.previous_application_read.swap(crypto.application_read);
    crypto.previous_application_keys_ready = true;

    // 2. Promote next_application_read → application_read.
    crypto.application_read.swap(crypto.next_application_read);
    crypto.next_application_read.reset();

    // 3. Promote next_application_write → application_write (writes have no
    //    grace-period retention; the old write context is simply discarded).
    crypto.application_write.swap(crypto.next_application_write);
    crypto.next_application_write.reset();

    crypto.next_application_keys_ready = false;

    // 4. Flip the key phase bit so subsequent outbound short headers carry
    //    the new phase.
    conn.flip_key_phase();

    // 5. Derive the next-next generation immediately so the connection can
    //    accept another update without deriving on the fly.
    auto derived = quic_derive_next_key_pair(crypto);
    if (!derived) {
        conn.close(QuicErrorCode::InternalError);
        return std::unexpected(derived.error());
    }

    // 6. Arm the grace-period timer to discard the previous application keys.
    conn.arm_key_update_discard_timer();

    return {};
}

[[nodiscard]] common::IoResult<QuicPath *> bind_datagram_path(QuicConnection &conn,
                                                              const QuicReceivedDatagram &datagram,
                                                              const QuicPacketHeader &packet,
                                                              std::size_t received_len) noexcept {
    if (QuicPath *path = conn.find_path(datagram.peer, datagram.local)) {
        conn.record_path_received(*path, received_len);
        return path;
    }

    auto &space = conn.packet_number_space(packet.level);
    if (packet.packet_number != space.largest_received_packet_number) {
        return std::unexpected(common::IoErr::Invalid);
    }

    if (QuicPath *probe = conn.find_path(QuicPathTag::Probe)) {
        conn.free_path(*probe);
    }

    QuicConnectionId remote_connection_id = packet.scid;
    if (remote_connection_id.empty()) {
        if (QuicPath *active = conn.active_path()) {
            remote_connection_id = active->remote_connection_id;
        }
    }

    QuicPath *path = conn.create_path(datagram.peer, datagram.local, remote_connection_id, QuicPathTag::Probe);
    if (path == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    conn.record_path_received(*path, received_len);
    return path;
}

void discard_frame_queue(QuicConnection &conn, QuicPacketNumberSpace &space, QuicOutputFrameQueue &queue) noexcept {
    (void) conn;
    while (QuicOutputFrame *frame = queue.pop_front()) {
        space.release_frame(*frame);
    }
}

void discard_packet_number_space(QuicConnection &conn, QuicEncryptionLevel level) noexcept {
    QuicPacketNumberSpace &space = conn.packet_number_space(level);
    conn.reset_pto_count();
    discard_frame_queue(conn, space, space.pending_frames);
    discard_frame_queue(conn, space, space.sending_frames);
    discard_frame_queue(conn, space, space.sent_frames);
    if (space.ack_frame.queued) {
        space.pending_frames.erase(space.ack_frame);
    }
    space.send_ack = false;
    space.send_ack_count = 0;
    space.pending_ack = kUnsetPacketNumber;
    space.crypto_recv.clear();

    if (QuicPacketProtectionKeys *read = quic_packet_keys(conn.crypto(), level, false)) {
        read->reset();
    }
    if (QuicPacketProtectionKeys *write = quic_packet_keys(conn.crypto(), level, true)) {
        write->reset();
    }
}

[[nodiscard]] common::IoResult<void> queue_handshake_done(QuicConnection &conn) noexcept {
    if (conn.role() != QuicConnectionRole::Server) {
        return {};
    }

    QuicPacketNumberSpace &space = conn.packet_number_space(QuicEncryptionLevel::Application);
    QuicOutputFrame *frame = space.alloc_frame();
    if (frame == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    frame->type = QuicFrameType::HandshakeDone;
    space.pending_frames.push_back(*frame);
    return {};
}

[[nodiscard]] common::IoResult<void> provide_crypto_data(QuicConnection &conn, QuicEncryptionLevel level,
                                                         std::uint64_t offset, QuicSlice data) noexcept {
    if (!conn.tls().initialized()) {
        return {};
    }

    QuicPacketNumberSpace &space = conn.packet_number_space(level);
    auto inserted = space.crypto_recv.insert(offset, data);
    if (!inserted) {
        if (inserted.error() == common::IoErr::MessageTooLarge) {
            conn.close(QuicErrorCode::CryptoBufferExceeded);
        }
        return std::unexpected(inserted.error());
    }

    mem::IoBufChain contiguous(conn.recv_extent_pool());
    auto taken = space.crypto_recv.take_contiguous(contiguous);
    if (!taken) {
        return std::unexpected(taken.error());
    }

    while (mem::IoBufNode *node = contiguous.pop_front_node()) {
        auto provided = conn.tls().provide_crypto_data(level, node->buf.readable_data(), node->buf.readable());
        conn.recv_extent_pool().release(node);
        if (!provided) {
            return std::unexpected(provided.error());
        }
    }

    return {};
}

[[nodiscard]] common::IoResult<bool> handle_crypto_frame(QuicConnection &conn, QuicEncryptionLevel level,
                                                         const QuicInputFrame &frame,
                                                         bool &handshake_confirmed) noexcept {
    auto provided = provide_crypto_data(conn, level, frame.u.crypto.offset, frame.data);
    if (!provided) {
        return std::unexpected(provided.error());
    }

    if (!conn.tls().initialized()) {
        return false;
    }

    auto driven = conn.tls().drive_handshake();
    if (!driven && driven.error() != common::IoErr::WouldBlock) {
        return std::unexpected(driven.error());
    }

    if (conn.tls().handshake_done() && conn.state() != QuicConnectionState::Established) {
        auto established = conn.mark_established();
        if (!established) {
            return std::unexpected(established.error());
        }
        auto queued = queue_handshake_done(conn);
        if (!queued) {
            return std::unexpected(queued.error());
        }
        discard_packet_number_space(conn, QuicEncryptionLevel::Handshake);
        // RFC 9001 §9.5 / nginx ngx_event_quic_ssl.c — pre-derive the first
        // next-generation application keys so the connection can immediately
        // respond to a peer-initiated key update without deriving on the fly
        // (which would also leak a timing side channel).
        if (conn.crypto().application_read.ready && conn.crypto().application_write.ready) {
            auto derived = quic_derive_next_key_pair(conn.crypto());
            if (!derived) {
                return std::unexpected(derived.error());
            }
        }
        handshake_confirmed = true;
        return true;
    }

    return !conn.packet_number_space(level).pending_frames.empty() ||
           !conn.packet_number_space(QuicEncryptionLevel::Handshake).pending_frames.empty() ||
           !conn.packet_number_space(QuicEncryptionLevel::Application).pending_frames.empty();
}

[[nodiscard]] common::IoResult<void> handle_ack_eliciting_packet(QuicPacketNumberSpace &space,
                                                                 const QuicPacketHeader &packet,
                                                                 const QuicReceivedDatagram &datagram,
                                                                 std::uint64_t previous_largest_received,
                                                                 bool ack_eliciting) noexcept {
    if (!ack_eliciting) {
        return {};
    }

    const QuicTime now = quic_time_ms(datagram.received_at);
    quic_record_ack_eliciting_received(space, packet.packet_number, now, previous_largest_received);
    return {};
}

[[nodiscard]] common::IoResult<QuicPacketProcessResult>
process_decoded_packet(QuicConnection &conn, const QuicReceivedDatagram &datagram,
                       const QuicPacketDecodeResult &decoded, std::size_t received_len,
                       std::uint64_t previous_largest_received) noexcept {
    const QuicPacketHeader &packet = decoded.header;
    QuicPacketNumberSpace &space = conn.packet_number_space(packet.level);

    auto bound_path = bind_datagram_path(conn, datagram, packet, received_len);
    if (!bound_path) {
        return std::unexpected(bound_path.error());
    }

    QuicPacketProcessResult result{};
    result.path = *bound_path;
    result.created_path = result.path->tag == QuicPathTag::Probe;
    result.rebound =
            result.created_path && (conn.has_active_local_connection_id(packet.dcid) ||
                                    connection_id_equal(packet.dcid, conn.original_destination_connection_id()));
    result.packet_type = packet.type;
    result.level = packet.level;
    result.packet_number = packet.packet_number;
    result.packet_count = 1;

    // RFC 9000 §10.2.2: in the Closing state, received packets only prompt a
    // CONNECTION_CLOSE frame on the same encryption level. We skip the normal
    // frame dispatch — there is no point processing stream data or flow-control
    // updates when the connection is being torn down. ACK frames are also not
    // processed, matching nginx's behaviour. Rate limiting is handled inside
    // requeue_close_frame (1s interval).
    if (conn.state() == QuicConnectionState::Closing) {
        conn.requeue_close_frame(packet.level);
        return result;
    }
    // RFC 9000 §10.2.2: in the Draining state we silently discard packets.
    if (conn.state() == QuicConnectionState::Draining || conn.state() == QuicConnectionState::Closed) {
        return result;
    }

    const QuicTime now = quic_time_ms(datagram.received_at);
    bool path_challenged = false;
    QuicReadCursor payload(decoded.payload.data, decoded.payload.len);
    while (!payload.empty()) {
        const std::uint8_t *frame_data = payload.pos();
        const std::size_t frame_len = payload.remaining();
        auto parsed = quic_parse_frame_for_receiver(conn.role(), packet.level, payload);
        if (!parsed) {
            // Initial payloads were fully prevalidated by quic_decode_packet so
            // this branch is reached only for strongly protected packets in
            // normal operation. The peer authenticated the malformed frame;
            // terminate the connection instead of silently discarding it.
            const FrameParseClose close =
                    classify_frame_parse_failure(conn.role(), packet.level, frame_data, frame_len);
            conn.close(close.error, close.frame_type);
            return std::unexpected(parsed.error());
        }

        QuicInputFrame &frame = parsed->frame;
        ++result.frame_count;
        result.ack_eliciting = result.ack_eliciting || frame.ack_eliciting;
        result.non_probing = result.non_probing || !probing_frame(frame.type);

        switch (frame.type) {
            case QuicFrameType::Ack:
            case QuicFrameType::AckEcn: {
                const QuicPeerTransportState &peer = conn.peer_transport();
                auto acked = quic_handle_ack_frame(conn, packet.level, frame, quic_time_ms(datagram.received_at),
                                                   peer.params.ack_delay_exponent, peer.params.max_ack_delay,
                                                   conn.state() == QuicConnectionState::Established);
                if (!acked) {
                    return std::unexpected(acked.error());
                }
                result.send_output = result.send_output || acked->unblocked || acked->lost_frames || acked->force_send;
                break;
            }
            case QuicFrameType::Padding:
            case QuicFrameType::Ping:
                break;
            case QuicFrameType::ConnectionClose:
            case QuicFrameType::ConnectionCloseApp:
                conn.begin_draining(QuicCloseInfo{
                        .source = QuicCloseSource::PeerConnectionClose,
                        .frame_kind = frame.type == QuicFrameType::ConnectionCloseApp ? QuicCloseFrameKind::Application
                                                                                      : QuicCloseFrameKind::Transport,
                        .error_code = frame.u.close.error_code,
                        .frame_type = frame.u.close.frame_type,
                });
                return result;
            case QuicFrameType::Crypto: {
                bool handshake_confirmed = false;
                auto handled = handle_crypto_frame(conn, packet.level, frame, handshake_confirmed);
                if (!handled) {
                    return std::unexpected(handled.error());
                }
                result.send_output = result.send_output || *handled;
                result.handshake_confirmed = result.handshake_confirmed || handshake_confirmed;
                break;
            }
            case QuicFrameType::Stream: {
                auto received = conn.recv_stream_frame(frame.u.stream, frame.data);
                if (!received) {
                    return std::unexpected(received.error());
                }
                break;
            }
            case QuicFrameType::ResetStream: {
                auto reset = conn.recv_reset_stream_frame(frame.u.reset_stream);
                if (!reset) {
                    return std::unexpected(reset.error());
                }
                break;
            }
            case QuicFrameType::StopSending: {
                auto stopped = conn.recv_stop_sending_frame(frame.u.stop_sending);
                if (!stopped) {
                    return std::unexpected(stopped.error());
                }
                break;
            }
            case QuicFrameType::MaxStreamData: {
                auto updated = conn.recv_max_stream_data_frame(frame.u.max_stream_data);
                if (!updated) {
                    return std::unexpected(updated.error());
                }
                break;
            }
            case QuicFrameType::MaxData: {
                auto updated = conn.recv_max_data_frame(frame.u.max_data);
                if (!updated) {
                    return std::unexpected(updated.error());
                }
                break;
            }
            case QuicFrameType::StreamDataBlocked:
                // RFC 9000 §4.1: STREAM_DATA_BLOCKED is sent by the stream
                // sender. On a unidirectional stream the local side initiated,
                // the peer is the receiver and MUST NOT send it →
                // STREAM_STATE_ERROR (nginx
                // ngx_quic_handle_stream_data_blocked_frame).
                if (conn.is_local_stream(frame.u.stream_data_blocked.id) &&
                    conn.is_unidirectional_stream(frame.u.stream_data_blocked.id)) {
                    conn.close(QuicErrorCode::StreamStateError,
                               static_cast<std::uint64_t>(QuicFrameType::StreamDataBlocked));
                    return std::unexpected(common::IoErr::Busy);
                }
                if (conn.find_stream(frame.u.stream_data_blocked.id) == nullptr) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                break;
            case QuicFrameType::MaxStreamsBidi:
            case QuicFrameType::MaxStreamsUni: {
                auto updated = conn.recv_max_streams_frame(frame.u.max_streams);
                if (!updated) {
                    return std::unexpected(updated.error());
                }
                break;
            }
            case QuicFrameType::DataBlocked:
                break;
            case QuicFrameType::StreamsBlockedBidi:
            case QuicFrameType::StreamsBlockedUni: {
                auto blocked = conn.recv_streams_blocked_frame(frame.u.streams_blocked);
                if (!blocked) {
                    return std::unexpected(blocked.error());
                }
                break;
            }
            case QuicFrameType::NewToken:
            case QuicFrameType::HandshakeDone:
            case QuicFrameType::Stream1:
            case QuicFrameType::Stream2:
            case QuicFrameType::Stream3:
            case QuicFrameType::Stream4:
            case QuicFrameType::Stream5:
            case QuicFrameType::Stream6:
            case QuicFrameType::Stream7:
                break;
            case QuicFrameType::NewConnectionId: {
                auto received = conn.recv_new_connection_id_frame(frame.u.new_connection_id);
                if (!received) {
                    return std::unexpected(received.error());
                }
                result.send_output = result.send_output || *received;
                break;
            }
            case QuicFrameType::RetireConnectionId: {
                auto retired = conn.recv_retire_connection_id_frame(frame.u.retire_connection_id, packet.dcid);
                if (!retired) {
                    return std::unexpected(retired.error());
                }
                result.send_output = result.send_output || *retired;
                break;
            }
            case QuicFrameType::PathChallenge: {
                if (packet.level != QuicEncryptionLevel::Application || path_challenged || result.path == nullptr) {
                    break;
                }
                path_challenged = true;
                auto handled = conn.recv_path_challenge_frame(*result.path, frame.u.path_challenge);
                if (!handled) {
                    return std::unexpected(handled.error());
                }
                result.send_output = true;
                break;
            }
            case QuicFrameType::PathResponse: {
                if (packet.level != QuicEncryptionLevel::Application) {
                    break;
                }
                auto validated_path = conn.recv_path_response_frame_with_path(frame.u.path_response, now);
                if (!validated_path) {
                    return std::unexpected(validated_path.error());
                }
                if (*validated_path != nullptr) {
                    result.validated_path = *validated_path;
                    result.path_validated = true;
                    result.send_output = true;
                }
                break;
            }
        }
    }

    // Track ACK ranges for this received packet.
    // on_packet_received handles forced ACK creation internally when range
    // overflow or too-old conditions occur (mirrors ngx_quic_ack_packet).
    space.on_packet_received(packet.packet_number, now, result.ack_eliciting, datagram.ecn);

    auto acked = handle_ack_eliciting_packet(space, packet, datagram, previous_largest_received, result.ack_eliciting);
    if (!acked) {
        return std::unexpected(acked.error());
    }
    result.send_ack = space.send_ack;

    if (conn.state() == QuicConnectionState::Init && packet.level == QuicEncryptionLevel::Initial) {
        auto started = conn.start_handshake();
        if (!started) {
            return std::unexpected(started.error());
        }
    }
    if (packet.level == QuicEncryptionLevel::Handshake) {
        discard_packet_number_space(conn, QuicEncryptionLevel::Initial);
        if (result.path != nullptr) {
            result.path->validated = true;
        }
    }

    if (result.path != conn.active_path() && result.non_probing &&
        packet.packet_number == space.largest_received_packet_number) {
        if (packet.level == QuicEncryptionLevel::Application) {
            auto migrated = conn.handle_migration(*result.path, result.rebound, now);
            if (!migrated) {
                return std::unexpected(migrated.error());
            }
            result.send_output = true;
        } else {
            QuicPath *old = conn.active_path();
            if (old != nullptr && !old->validated) {
                conn.free_path(*old);
            }
            if (!conn.set_active_path(*result.path)) {
                return std::unexpected(common::IoErr::Invalid);
            }
        }
    }

    // RFC 9001 §6.1 — apply key update only after the packet decoded cleanly
    // (frames parsed, no protocol violations). Doing this last guarantees we
    // never rotate keys for a packet that ends up being rejected.
    if (decoded.key_update) {
        auto applied = apply_key_update(conn);
        if (!applied) {
            return std::unexpected(applied.error());
        }
        result.send_output = true;
    }

    return result;
}

} // namespace

QuicReceiveApplyResult quic_apply_receive_result(QuicConnection &conn, const QuicPacketProcessResult &result) noexcept {
    (void) conn;

    QuicReceiveApplyResult applied{};
    if (result.handshake_confirmed && result.path != nullptr) {
        result.path->validated = true;
        applied.handshake_validated_path = result.path;
    }
    return applied;
}

common::IoResult<QuicPacketProcessResult> quic_process_initial_datagram(QuicConnection &conn,
                                                                        const QuicReceivedDatagram &datagram,
                                                                        std::uint8_t *plaintext,
                                                                        std::size_t plaintext_cap) noexcept {
    if (conn.role() != QuicConnectionRole::Server || datagram.data == nullptr ||
        datagram.len < kMinInitialDatagramSize || plaintext == nullptr || plaintext_cap == 0) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (conn.closed()) {
        return std::unexpected(common::IoErr::Canceled);
    }

    auto packet = quic_parse_packet_header(datagram.data, datagram.len, 0);
    if (!packet) {
        return std::unexpected(packet.error());
    }
    if (!packet->long_header || packet->type != QuicPacketType::Initial ||
        packet->level != QuicEncryptionLevel::Initial || packet->version != kQuicVersion1) {
        return std::unexpected(common::IoErr::Invalid);
    }

    if (!conn.crypto().initial_ready) {
        auto initialized = conn.init_initial_crypto(packet->dcid);
        if (!initialized) {
            return std::unexpected(initialized.error());
        }
    }

    auto &space = conn.packet_number_space(packet->level);
    const std::uint64_t previous_largest_received = space.largest_received_packet_number;

    auto opened =
            quic_decrypt_initial_packet(conn, *packet, datagram.data, packet->packet_len, plaintext, plaintext_cap);
    if (!opened) {
        return std::unexpected(opened.error());
    }

    if (conn.role() == QuicConnectionRole::Server && !conn.tls().initialized()) {
        auto tls_ok = conn.ensure_server_tls();
        if (!tls_ok) {
            return std::unexpected(tls_ok.error());
        }
    }

    auto bound_path = bind_datagram_path(conn, datagram, *packet, datagram.len);
    if (!bound_path) {
        return std::unexpected(bound_path.error());
    }

    QuicPacketProcessResult result{};
    result.path = *bound_path;
    result.created_path = result.path->tag == QuicPathTag::Probe;
    result.rebound =
            result.created_path && (conn.has_active_local_connection_id(packet->dcid) ||
                                    connection_id_equal(packet->dcid, conn.original_destination_connection_id()));
    result.packet_type = packet->type;
    result.level = packet->level;
    result.packet_number = packet->packet_number;

    QuicReadCursor payload(opened->data, opened->len);
    while (!payload.empty()) {
        auto parsed = quic_parse_frame(QuicEncryptionLevel::Initial, payload);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        if (parsed->frame.type == QuicFrameType::Ack || parsed->frame.type == QuicFrameType::AckEcn) {
            auto acked = quic_handle_ack_frame(conn, packet->level, parsed->frame, quic_time_ms(datagram.received_at));
            if (!acked) {
                return std::unexpected(acked.error());
            }
            result.send_output = result.send_output || acked->unblocked || acked->lost_frames || acked->force_send;
        }
        ++result.frame_count;
        result.ack_eliciting = result.ack_eliciting || parsed->frame.ack_eliciting;
        result.non_probing = result.non_probing || !probing_frame(parsed->frame.type);
    }

    if (conn.state() == QuicConnectionState::Init) {
        auto started = conn.start_handshake();
        if (!started) {
            return std::unexpected(started.error());
        }
    }

    if (result.ack_eliciting) {
        const QuicTime now = quic_time_ms(datagram.received_at);
        quic_record_ack_eliciting_received(space, packet->packet_number, now, previous_largest_received);
    }

    // Track ACK ranges for every valid received packet.
    // on_packet_received handles forced ACK creation internally when range
    // overflow or too-old conditions occur (mirrors ngx_quic_ack_packet).
    const QuicTime received_time = quic_time_ms(datagram.received_at);
    space.on_packet_received(packet->packet_number, received_time, result.ack_eliciting, datagram.ecn);
    result.send_ack = space.send_ack;

    if (result.path != conn.active_path() && result.non_probing &&
        packet->packet_number == space.largest_received_packet_number) {
        QuicPath *old = conn.active_path();
        if (old != nullptr && !old->validated) {
            conn.free_path(*old);
        }
        if (!conn.set_active_path(*result.path)) {
            return std::unexpected(common::IoErr::Invalid);
        }
    }

    return result;
}

common::IoResult<QuicPacketProcessResult> quic_process_datagram(QuicConnection &conn,
                                                                const QuicReceivedDatagram &datagram,
                                                                std::uint8_t *plaintext, std::size_t plaintext_cap,
                                                                std::uint8_t short_dcid_len) noexcept {
    if (datagram.data == nullptr || datagram.len == 0 || plaintext == nullptr || plaintext_cap == 0 || conn.closed()) {
        return std::unexpected(conn.closed() ? common::IoErr::Canceled : common::IoErr::Invalid);
    }

    QuicPacketProcessResult aggregate{};
    bool has_good_packet = false;
    bool recorded_datagram_bytes = false;
    std::size_t offset = 0;

    while (offset < datagram.len) {
        std::uint8_t *packet_data = datagram.data + offset;
        const std::size_t remaining = datagram.len - offset;
        auto packet = quic_parse_packet_header(packet_data, remaining, short_dcid_len);
        if (!packet) {
            if (has_good_packet) {
                return aggregate;
            }
            return std::unexpected(packet.error());
        }

        if (packet->long_header && packet->version != kQuicVersion1) {
            if (has_good_packet) {
                return aggregate;
            }
            return std::unexpected(common::IoErr::NotSupported);
        }
        if (packet->type == QuicPacketType::Retry || packet->type == QuicPacketType::VersionNegotiation) {
            if (has_good_packet) {
                return aggregate;
            }
            return std::unexpected(common::IoErr::Invalid);
        }
        if (packet->packet_len == 0 || packet->packet_len > remaining) {
            if (has_good_packet) {
                return aggregate;
            }
            return std::unexpected(common::IoErr::Invalid);
        }

        if (packet->level == QuicEncryptionLevel::Initial && !conn.crypto().initial_ready) {
            auto initialized = conn.init_initial_crypto(packet->dcid);
            if (!initialized) {
                if (has_good_packet) {
                    return aggregate;
                }
                return std::unexpected(initialized.error());
            }
        }

        QuicPacketNumberSpace &space = conn.packet_number_space(packet->level);
        const std::uint64_t previous_largest_received = space.largest_received_packet_number;
        auto decoded =
                quic_decode_packet(conn, packet_data, packet->packet_len, short_dcid_len, plaintext, plaintext_cap);
        if (!decoded) {
            if (decoded.error() == common::IoErr::NotFound) {
                offset += packet->packet_len;
                continue;
            }
            // RFC 9000 §10.3: a short-header (1-RTT) packet that fails to
            // decrypt may be a stateless reset. A stateless reset occupies an
            // entire datagram, so only the first packet of a datagram can be
            // one; a short header can never be coalesced behind another packet,
            // so for the first packet packet_len spans the whole datagram and
            // its trailing 16 bytes are the datagram's trailing 16 bytes. If
            // they match a stateless_reset_token the peer advertised, enter the
            // DRAINING state SILENTLY — no CONNECTION_CLOSE is sent (§10.2.2:
            // the peer already lost state, so a close frame is pointless).
            if (offset == 0 && !packet->long_header && packet->level == QuicEncryptionLevel::Application &&
                conn.detects_stateless_reset(packet_data, packet->packet_len)) {
                conn.begin_draining(QuicCloseInfo{
                        .source = QuicCloseSource::StatelessReset,
                        .frame_kind = QuicCloseFrameKind::Transport,
                        .error_code = static_cast<std::uint64_t>(QuicErrorCode::NoError),
                        .frame_type = 0,
                });
                return aggregate;
            }
            if (has_good_packet) {
                return aggregate;
            }
            return std::unexpected(decoded.error());
        }

        // AEAD authentication just succeeded for this packet. Now is the
        // earliest safe point to create the server SSL object (mirrors nginx
        // ngx_quic_init_connection after ngx_quic_decrypt). Forged packets
        // fail quic_decode_packet above and never reach here, so they no
        // longer pay the SSL_new cost.
        if (conn.role() == QuicConnectionRole::Server && !conn.tls().initialized()) {
            auto tls_ok = conn.ensure_server_tls();
            if (!tls_ok) {
                if (has_good_packet) {
                    return aggregate;
                }
                return std::unexpected(tls_ok.error());
            }
        }

        const std::size_t received_len = recorded_datagram_bytes ? 0 : datagram.len;
        auto processed = process_decoded_packet(conn, datagram, *decoded, received_len, previous_largest_received);
        if (!processed) {
            return std::unexpected(processed.error());
        }
        recorded_datagram_bytes = true;
        has_good_packet = true;

        aggregate.path = processed->path;
        aggregate.packet_type = processed->packet_type;
        aggregate.level = processed->level;
        aggregate.packet_number = processed->packet_number;
        aggregate.packet_count += processed->packet_count;
        aggregate.frame_count += processed->frame_count;
        aggregate.ack_eliciting = aggregate.ack_eliciting || processed->ack_eliciting;
        aggregate.send_ack = aggregate.send_ack || processed->send_ack;
        aggregate.send_output = aggregate.send_output || processed->send_output;
        aggregate.non_probing = aggregate.non_probing || processed->non_probing;
        aggregate.created_path = aggregate.created_path || processed->created_path;
        aggregate.rebound = aggregate.rebound || processed->rebound;
        aggregate.handshake_confirmed = aggregate.handshake_confirmed || processed->handshake_confirmed;
        aggregate.path_validated = aggregate.path_validated || processed->path_validated;
        if (processed->validated_path != nullptr) {
            aggregate.validated_path = processed->validated_path;
        }

        offset += packet->packet_len;
    }

    if (!has_good_packet) {
        return std::unexpected(common::IoErr::NotFound);
    }
    return aggregate;
}

} // namespace fiber::quic
