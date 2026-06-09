#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <future>
#include <string_view>

#include "async/Spawn.h"
#include "common/IoError.h"
#include "event/EventLoopGroup.h"
#include "net/IpAddress.h"
#include "net/SocketAddress.h"
#include "net/UdpSocket.h"
#include "quic/QuicCrypto.h"
#include "quic/QuicPacketCodec.h"
#include "quic/QuicTransportCodec.h"
#include "quic/QuicUdpEndpoint.h"

namespace {

using DetachedTask = fiber::async::DetachedTask;
using EndpointResult = fiber::common::IoResult<fiber::quic::QuicUdpReceiveResult>;

struct DecodedPacketSummary {
    fiber::quic::QuicPacketType type = fiber::quic::QuicPacketType::Initial;
    std::uint32_t frame_count = 0;
    bool ack_eliciting = false;
};

struct CoalescedPacketSummary {
    std::size_t datagram_len = 0;
    std::size_t first_packet_len = 0;
    fiber::quic::QuicPacketType first_type = fiber::quic::QuicPacketType::Initial;
    fiber::quic::QuicPacketType second_type = fiber::quic::QuicPacketType::Handshake;
    bool first_ack_eliciting = false;
    bool second_ack_eliciting = false;
};

struct ManyFramePacketSummary {
    std::uint32_t decoded_frame_count = 0;
    std::size_t sent_frame_count = 0;
    bool pending_empty = false;
    bool sending_empty = false;
};

struct SplitFramePacketSummary {
    std::uint32_t decoded_frame_count = 0;
    std::size_t sent_crypto_len = 0;
    std::size_t pending_crypto_len = 0;
    bool pending_owned = false;
    bool sending_empty = false;
    std::size_t in_flight = 0;
};

struct AckOnlyPacketSummary {
    std::size_t datagram_len = 0;
    std::uint32_t decoded_frame_count = 0;
    bool ack_eliciting = true;
    bool data_pending = false;
    bool sending_empty = false;
    std::size_t in_flight = 0;
};

struct TwoEndpointResults {
    EndpointResult first;
    EndpointResult second;
};

struct TwoEndpointResultsWithPorts {
    TwoEndpointResults results{};
    std::array<std::uint16_t, 2> ports{};
};

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

fiber::quic::QuicConnectionId cid_from_hex(std::string_view value) {
    std::array<std::uint8_t, fiber::quic::kMaxConnectionIdLength> bytes{};
    std::size_t len = 0;
    int high = -1;
    for (char c: value) {
        const int v = hex_value(c);
        if (v < 0) {
            continue;
        }
        if (high < 0) {
            high = v;
        } else {
            bytes[len++] = static_cast<std::uint8_t>((high << 4U) | v);
            high = -1;
        }
    }
    EXPECT_LT(high, 0);
    auto cid = fiber::quic::QuicConnectionId::from_bytes(bytes.data(), len);
    EXPECT_TRUE(cid.has_value());
    return cid.value_or(fiber::quic::QuicConnectionId{});
}

fiber::net::SocketAddress loopback(std::uint16_t port) { return {fiber::net::IpAddress::loopback_v4(), port}; }

void build_initial_datagram(std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> &datagram,
                            const fiber::quic::QuicConnectionId &dcid, const fiber::quic::QuicConnectionId &scid,
                            std::uint64_t packet_number = 1) {
    datagram = {};

    fiber::quic::QuicOutputFrame frame{};
    frame.type = fiber::quic::QuicFrameType::Ping;

    std::array<std::uint8_t, 16> payload{};
    fiber::quic::QuicWriteCursor payload_out(payload.data(), payload.size());
    auto payload_len = fiber::quic::quic_create_output_frame(&payload_out, frame);
    ASSERT_TRUE(payload_len.has_value());

    fiber::quic::QuicCryptoState crypto{};
    ASSERT_TRUE(fiber::quic::quic_init_initial_crypto(crypto, fiber::quic::QuicConnectionRole::Server, dcid));

    fiber::quic::QuicPacketHeader packet{};
    packet.long_header = true;
    packet.type = fiber::quic::QuicPacketType::Initial;
    packet.level = fiber::quic::QuicEncryptionLevel::Initial;
    packet.flags =
            fiber::quic::kPacketFlagLong | fiber::quic::kPacketFlagFixed | fiber::quic::kLongPacketTypeInitial | 0x03;
    packet.version = fiber::quic::kQuicVersion1;
    packet.dcid = dcid;
    packet.scid = scid;
    packet.length = 4 + payload_out.offset() + fiber::quic::kAeadTagLength;
    packet.pn_len = 4;
    packet.packet_number = packet_number;
    packet.truncated_pn = static_cast<std::uint32_t>(packet_number);

    fiber::quic::QuicWriteCursor out(datagram.data(), datagram.size());
    std::uint8_t *pn = nullptr;
    auto header_len = fiber::quic::quic_create_packet_header(out, packet, &pn);
    ASSERT_TRUE(header_len.has_value());
    ASSERT_NE(pn, nullptr);

    packet.packet_data = datagram.data();
    packet.packet_len = *header_len + payload_out.offset() + fiber::quic::kAeadTagLength;
    packet.protected_pn = pn;
    packet.ciphertext = pn + packet.pn_len;
    packet.ciphertext_len = payload_out.offset() + fiber::quic::kAeadTagLength;

    auto sealed = fiber::quic::quic_encrypt_packet_payload(
            packet, crypto.initial_read, payload.data(), payload_out.offset(), pn + packet.pn_len,
            datagram.size() - static_cast<std::size_t>(pn + packet.pn_len - datagram.data()));
    ASSERT_TRUE(sealed.has_value());
    packet.packet_len = static_cast<std::size_t>(pn + packet.pn_len - datagram.data()) + *sealed;
    ASSERT_TRUE(
            fiber::quic::quic_apply_header_protection(packet, crypto.initial_read, datagram.data(), packet.packet_len));
}

DetachedTask recv_endpoint_once(fiber::quic::QuicUdpEndpoint *endpoint, std::promise<EndpointResult> *done_promise) {
    done_promise->set_value(co_await endpoint->recv_once());
}

DetachedTask recv_endpoint_twice(fiber::quic::QuicUdpEndpoint *endpoint,
                                 std::promise<TwoEndpointResults> *done_promise) {
    TwoEndpointResults results{co_await endpoint->recv_once(), co_await endpoint->recv_once()};
    done_promise->set_value(results);
}

DetachedTask send_datagram(fiber::event::EventLoop *loop, std::uint16_t port, const std::uint8_t *data, std::size_t len,
                           std::promise<fiber::common::IoErr> *done_promise) {
    fiber::net::UdpSocket client(*loop);
    auto bound = client.bind(loopback(0), {});
    if (!bound) {
        done_promise->set_value(bound.error());
        co_return;
    }

    auto sent = co_await client.send_to(data, len, loopback(port));
    done_promise->set_value(sent ? fiber::common::IoErr::None : sent.error());
    client.close();
}

DetachedTask send_datagram_and_recv_response(fiber::event::EventLoop *loop, std::uint16_t port,
                                             const std::uint8_t *data, std::size_t len, std::uint8_t *response,
                                             std::size_t response_cap,
                                             std::promise<fiber::common::IoResult<std::size_t>> *done_promise) {
    fiber::net::UdpSocket client(*loop);
    auto bound = client.bind(loopback(0), {});
    if (!bound) {
        done_promise->set_value(std::unexpected(bound.error()));
        co_return;
    }

    auto sent = co_await client.send_to(data, len, loopback(port));
    if (!sent) {
        done_promise->set_value(std::unexpected(sent.error()));
        client.close();
        co_return;
    }

    auto received = co_await client.recv_from(response, response_cap);
    done_promise->set_value(received ? fiber::common::IoResult<std::size_t>{received->size}
                                     : std::unexpected(received.error()));
    client.close();
}

DetachedTask send_two_datagrams(fiber::event::EventLoop *loop, std::uint16_t port, const std::uint8_t *first,
                                std::size_t first_len, const std::uint8_t *second, std::size_t second_len,
                                std::promise<fiber::common::IoErr> *done_promise) {
    fiber::net::UdpSocket client(*loop);
    auto bound = client.bind(loopback(0), {});
    if (!bound) {
        done_promise->set_value(bound.error());
        co_return;
    }

    auto first_sent = co_await client.send_to(first, first_len, loopback(port));
    if (!first_sent) {
        done_promise->set_value(first_sent.error());
        client.close();
        co_return;
    }

    auto second_sent = co_await client.send_to(second, second_len, loopback(port));
    done_promise->set_value(second_sent ? fiber::common::IoErr::None : second_sent.error());
    client.close();
}

DetachedTask send_two_datagrams_from_distinct_clients(
        fiber::event::EventLoop *loop, std::uint16_t port, const std::uint8_t *first, std::size_t first_len,
        const std::uint8_t *second, std::size_t second_len,
        std::promise<fiber::common::IoResult<std::array<std::uint16_t, 2>>> *done_promise) {
    fiber::net::UdpSocket first_client(*loop);
    auto first_bound = first_client.bind(loopback(0), {});
    if (!first_bound) {
        done_promise->set_value(std::unexpected(first_bound.error()));
        co_return;
    }

    fiber::net::UdpSocket second_client(*loop);
    auto second_bound = second_client.bind(loopback(0), {});
    if (!second_bound) {
        done_promise->set_value(std::unexpected(second_bound.error()));
        first_client.close();
        co_return;
    }

    std::array<std::uint16_t, 2> ports{first_client.local_addr().port(), second_client.local_addr().port()};

    auto first_sent = co_await first_client.send_to(first, first_len, loopback(port));
    if (!first_sent) {
        done_promise->set_value(std::unexpected(first_sent.error()));
        first_client.close();
        second_client.close();
        co_return;
    }

    auto second_sent = co_await second_client.send_to(second, second_len, loopback(port));
    done_promise->set_value(second_sent ? fiber::common::IoResult<std::array<std::uint16_t, 2>>{ports}
                                        : std::unexpected(second_sent.error()));
    first_client.close();
    second_client.close();
}

DetachedTask close_endpoint(fiber::quic::QuicUdpEndpoint *endpoint, std::promise<void> *done_promise) {
    endpoint->close();
    done_promise->set_value();
    co_return;
}

DetachedTask recv_delayed_application_ack(fiber::event::EventLoop *loop, fiber::quic::QuicUdpEndpoint *endpoint,
                                          std::promise<fiber::common::IoResult<DecodedPacketSummary>> *done_promise) {
    fiber::net::UdpSocket client(*loop);
    auto bound = client.bind(loopback(0), {});
    if (!bound) {
        done_promise->set_value(std::unexpected(bound.error()));
        co_return;
    }

    const auto server_cid = cid_from_hex("0102030405060708");
    const auto client_cid = cid_from_hex("1112131415161718");
    constexpr fiber::quic::QuicCryptoSuite suite = fiber::quic::QuicCryptoSuite::Aes128GcmSha256;
    std::array<std::uint8_t, fiber::quic::kQuicMaxSecretLength> secret{};
    for (std::size_t i = 0; i < 32; ++i) {
        secret[i] = static_cast<std::uint8_t>(i + 1);
    }

    fiber::quic::QuicConnection::Options server_options{};
    server_options.role = fiber::quic::QuicConnectionRole::Server;
    server_options.local_addr = endpoint->local_addr();
    server_options.remote_addr = client.local_addr();
    server_options.local_connection_id = server_cid;
    server_options.remote_connection_id = client_cid;
    fiber::quic::QuicConnection server(server_options);
    auto *path = server.active_path();
    if (path == nullptr) {
        done_promise->set_value(std::unexpected(fiber::common::IoErr::Invalid));
        co_return;
    }
    path->validated = true;

    auto server_secret = fiber::quic::quic_set_encryption_secret(
            server.crypto(), fiber::quic::QuicEncryptionLevel::Application, true, suite, secret.data(), 32);
    if (!server_secret) {
        done_promise->set_value(std::unexpected(server_secret.error()));
        co_return;
    }

    fiber::quic::QuicConnection::Options client_options{};
    client_options.role = fiber::quic::QuicConnectionRole::Client;
    fiber::quic::QuicConnection peer(client_options);
    auto client_secret = fiber::quic::quic_set_encryption_secret(
            peer.crypto(), fiber::quic::QuicEncryptionLevel::Application, false, suite, secret.data(), 32);
    if (!client_secret) {
        done_promise->set_value(std::unexpected(client_secret.error()));
        co_return;
    }

    const auto now = fiber::quic::quic_time_ms(loop->now());
    auto &space = server.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);
    space.largest_received_packet_number = 3;
    space.pending_ack = 3;
    space.largest_received_time = now;
    space.ack_delay_start = now;
    space.send_ack_count = 1;
    space.send_ack = true;

    endpoint->schedule_send_after(server, std::chrono::milliseconds{5});

    auto readable = co_await client.wait_event(fiber::event::IoEvent::Read, std::chrono::milliseconds{500});
    if (!readable) {
        done_promise->set_value(std::unexpected(readable.error()));
        co_return;
    }

    std::array<std::uint8_t, 1400> response{};
    auto received = client.try_recv_from(response.data(), response.size());
    if (!received) {
        done_promise->set_value(std::unexpected(received.error()));
        co_return;
    }

    std::array<std::uint8_t, 256> plaintext{};
    auto decoded = fiber::quic::quic_decode_packet(peer, response.data(), received->size,
                                                   static_cast<std::uint8_t>(client_cid.size()), plaintext.data(),
                                                   plaintext.size());
    if (!decoded) {
        done_promise->set_value(std::unexpected(decoded.error()));
        co_return;
    }

    done_promise->set_value(DecodedPacketSummary{decoded->header.type, decoded->frame_count, decoded->ack_eliciting});
    client.close();
}

DetachedTask
recv_coalesced_initial_handshake(fiber::event::EventLoop *loop, fiber::quic::QuicUdpEndpoint *endpoint,
                                 std::promise<fiber::common::IoResult<CoalescedPacketSummary>> *done_promise) {
    fiber::net::UdpSocket client(*loop);
    auto bound = client.bind(loopback(0), {});
    if (!bound) {
        done_promise->set_value(std::unexpected(bound.error()));
        co_return;
    }

    const auto original_dcid = cid_from_hex("8394c8f03e515708");
    const auto server_cid = cid_from_hex("0102030405060708");
    const auto client_cid = cid_from_hex("1112131415161718");
    constexpr fiber::quic::QuicCryptoSuite suite = fiber::quic::QuicCryptoSuite::Aes128GcmSha256;
    std::array<std::uint8_t, fiber::quic::kQuicMaxSecretLength> handshake_secret{};
    for (std::size_t i = 0; i < 32; ++i) {
        handshake_secret[i] = static_cast<std::uint8_t>(0x80U + i);
    }

    fiber::quic::QuicConnection::Options server_options{};
    server_options.role = fiber::quic::QuicConnectionRole::Server;
    server_options.local_addr = endpoint->local_addr();
    server_options.remote_addr = client.local_addr();
    server_options.local_connection_id = server_cid;
    server_options.remote_connection_id = client_cid;
    fiber::quic::QuicConnection server(server_options);
    auto *path = server.active_path();
    if (path == nullptr) {
        done_promise->set_value(std::unexpected(fiber::common::IoErr::Invalid));
        co_return;
    }
    path->validated = true;

    auto initial_ready = server.init_initial_crypto(original_dcid);
    if (!initial_ready) {
        done_promise->set_value(std::unexpected(initial_ready.error()));
        co_return;
    }
    auto server_handshake_secret = fiber::quic::quic_set_encryption_secret(
            server.crypto(), fiber::quic::QuicEncryptionLevel::Handshake, true, suite, handshake_secret.data(), 32);
    if (!server_handshake_secret) {
        done_promise->set_value(std::unexpected(server_handshake_secret.error()));
        co_return;
    }

    auto &initial_space = server.packet_number_space(fiber::quic::QuicEncryptionLevel::Initial);
    fiber::quic::QuicOutputFrame *initial_ping = initial_space.alloc_frame();
    if (initial_ping == nullptr) {
        done_promise->set_value(std::unexpected(fiber::common::IoErr::NoMem));
        co_return;
    }
    initial_ping->type = fiber::quic::QuicFrameType::Ping;
    initial_space.pending_frames.push_back(*initial_ping);

    auto &handshake_space = server.packet_number_space(fiber::quic::QuicEncryptionLevel::Handshake);
    fiber::quic::QuicOutputFrame *handshake_ping = handshake_space.alloc_frame();
    if (handshake_ping == nullptr) {
        done_promise->set_value(std::unexpected(fiber::common::IoErr::NoMem));
        co_return;
    }
    handshake_ping->type = fiber::quic::QuicFrameType::Ping;
    handshake_space.pending_frames.push_back(*handshake_ping);

    fiber::quic::QuicConnection::Options client_options{};
    client_options.role = fiber::quic::QuicConnectionRole::Client;
    fiber::quic::QuicConnection peer(client_options);
    auto peer_initial_ready = peer.init_initial_crypto(original_dcid);
    if (!peer_initial_ready) {
        done_promise->set_value(std::unexpected(peer_initial_ready.error()));
        co_return;
    }
    auto peer_handshake_secret = fiber::quic::quic_set_encryption_secret(
            peer.crypto(), fiber::quic::QuicEncryptionLevel::Handshake, false, suite, handshake_secret.data(), 32);
    if (!peer_handshake_secret) {
        done_promise->set_value(std::unexpected(peer_handshake_secret.error()));
        co_return;
    }

    endpoint->schedule_send(server);

    auto readable = co_await client.wait_event(fiber::event::IoEvent::Read, std::chrono::milliseconds{500});
    if (!readable) {
        done_promise->set_value(std::unexpected(readable.error()));
        co_return;
    }

    std::array<std::uint8_t, 1400> response{};
    auto received = client.try_recv_from(response.data(), response.size());
    if (!received) {
        done_promise->set_value(std::unexpected(received.error()));
        co_return;
    }

    std::array<std::uint8_t, 1400> plaintext{};
    auto first = fiber::quic::quic_decode_packet(peer, response.data(), received->size, 0, plaintext.data(),
                                                 plaintext.size());
    if (!first) {
        done_promise->set_value(std::unexpected(first.error()));
        co_return;
    }

    const std::size_t second_offset = first->header.packet_len;
    if (second_offset >= received->size) {
        done_promise->set_value(std::unexpected(fiber::common::IoErr::Invalid));
        co_return;
    }

    auto second = fiber::quic::quic_decode_packet(peer, response.data() + second_offset, received->size - second_offset,
                                                  0, plaintext.data(), plaintext.size());
    if (!second) {
        done_promise->set_value(std::unexpected(second.error()));
        co_return;
    }

    done_promise->set_value(CoalescedPacketSummary{received->size, first->header.packet_len, first->header.type,
                                                   second->header.type, first->ack_eliciting, second->ack_eliciting});
    client.close();
}

DetachedTask
recv_handshake_packet_with_many_frames(fiber::event::EventLoop *loop, fiber::quic::QuicUdpEndpoint *endpoint,
                                       std::promise<fiber::common::IoResult<ManyFramePacketSummary>> *done_promise) {
    fiber::net::UdpSocket client(*loop);
    auto bound = client.bind(loopback(0), {});
    if (!bound) {
        done_promise->set_value(std::unexpected(bound.error()));
        co_return;
    }

    const auto server_cid = cid_from_hex("2223242526272829");
    const auto client_cid = cid_from_hex("3132333435363738");
    constexpr fiber::quic::QuicCryptoSuite suite = fiber::quic::QuicCryptoSuite::Aes128GcmSha256;
    std::array<std::uint8_t, fiber::quic::kQuicMaxSecretLength> handshake_secret{};
    for (std::size_t i = 0; i < 32; ++i) {
        handshake_secret[i] = static_cast<std::uint8_t>(0x40U + i);
    }

    fiber::quic::QuicConnection::Options server_options{};
    server_options.role = fiber::quic::QuicConnectionRole::Server;
    server_options.local_addr = endpoint->local_addr();
    server_options.remote_addr = client.local_addr();
    server_options.local_connection_id = server_cid;
    server_options.remote_connection_id = client_cid;
    fiber::quic::QuicConnection server(server_options);
    auto *path = server.active_path();
    if (path == nullptr) {
        done_promise->set_value(std::unexpected(fiber::common::IoErr::Invalid));
        co_return;
    }
    path->validated = true;

    auto server_secret = fiber::quic::quic_set_encryption_secret(
            server.crypto(), fiber::quic::QuicEncryptionLevel::Handshake, true, suite, handshake_secret.data(), 32);
    if (!server_secret) {
        done_promise->set_value(std::unexpected(server_secret.error()));
        co_return;
    }

    auto &space = server.packet_number_space(fiber::quic::QuicEncryptionLevel::Handshake);
    for (std::size_t i = 0; i < 12; ++i) {
        fiber::quic::QuicOutputFrame *frame = space.alloc_frame();
        if (frame == nullptr) {
            done_promise->set_value(std::unexpected(fiber::common::IoErr::NoMem));
            co_return;
        }
        frame->type = fiber::quic::QuicFrameType::Ping;
        space.pending_frames.push_back(*frame);
    }

    fiber::quic::QuicConnection::Options client_options{};
    client_options.role = fiber::quic::QuicConnectionRole::Client;
    fiber::quic::QuicConnection peer(client_options);
    auto peer_secret = fiber::quic::quic_set_encryption_secret(
            peer.crypto(), fiber::quic::QuicEncryptionLevel::Handshake, false, suite, handshake_secret.data(), 32);
    if (!peer_secret) {
        done_promise->set_value(std::unexpected(peer_secret.error()));
        co_return;
    }

    endpoint->schedule_send(server);

    auto readable = co_await client.wait_event(fiber::event::IoEvent::Read, std::chrono::milliseconds{500});
    if (!readable) {
        done_promise->set_value(std::unexpected(readable.error()));
        co_return;
    }

    std::array<std::uint8_t, 1400> response{};
    auto received = client.try_recv_from(response.data(), response.size());
    if (!received) {
        done_promise->set_value(std::unexpected(received.error()));
        co_return;
    }

    std::array<std::uint8_t, 256> plaintext{};
    auto decoded = fiber::quic::quic_decode_packet(peer, response.data(), received->size, 0, plaintext.data(),
                                                   plaintext.size());
    if (!decoded) {
        done_promise->set_value(std::unexpected(decoded.error()));
        co_return;
    }

    std::size_t sent_count = 0;
    for (fiber::quic::QuicOutputFrame *frame = space.sent_frames.front(); frame != nullptr;
         frame = space.sent_frames.next_of(*frame)) {
        ++sent_count;
    }

    done_promise->set_value(ManyFramePacketSummary{decoded->frame_count, sent_count, space.pending_frames.empty(),
                                                   space.sending_frames.empty()});
    client.close();
}

DetachedTask
recv_split_handshake_crypto_frame(fiber::event::EventLoop *loop, fiber::quic::QuicUdpEndpoint *endpoint,
                                  std::promise<fiber::common::IoResult<SplitFramePacketSummary>> *done_promise) {
    fiber::net::UdpSocket client(*loop);
    auto bound = client.bind(loopback(0), {});
    if (!bound) {
        done_promise->set_value(std::unexpected(bound.error()));
        co_return;
    }

    const auto server_cid = cid_from_hex("5152535455565758");
    const auto client_cid = cid_from_hex("6162636465666768");
    constexpr fiber::quic::QuicCryptoSuite suite = fiber::quic::QuicCryptoSuite::Aes128GcmSha256;
    std::array<std::uint8_t, fiber::quic::kQuicMaxSecretLength> handshake_secret{};
    for (std::size_t i = 0; i < 32; ++i) {
        handshake_secret[i] = static_cast<std::uint8_t>(0x20U + i);
    }

    fiber::quic::QuicConnection::Options server_options{};
    server_options.role = fiber::quic::QuicConnectionRole::Server;
    server_options.local_addr = endpoint->local_addr();
    server_options.remote_addr = client.local_addr();
    server_options.local_connection_id = server_cid;
    server_options.remote_connection_id = client_cid;
    fiber::quic::QuicConnection server(server_options);
    auto *path = server.active_path();
    if (path == nullptr) {
        done_promise->set_value(std::unexpected(fiber::common::IoErr::Invalid));
        co_return;
    }
    path->validated = true;
    server.congestion().window = fiber::quic::kQuicCongestionMinInitialSize;

    auto server_secret = fiber::quic::quic_set_encryption_secret(
            server.crypto(), fiber::quic::QuicEncryptionLevel::Handshake, true, suite, handshake_secret.data(), 32);
    if (!server_secret) {
        done_promise->set_value(std::unexpected(server_secret.error()));
        co_return;
    }

    std::array<std::uint8_t, 1800> crypto_data{};
    for (std::size_t i = 0; i < crypto_data.size(); ++i) {
        crypto_data[i] = static_cast<std::uint8_t>(i);
    }
    auto &space = server.packet_number_space(fiber::quic::QuicEncryptionLevel::Handshake);
    fiber::quic::QuicOutputFrame *crypto = space.alloc_frame();
    if (crypto == nullptr) {
        done_promise->set_value(std::unexpected(fiber::common::IoErr::NoMem));
        co_return;
    }
    crypto->type = fiber::quic::QuicFrameType::Crypto;
    crypto->u.crypto.offset = 0;
    crypto->data = fiber::quic::QuicSlice{crypto_data.data(), crypto_data.size()};
    space.pending_frames.push_back(*crypto);

    fiber::quic::QuicConnection::Options client_options{};
    client_options.role = fiber::quic::QuicConnectionRole::Client;
    fiber::quic::QuicConnection peer(client_options);
    auto peer_secret = fiber::quic::quic_set_encryption_secret(
            peer.crypto(), fiber::quic::QuicEncryptionLevel::Handshake, false, suite, handshake_secret.data(), 32);
    if (!peer_secret) {
        done_promise->set_value(std::unexpected(peer_secret.error()));
        co_return;
    }

    endpoint->schedule_send(server);

    auto readable = co_await client.wait_event(fiber::event::IoEvent::Read, std::chrono::milliseconds{500});
    if (!readable) {
        done_promise->set_value(std::unexpected(readable.error()));
        co_return;
    }

    std::array<std::uint8_t, 1400> response{};
    auto received = client.try_recv_from(response.data(), response.size());
    if (!received) {
        done_promise->set_value(std::unexpected(received.error()));
        co_return;
    }

    std::array<std::uint8_t, 1400> plaintext{};
    auto decoded = fiber::quic::quic_decode_packet(peer, response.data(), received->size, 0, plaintext.data(),
                                                   plaintext.size());
    if (!decoded) {
        done_promise->set_value(std::unexpected(decoded.error()));
        co_return;
    }

    const fiber::quic::QuicOutputFrame *sent = space.sent_frames.front();
    const fiber::quic::QuicOutputFrame *pending = space.pending_frames.front();
    done_promise->set_value(SplitFramePacketSummary{
            decoded->frame_count,
            sent != nullptr ? sent->data.len : 0,
            pending != nullptr ? pending->data.len : 0,
            pending != nullptr,
            space.sending_frames.empty(),
            server.congestion().in_flight,
    });
    client.close();
}

DetachedTask recv_handshake_ack_only_when_congestion_full(
        fiber::event::EventLoop *loop, fiber::quic::QuicUdpEndpoint *endpoint,
        std::promise<fiber::common::IoResult<AckOnlyPacketSummary>> *done_promise) {
    fiber::net::UdpSocket client(*loop);
    auto bound = client.bind(loopback(0), {});
    if (!bound) {
        done_promise->set_value(std::unexpected(bound.error()));
        co_return;
    }

    const auto server_cid = cid_from_hex("7172737475767778");
    const auto client_cid = cid_from_hex("8182838485868788");
    constexpr fiber::quic::QuicCryptoSuite suite = fiber::quic::QuicCryptoSuite::Aes128GcmSha256;
    std::array<std::uint8_t, fiber::quic::kQuicMaxSecretLength> handshake_secret{};
    for (std::size_t i = 0; i < 32; ++i) {
        handshake_secret[i] = static_cast<std::uint8_t>(0x60U + i);
    }

    fiber::quic::QuicConnection::Options server_options{};
    server_options.role = fiber::quic::QuicConnectionRole::Server;
    server_options.local_addr = endpoint->local_addr();
    server_options.remote_addr = client.local_addr();
    server_options.local_connection_id = server_cid;
    server_options.remote_connection_id = client_cid;
    fiber::quic::QuicConnection server(server_options);
    auto *path = server.active_path();
    if (path == nullptr) {
        done_promise->set_value(std::unexpected(fiber::common::IoErr::Invalid));
        co_return;
    }
    path->validated = true;
    server.congestion().window = fiber::quic::kQuicCongestionMinInitialSize;
    server.congestion().in_flight = server.congestion().window;

    auto server_secret = fiber::quic::quic_set_encryption_secret(
            server.crypto(), fiber::quic::QuicEncryptionLevel::Handshake, true, suite, handshake_secret.data(), 32);
    if (!server_secret) {
        done_promise->set_value(std::unexpected(server_secret.error()));
        co_return;
    }

    auto &space = server.packet_number_space(fiber::quic::QuicEncryptionLevel::Handshake);
    space.send_ack = true;
    space.pending_ack = 3;
    space.largest_received_packet_number = 3;

    fiber::quic::QuicOutputFrame *ping = space.alloc_frame();
    if (ping == nullptr) {
        done_promise->set_value(std::unexpected(fiber::common::IoErr::NoMem));
        co_return;
    }
    ping->type = fiber::quic::QuicFrameType::Ping;
    space.pending_frames.push_back(*ping);

    fiber::quic::QuicConnection::Options client_options{};
    client_options.role = fiber::quic::QuicConnectionRole::Client;
    fiber::quic::QuicConnection peer(client_options);
    auto peer_secret = fiber::quic::quic_set_encryption_secret(
            peer.crypto(), fiber::quic::QuicEncryptionLevel::Handshake, false, suite, handshake_secret.data(), 32);
    if (!peer_secret) {
        done_promise->set_value(std::unexpected(peer_secret.error()));
        co_return;
    }

    endpoint->schedule_send(server);

    auto readable = co_await client.wait_event(fiber::event::IoEvent::Read, std::chrono::milliseconds{500});
    if (!readable) {
        done_promise->set_value(std::unexpected(readable.error()));
        co_return;
    }

    std::array<std::uint8_t, 1400> response{};
    auto received = client.try_recv_from(response.data(), response.size());
    if (!received) {
        done_promise->set_value(std::unexpected(received.error()));
        co_return;
    }

    std::array<std::uint8_t, 256> plaintext{};
    auto decoded = fiber::quic::quic_decode_packet(peer, response.data(), received->size, 0, plaintext.data(),
                                                   plaintext.size());
    if (!decoded) {
        done_promise->set_value(std::unexpected(decoded.error()));
        co_return;
    }

    done_promise->set_value(AckOnlyPacketSummary{received->size, decoded->frame_count, decoded->ack_eliciting,
                                                 space.pending_frames.front() == ping, space.sending_frames.empty(),
                                                 server.congestion().in_flight});
    client.close();
}

DetachedTask recv_initial_ack_only_without_min_initial_padding(
        fiber::event::EventLoop *loop, fiber::quic::QuicUdpEndpoint *endpoint,
        std::promise<fiber::common::IoResult<AckOnlyPacketSummary>> *done_promise) {
    fiber::net::UdpSocket client(*loop);
    auto bound = client.bind(loopback(0), {});
    if (!bound) {
        done_promise->set_value(std::unexpected(bound.error()));
        co_return;
    }

    const auto original_dcid = cid_from_hex("9192939495969798");
    const auto server_cid = cid_from_hex("a1a2a3a4a5a6a7a8");
    const auto client_cid = cid_from_hex("b1b2b3b4b5b6b7b8");

    fiber::quic::QuicConnection::Options server_options{};
    server_options.role = fiber::quic::QuicConnectionRole::Server;
    server_options.local_addr = endpoint->local_addr();
    server_options.remote_addr = client.local_addr();
    server_options.local_connection_id = server_cid;
    server_options.remote_connection_id = client_cid;
    fiber::quic::QuicConnection server(server_options);
    auto *path = server.active_path();
    if (path == nullptr) {
        done_promise->set_value(std::unexpected(fiber::common::IoErr::Invalid));
        co_return;
    }
    path->validated = true;
    server.congestion().window = fiber::quic::kQuicCongestionMinInitialSize;
    server.congestion().in_flight = server.congestion().window;

    auto initial_ready = server.init_initial_crypto(original_dcid);
    if (!initial_ready) {
        done_promise->set_value(std::unexpected(initial_ready.error()));
        co_return;
    }

    auto &space = server.packet_number_space(fiber::quic::QuicEncryptionLevel::Initial);
    space.send_ack = true;
    space.pending_ack = 9;
    space.largest_received_packet_number = 9;

    fiber::quic::QuicConnection::Options client_options{};
    client_options.role = fiber::quic::QuicConnectionRole::Client;
    fiber::quic::QuicConnection peer(client_options);
    auto peer_initial_ready = peer.init_initial_crypto(original_dcid);
    if (!peer_initial_ready) {
        done_promise->set_value(std::unexpected(peer_initial_ready.error()));
        co_return;
    }

    endpoint->schedule_send(server);

    auto readable = co_await client.wait_event(fiber::event::IoEvent::Read, std::chrono::milliseconds{500});
    if (!readable) {
        done_promise->set_value(std::unexpected(readable.error()));
        co_return;
    }

    std::array<std::uint8_t, 1400> response{};
    auto received = client.try_recv_from(response.data(), response.size());
    if (!received) {
        done_promise->set_value(std::unexpected(received.error()));
        co_return;
    }

    std::array<std::uint8_t, 256> plaintext{};
    auto decoded = fiber::quic::quic_decode_packet(peer, response.data(), received->size, 0, plaintext.data(),
                                                   plaintext.size());
    if (!decoded) {
        done_promise->set_value(std::unexpected(decoded.error()));
        co_return;
    }

    done_promise->set_value(AckOnlyPacketSummary{received->size, decoded->frame_count, decoded->ack_eliciting,
                                                 !space.pending_frames.empty(), space.sending_frames.empty(),
                                                 server.congestion().in_flight});
    client.close();
}

EndpointResult recv_after_send(fiber::event::EventLoopGroup &group, fiber::quic::QuicUdpEndpoint &endpoint,
                               const std::uint8_t *data, std::size_t len) {
    std::promise<EndpointResult> recv_promise;
    std::promise<fiber::common::IoErr> send_promise;
    auto recv_future = recv_promise.get_future();
    auto send_future = send_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() { return recv_endpoint_once(&endpoint, &recv_promise); });
    fiber::async::spawn(group.at(0), [&]() {
        return send_datagram(&group.at(0), endpoint.local_addr().port(), data, len, &send_promise);
    });

    EXPECT_EQ(send_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(recv_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(send_future.get(), fiber::common::IoErr::None);
    return recv_future.get();
}

TwoEndpointResults recv_twice_after_send_twice(fiber::event::EventLoopGroup &group,
                                               fiber::quic::QuicUdpEndpoint &endpoint, const std::uint8_t *first,
                                               std::size_t first_len, const std::uint8_t *second,
                                               std::size_t second_len) {
    std::promise<TwoEndpointResults> recv_promise;
    std::promise<fiber::common::IoErr> send_promise;
    auto recv_future = recv_promise.get_future();
    auto send_future = send_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() { return recv_endpoint_twice(&endpoint, &recv_promise); });
    fiber::async::spawn(group.at(0), [&]() {
        return send_two_datagrams(&group.at(0), endpoint.local_addr().port(), first, first_len, second, second_len,
                                  &send_promise);
    });

    EXPECT_EQ(send_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(recv_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(send_future.get(), fiber::common::IoErr::None);
    return recv_future.get();
}

TwoEndpointResultsWithPorts
recv_twice_after_send_from_distinct_clients(fiber::event::EventLoopGroup &group, fiber::quic::QuicUdpEndpoint &endpoint,
                                            const std::uint8_t *first, std::size_t first_len,
                                            const std::uint8_t *second, std::size_t second_len) {
    std::promise<TwoEndpointResults> recv_promise;
    std::promise<fiber::common::IoResult<std::array<std::uint16_t, 2>>> send_promise;
    auto recv_future = recv_promise.get_future();
    auto send_future = send_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() { return recv_endpoint_twice(&endpoint, &recv_promise); });
    fiber::async::spawn(group.at(0), [&]() {
        return send_two_datagrams_from_distinct_clients(&group.at(0), endpoint.local_addr().port(), first, first_len,
                                                        second, second_len, &send_promise);
    });

    EXPECT_EQ(send_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(recv_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto send_result = send_future.get();
    EXPECT_TRUE(send_result.has_value()) << static_cast<int>(send_result.error());
    return TwoEndpointResultsWithPorts{recv_future.get(), send_result.value_or(std::array<std::uint16_t, 2>{})};
}

void close_endpoint_on_loop(fiber::event::EventLoopGroup &group, fiber::quic::QuicUdpEndpoint &endpoint) {
    std::promise<void> close_promise;
    auto close_future = close_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return close_endpoint(&endpoint, &close_promise); });
    EXPECT_EQ(close_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
}

} // namespace

TEST(QuicUdpEndpointTest, CreatesConnectionForNewInitialDcid) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options{};
    options.bind_addr = loopback(0);
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    const auto dcid = cid_from_hex("8394c8f03e515708");
    const auto scid = cid_from_hex("11223344");
    std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> datagram{};
    build_initial_datagram(datagram, dcid, scid);

    auto result = recv_after_send(group, endpoint, datagram.data(), datagram.size());

    ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
    EXPECT_TRUE(result->created);
    ASSERT_NE(result->connection, nullptr);
    EXPECT_EQ(endpoint.active_connection_count(), 1U);
    EXPECT_NE(endpoint.find_connection(dcid), nullptr);
    EXPECT_EQ(result->connection->state(), fiber::quic::QuicConnectionState::Handshaking);
    EXPECT_EQ(result->connection->local_addr().port(), endpoint.local_addr().port());
    EXPECT_NE(result->connection->remote_addr().port(), 0);
    EXPECT_EQ(result->connection->original_destination_connection_id().size(), dcid.size());
    EXPECT_EQ(result->connection->local_connection_id().size(), fiber::quic::kQuicConnectionIdLength);
    EXPECT_EQ(endpoint.find_connection(result->connection->local_connection_id()), result->connection);

    close_endpoint_on_loop(group, endpoint);
    group.stop();
    group.join();
}

TEST(QuicUdpEndpointTest, SendsInitialAckAfterProcessingAckElicitingInitial) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options{};
    options.bind_addr = loopback(0);
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    const auto dcid = cid_from_hex("8394c8f03e515708");
    const auto scid = cid_from_hex("11223344");
    std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> datagram{};
    build_initial_datagram(datagram, dcid, scid, 7);

    std::array<std::uint8_t, 1400> response{};
    std::promise<EndpointResult> recv_promise;
    std::promise<fiber::common::IoResult<std::size_t>> response_promise;
    auto recv_future = recv_promise.get_future();
    auto response_future = response_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() { return recv_endpoint_once(&endpoint, &recv_promise); });
    fiber::async::spawn(group.at(0), [&]() {
        return send_datagram_and_recv_response(&group.at(0), endpoint.local_addr().port(), datagram.data(),
                                               datagram.size(), response.data(), response.size(), &response_promise);
    });

    ASSERT_EQ(recv_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_EQ(response_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    auto recv_result = recv_future.get();
    ASSERT_TRUE(recv_result.has_value()) << static_cast<int>(recv_result.error());
    auto response_size = response_future.get();
    ASSERT_TRUE(response_size.has_value()) << static_cast<int>(response_size.error());

    fiber::quic::QuicConnection::Options client_options{};
    client_options.role = fiber::quic::QuicConnectionRole::Client;
    fiber::quic::QuicConnection client(client_options);
    ASSERT_TRUE(client.init_initial_crypto(dcid));

    std::array<std::uint8_t, 1400> plaintext{};
    auto decoded = fiber::quic::quic_decode_packet(client, response.data(), *response_size, 0, plaintext.data(),
                                                   plaintext.size());

    ASSERT_TRUE(decoded.has_value()) << static_cast<int>(decoded.error());
    EXPECT_EQ(decoded->header.type, fiber::quic::QuicPacketType::Initial);
    EXPECT_FALSE(decoded->ack_eliciting);
    EXPECT_GE(decoded->frame_count, 1U);

    close_endpoint_on_loop(group, endpoint);
    group.stop();
    group.join();
}

TEST(QuicUdpEndpointTest, CoalescesInitialAndHandshakePacketsIntoOneUdpDatagram) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options{};
    options.bind_addr = loopback(0);
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    std::promise<fiber::common::IoResult<CoalescedPacketSummary>> response_promise;
    auto response_future = response_promise.get_future();

    fiber::async::spawn(group.at(0),
                        [&]() { return recv_coalesced_initial_handshake(&group.at(0), &endpoint, &response_promise); });

    ASSERT_EQ(response_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto response = response_future.get();
    ASSERT_TRUE(response.has_value()) << static_cast<int>(response.error());
    EXPECT_EQ(response->datagram_len, fiber::quic::kMinInitialDatagramSize);
    EXPECT_LT(response->first_packet_len, response->datagram_len);
    EXPECT_EQ(response->first_type, fiber::quic::QuicPacketType::Initial);
    EXPECT_EQ(response->second_type, fiber::quic::QuicPacketType::Handshake);
    EXPECT_TRUE(response->first_ack_eliciting);
    EXPECT_TRUE(response->second_ack_eliciting);

    close_endpoint_on_loop(group, endpoint);
    group.stop();
    group.join();
}

TEST(QuicUdpEndpointTest, EncodesMoreThanEightFramesInOnePacket) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options{};
    options.bind_addr = loopback(0);
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    std::promise<fiber::common::IoResult<ManyFramePacketSummary>> response_promise;
    auto response_future = response_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() {
        return recv_handshake_packet_with_many_frames(&group.at(0), &endpoint, &response_promise);
    });

    ASSERT_EQ(response_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto response = response_future.get();
    ASSERT_TRUE(response.has_value()) << static_cast<int>(response.error());
    EXPECT_GE(response->decoded_frame_count, 12U);
    EXPECT_EQ(response->sent_frame_count, 12U);
    EXPECT_TRUE(response->pending_empty);
    EXPECT_TRUE(response->sending_empty);

    close_endpoint_on_loop(group, endpoint);
    group.stop();
    group.join();
}

TEST(QuicUdpEndpointTest, SplitsHandshakeCryptoFrameWhenPacketPayloadIsFull) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options{};
    options.bind_addr = loopback(0);
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    std::promise<fiber::common::IoResult<SplitFramePacketSummary>> response_promise;
    auto response_future = response_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() {
        return recv_split_handshake_crypto_frame(&group.at(0), &endpoint, &response_promise);
    });

    ASSERT_EQ(response_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto response = response_future.get();
    ASSERT_TRUE(response.has_value()) << static_cast<int>(response.error());
    EXPECT_EQ(response->decoded_frame_count, 1U);
    EXPECT_GT(response->sent_crypto_len, 0U);
    EXPECT_GT(response->pending_crypto_len, 0U);
    EXPECT_EQ(response->sent_crypto_len + response->pending_crypto_len, 1800U);
    EXPECT_TRUE(response->pending_owned);
    EXPECT_TRUE(response->sending_empty);
    EXPECT_GE(response->in_flight, fiber::quic::kQuicCongestionMinInitialSize);

    close_endpoint_on_loop(group, endpoint);
    group.stop();
    group.join();
}

TEST(QuicUdpEndpointTest, SendsOnlyAckWhenCongestionWindowIsFull) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options{};
    options.bind_addr = loopback(0);
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    std::promise<fiber::common::IoResult<AckOnlyPacketSummary>> response_promise;
    auto response_future = response_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() {
        return recv_handshake_ack_only_when_congestion_full(&group.at(0), &endpoint, &response_promise);
    });

    ASSERT_EQ(response_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto response = response_future.get();
    ASSERT_TRUE(response.has_value()) << static_cast<int>(response.error());
    EXPECT_EQ(response->decoded_frame_count, 1U);
    EXPECT_FALSE(response->ack_eliciting);
    EXPECT_TRUE(response->data_pending);
    EXPECT_TRUE(response->sending_empty);
    EXPECT_EQ(response->in_flight, fiber::quic::kQuicCongestionMinInitialSize);

    close_endpoint_on_loop(group, endpoint);
    group.stop();
    group.join();
}

TEST(QuicUdpEndpointTest, DoesNotPadInitialAckOnlyPacketToMinInitialSize) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options{};
    options.bind_addr = loopback(0);
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    std::promise<fiber::common::IoResult<AckOnlyPacketSummary>> response_promise;
    auto response_future = response_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() {
        return recv_initial_ack_only_without_min_initial_padding(&group.at(0), &endpoint, &response_promise);
    });

    ASSERT_EQ(response_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto response = response_future.get();
    ASSERT_TRUE(response.has_value()) << static_cast<int>(response.error());
    EXPECT_LT(response->datagram_len, fiber::quic::kMinInitialDatagramSize);
    EXPECT_EQ(response->decoded_frame_count, 1U);
    EXPECT_FALSE(response->ack_eliciting);
    EXPECT_TRUE(response->sending_empty);
    EXPECT_EQ(response->in_flight, fiber::quic::kQuicCongestionMinInitialSize);

    close_endpoint_on_loop(group, endpoint);
    group.stop();
    group.join();
}

TEST(QuicUdpEndpointTest, SendsDelayedApplicationAckFromSchedulerTimer) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options{};
    options.bind_addr = loopback(0);
    options.max_ack_delay = std::chrono::milliseconds{5};
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    std::promise<fiber::common::IoResult<DecodedPacketSummary>> response_promise;
    auto response_future = response_promise.get_future();

    fiber::async::spawn(group.at(0),
                        [&]() { return recv_delayed_application_ack(&group.at(0), &endpoint, &response_promise); });

    ASSERT_EQ(response_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto response = response_future.get();
    ASSERT_TRUE(response.has_value()) << static_cast<int>(response.error());
    EXPECT_EQ(response->type, fiber::quic::QuicPacketType::Short);
    EXPECT_FALSE(response->ack_eliciting);
    EXPECT_GE(response->frame_count, 1U);

    close_endpoint_on_loop(group, endpoint);
    group.stop();
    group.join();
}

TEST(QuicUdpEndpointTest, ReusesExistingConnectionForSameDcid) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options{};
    options.bind_addr = loopback(0);
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    const auto dcid = cid_from_hex("0011223344556677");
    const auto scid = cid_from_hex("10203040");
    std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> first{};
    std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> second{};
    build_initial_datagram(first, dcid, scid, 1);
    build_initial_datagram(second, dcid, scid, 2);

    auto results =
            recv_twice_after_send_twice(group, endpoint, first.data(), first.size(), second.data(), second.size());
    auto first_result = results.first;
    auto second_result = results.second;

    ASSERT_TRUE(first_result.has_value()) << static_cast<int>(first_result.error());
    ASSERT_TRUE(second_result.has_value()) << static_cast<int>(second_result.error());
    EXPECT_TRUE(first_result->created);
    EXPECT_FALSE(second_result->created);
    EXPECT_EQ(first_result->connection, second_result->connection);
    EXPECT_EQ(endpoint.active_connection_count(), 1U);

    close_endpoint_on_loop(group, endpoint);
    group.stop();
    group.join();
}

TEST(QuicUdpEndpointTest, MovesActivePathWhenSameConnectionArrivesFromDifferentRemoteAddress) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options{};
    options.bind_addr = loopback(0);
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    const auto dcid = cid_from_hex("0102030405060708");
    const auto scid = cid_from_hex("aabbccdd");
    std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> first{};
    std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> second{};
    build_initial_datagram(first, dcid, scid, 1);
    build_initial_datagram(second, dcid, scid, 2);

    auto outcome = recv_twice_after_send_from_distinct_clients(group, endpoint, first.data(), first.size(),
                                                               second.data(), second.size());

    ASSERT_TRUE(outcome.results.first.has_value()) << static_cast<int>(outcome.results.first.error());
    ASSERT_TRUE(outcome.results.second.has_value()) << static_cast<int>(outcome.results.second.error());
    ASSERT_NE(outcome.results.first->connection, nullptr);
    EXPECT_EQ(outcome.results.first->connection, outcome.results.second->connection);
    EXPECT_TRUE(outcome.results.first->created);
    EXPECT_FALSE(outcome.results.second->created);
    EXPECT_TRUE(outcome.results.second->packet.created_path);
    EXPECT_EQ(outcome.results.second->packet.path, outcome.results.second->connection->active_path());
    EXPECT_EQ(outcome.results.second->connection->remote_addr().port(), outcome.ports[1]);
    EXPECT_EQ(outcome.results.second->connection->path_count(), 1U);

    close_endpoint_on_loop(group, endpoint);
    group.stop();
    group.join();
}

TEST(QuicUdpEndpointTest, RejectsNewConnectionWhenSlotsAreFull) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options{};
    options.bind_addr = loopback(0);
    options.max_connections = 1;
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    const auto first_dcid = cid_from_hex("aaaaaaaaaaaaaaaa");
    const auto second_dcid = cid_from_hex("bbbbbbbbbbbbbbbb");
    const auto scid = cid_from_hex("11223344");
    std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> first{};
    std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> second{};
    build_initial_datagram(first, first_dcid, scid, 1);
    build_initial_datagram(second, second_dcid, scid, 1);

    auto first_result = recv_after_send(group, endpoint, first.data(), first.size());
    auto second_result = recv_after_send(group, endpoint, second.data(), second.size());

    ASSERT_TRUE(first_result.has_value()) << static_cast<int>(first_result.error());
    EXPECT_FALSE(second_result.has_value());
    EXPECT_EQ(second_result.error(), fiber::common::IoErr::NoMem);
    EXPECT_EQ(endpoint.active_connection_count(), 1U);
    EXPECT_NE(endpoint.find_connection(first_dcid), nullptr);
    EXPECT_EQ(endpoint.find_connection(second_dcid), nullptr);
    EXPECT_EQ(endpoint.rejected_connection_count(), 1U);

    close_endpoint_on_loop(group, endpoint);
    group.stop();
    group.join();
}

TEST(QuicUdpEndpointTest, DropsMalformedPacketWithoutCreatingConnection) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options{};
    options.bind_addr = loopback(0);
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> datagram{};
    auto result = recv_after_send(group, endpoint, datagram.data(), datagram.size());

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(endpoint.active_connection_count(), 0U);
    EXPECT_EQ(endpoint.dropped_datagram_count(), 1U);

    close_endpoint_on_loop(group, endpoint);
    group.stop();
    group.join();
}
