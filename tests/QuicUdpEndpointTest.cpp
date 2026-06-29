#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <future>
#include <new>
#include <string_view>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "async/Sleep.h"
#include "async/Spawn.h"
#include "common/IoError.h"
#include "common/mem/IoBuf.h"
#include "event/EventLoopGroup.h"
#include "net/IpAddress.h"
#include "net/SocketAddress.h"
#include "net/UdpSocket.h"
#include "quic/QuicCrypto.h"
#include "quic/QuicPacketCodec.h"
#include "quic/QuicToken.h"
#include "quic/QuicTransportCodec.h"
#include "quic/QuicTransportParamsCodec.h"
#include "quic/QuicUdpEndpoint.h"

#include "QuicTestLoop.h"

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
    fiber::net::UdpEcn ecn = fiber::net::UdpEcn::Unspecified;
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

struct StreamPacketSummary {
    std::uint32_t decoded_frame_count = 0;
    std::uint64_t stream_id = 0;
    std::uint64_t stream_offset = 0;
    std::uint64_t stream_length = 0;
    bool has_length = false;
    bool fin = false;
    std::array<std::uint8_t, 8> data{};
};

struct TwoEndpointResults {
    EndpointResult first;
    EndpointResult second;
};

struct TwoEndpointResultsWithPorts {
    TwoEndpointResults results{};
    std::array<std::uint16_t, 2> ports{};
};

struct RetryInitialResult {
    fiber::quic::QuicConnectionId retry_scid{};
    std::size_t token_len = 0;
};

struct PortResponseResult {
    std::uint16_t port = 0;
    std::size_t response_len = 0;
};

using PathChallengeBytes = std::array<std::uint8_t, 8>;

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

void destroy_test_stream(void *, fiber::quic::QuicStream &stream) noexcept { delete &stream; }

fiber::quic::QuicStream::Lease make_test_stream(void *) noexcept {
    return fiber::quic::QuicStream::Lease::adopt(new (std::nothrow)
                                                         fiber::quic::QuicStream(nullptr, destroy_test_stream));
}

void destroy_heap_connection(void *, fiber::quic::QuicConnection &connection) noexcept { delete &connection; }

fiber::quic::QuicConnection::Lease
create_heap_connection(void *, const fiber::quic::QuicConnection::Options &options) noexcept {
    fiber::quic::QuicConnection::Options owned_options = options;
    owned_options.destroy_owner = nullptr;
    owned_options.on_destroy = destroy_heap_connection;
    return fiber::quic::QuicConnection::Lease::adopt(new (std::nothrow) fiber::quic::QuicConnection(owned_options));
}

fiber::quic::QuicUdpEndpoint::Options make_endpoint_options() noexcept {
    fiber::quic::QuicUdpEndpoint::Options options{};
    options.bind_addr = loopback(0);
    options.create_connection = create_heap_connection;
    return options;
}

struct EmbeddedConnectionFactoryState {
    alignas(fiber::quic::QuicConnection) std::byte storage[sizeof(fiber::quic::QuicConnection)]{};
    fiber::quic::QuicConnection *connection = nullptr;
    std::uint32_t create_calls = 0;
    std::uint32_t destroy_calls = 0;
};

void destroy_embedded_connection(void *owner, fiber::quic::QuicConnection &connection) noexcept {
    auto *state = static_cast<EmbeddedConnectionFactoryState *>(owner);
    ++state->destroy_calls;
    connection.~QuicConnection();
    state->connection = nullptr;
}

fiber::quic::QuicConnection::Lease
create_embedded_connection(void *owner, const fiber::quic::QuicConnection::Options &options) noexcept {
    auto *state = static_cast<EmbeddedConnectionFactoryState *>(owner);
    ++state->create_calls;
    if (state->connection != nullptr) {
        return {};
    }

    fiber::quic::QuicConnection::Options owned_options = options;
    owned_options.destroy_owner = state;
    owned_options.on_destroy = destroy_embedded_connection;
    state->connection = new (static_cast<void *>(state->storage)) fiber::quic::QuicConnection(owned_options);
    return fiber::quic::QuicConnection::Lease::adopt(state->connection);
}

fiber::mem::IoBuf iobuf_of(std::string_view value) {
    auto buf = fiber::mem::IoBuf::allocate(value.size());
    EXPECT_TRUE(buf.valid());
    std::memcpy(buf.writable_data(), value.data(), value.size());
    buf.commit(value.size());
    return buf;
}

void fill_new_token_test_secret(std::array<std::uint8_t, fiber::quic::kQuicMaxSecretLength> &secret) {
    secret = {};
    for (std::size_t i = 0; i < 32; ++i) {
        secret[i] = static_cast<std::uint8_t>(0x90U + i);
    }
}

void build_initial_datagram(std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> &datagram,
                            const fiber::quic::QuicConnectionId &dcid, const fiber::quic::QuicConnectionId &scid,
                            std::uint64_t packet_number = 1, fiber::quic::QuicSlice token = {}) {
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
    packet.token = token;
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

DetachedTask observe_endpoint_connection_count_after_delay(fiber::quic::QuicUdpEndpoint *endpoint,
                                                           std::chrono::milliseconds delay,
                                                           std::promise<std::size_t> *done_promise) {
    co_await fiber::async::sleep(delay);
    done_promise->set_value(endpoint->active_connection_count());
    endpoint->close();
    fiber::event::EventLoop::current().stop();
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

DetachedTask
send_datagram_recv_response_with_port(fiber::event::EventLoop *loop, std::uint16_t port, const std::uint8_t *data,
                                      std::size_t len, std::uint8_t *response, std::size_t response_cap,
                                      std::promise<fiber::common::IoResult<PortResponseResult>> *done_promise) {
    fiber::net::UdpSocket client(*loop);
    auto bound = client.bind(loopback(0), {});
    if (!bound) {
        done_promise->set_value(std::unexpected(bound.error()));
        co_return;
    }
    const std::uint16_t local_port = client.local_addr().port();

    auto sent = co_await client.send_to(data, len, loopback(port));
    if (!sent) {
        done_promise->set_value(std::unexpected(sent.error()));
        client.close();
        co_return;
    }

    auto received = co_await client.recv_from(response, response_cap);
    if (!received) {
        done_promise->set_value(std::unexpected(received.error()));
        client.close();
        co_return;
    }

    client.close();
    done_promise->set_value(PortResponseResult{local_port, received->size});
}

DetachedTask
send_datagram_from_port_and_recv_response(fiber::event::EventLoop *loop, std::uint16_t port, std::uint16_t local_port,
                                          const std::uint8_t *data, std::size_t len, std::uint8_t *response,
                                          std::size_t response_cap,
                                          std::promise<fiber::common::IoResult<std::size_t>> *done_promise) {
    fiber::net::UdpSocket client(*loop);
    auto bound = client.bind(loopback(local_port), {});
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

DetachedTask send_initial_then_retry_initial(fiber::event::EventLoop *loop, std::uint16_t port,
                                             const fiber::quic::QuicConnectionId original_dcid,
                                             const fiber::quic::QuicConnectionId client_scid,
                                             std::promise<fiber::common::IoResult<RetryInitialResult>> *done_promise) {
    fiber::net::UdpSocket client(*loop);
    auto bound = client.bind(loopback(0), {});
    if (!bound) {
        done_promise->set_value(std::unexpected(bound.error()));
        co_return;
    }

    std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> first{};
    build_initial_datagram(first, original_dcid, client_scid, 1);
    auto first_sent = co_await client.send_to(first.data(), first.size(), loopback(port));
    if (!first_sent) {
        done_promise->set_value(std::unexpected(first_sent.error()));
        client.close();
        co_return;
    }

    std::array<std::uint8_t, 512> retry_response{};
    auto received = co_await client.recv_from(retry_response.data(), retry_response.size());
    if (!received) {
        done_promise->set_value(std::unexpected(received.error()));
        client.close();
        co_return;
    }

    auto retry = fiber::quic::quic_parse_packet_header(retry_response.data(), received->size, 0);
    if (!retry || retry->type != fiber::quic::QuicPacketType::Retry || retry->token.empty()) {
        done_promise->set_value(std::unexpected(retry ? fiber::common::IoErr::Invalid : retry.error()));
        client.close();
        co_return;
    }

    std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> second{};
    build_initial_datagram(second, retry->scid, client_scid, 1, retry->token);
    auto second_sent = co_await client.send_to(second.data(), second.size(), loopback(port));
    if (!second_sent) {
        done_promise->set_value(std::unexpected(second_sent.error()));
        client.close();
        co_return;
    }

    done_promise->set_value(RetryInitialResult{retry->scid, retry->token.len});
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

DetachedTask establish_connection(fiber::quic::QuicConnection *connection, std::uint64_t active_connection_id_limit,
                                  std::promise<fiber::common::IoResult<void>> *done_promise) {
    if (connection == nullptr) {
        done_promise->set_value(std::unexpected(fiber::common::IoErr::Invalid));
        co_return;
    }

    fiber::quic::QuicTransportParams params{};
    params.has_initial_source_connection_id = true;
    params.initial_source_connection_id = connection->remote_connection_id();
    params.max_udp_payload_size = fiber::quic::kMinInitialDatagramSize;
    params.active_connection_id_limit = active_connection_id_limit;

    auto applied = connection->apply_peer_transport_params(params);
    if (!applied) {
        done_promise->set_value(std::unexpected(applied.error()));
        co_return;
    }
    auto established = connection->mark_established();
    if (!established) {
        done_promise->set_value(std::unexpected(established.error()));
        co_return;
    }
    done_promise->set_value({});
    co_return;
}

DetachedTask retire_connection_id(fiber::quic::QuicConnection *connection, std::uint64_t sequence_number,
                                  fiber::quic::QuicConnectionId packet_dcid,
                                  std::promise<fiber::common::IoResult<bool>> *done_promise) {
    if (connection == nullptr) {
        done_promise->set_value(std::unexpected(fiber::common::IoErr::Invalid));
        co_return;
    }

    fiber::quic::QuicRetireConnectionIdFrame frame{};
    frame.sequence_number = sequence_number;
    done_promise->set_value(connection->recv_retire_connection_id_frame(frame, packet_dcid));
    co_return;
}

DetachedTask prepare_connection_for_new_token(fiber::quic::QuicConnection *connection,
                                              std::promise<fiber::common::IoResult<PathChallengeBytes>> *done_promise) {
    if (connection == nullptr) {
        done_promise->set_value(std::unexpected(fiber::common::IoErr::Invalid));
        co_return;
    }

    constexpr fiber::quic::QuicCryptoSuite suite = fiber::quic::QuicCryptoSuite::Aes128GcmSha256;
    std::array<std::uint8_t, fiber::quic::kQuicMaxSecretLength> app_secret{};
    fill_new_token_test_secret(app_secret);

    auto read_secret = fiber::quic::quic_set_encryption_secret(
            connection->crypto(), fiber::quic::QuicEncryptionLevel::Application, false, suite, app_secret.data(), 32);
    if (!read_secret) {
        done_promise->set_value(std::unexpected(read_secret.error()));
        co_return;
    }
    auto write_secret = fiber::quic::quic_set_encryption_secret(
            connection->crypto(), fiber::quic::QuicEncryptionLevel::Application, true, suite, app_secret.data(), 32);
    if (!write_secret) {
        done_promise->set_value(std::unexpected(write_secret.error()));
        co_return;
    }

    auto *path = connection->active_path();
    if (path == nullptr) {
        done_promise->set_value(std::unexpected(fiber::common::IoErr::Invalid));
        co_return;
    }

    auto validating = connection->paths().start_validation(*path, fiber::quic::QuicTime{100});
    if (!validating) {
        done_promise->set_value(std::unexpected(validating.error()));
        co_return;
    }
    connection->paths().clear_frames(*path, fiber::quic::QuicFrameType::PathChallenge);

    PathChallengeBytes challenge{};
    std::memcpy(challenge.data(), path->challenge[0], challenge.size());
    done_promise->set_value(challenge);
    co_return;
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
    fiber::net::UdpBindOptions bind_options{};
    bind_options.recv_ecn = true;
    auto bound = client.bind(loopback(0), bind_options);
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
    auto received = client.try_recv_packet(response.data(), response.size());
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

    done_promise->set_value(ManyFramePacketSummary{decoded->frame_count, sent_count, received->ecn,
                                                   space.pending_frames.empty(), space.sending_frames.empty()});
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
    auto owned = fiber::quic::quic_output_frame_set_owned_data(*crypto, crypto_data.data(), crypto_data.size());
    if (!owned) {
        space.release_frame(*crypto);
        done_promise->set_value(std::unexpected(owned.error()));
        co_return;
    }
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
            sent != nullptr && sent->u.crypto.data != nullptr ? sent->u.crypto.data->readable() : 0,
            pending != nullptr && pending->u.crypto.data != nullptr ? pending->u.crypto.data->readable() : 0,
            pending != nullptr && pending->u.crypto.data != nullptr,
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

DetachedTask recv_application_stream_frame(fiber::event::EventLoop *loop, fiber::quic::QuicUdpEndpoint *endpoint,
                                           std::promise<fiber::common::IoResult<StreamPacketSummary>> *done_promise) {
    fiber::net::UdpSocket client(*loop);
    auto bound = client.bind(loopback(0), {});
    if (!bound) {
        done_promise->set_value(std::unexpected(bound.error()));
        co_return;
    }

    const auto server_cid = cid_from_hex("c1c2c3c4c5c6c7c8");
    const auto client_cid = cid_from_hex("d1d2d3d4d5d6d7d8");
    constexpr fiber::quic::QuicCryptoSuite suite = fiber::quic::QuicCryptoSuite::Aes128GcmSha256;
    std::array<std::uint8_t, fiber::quic::kQuicMaxSecretLength> app_secret{};
    for (std::size_t i = 0; i < 32; ++i) {
        app_secret[i] = static_cast<std::uint8_t>(0xa0U + i);
    }

    fiber::quic::QuicConnection::Options server_options{};
    server_options.role = fiber::quic::QuicConnectionRole::Server;
    server_options.local_addr = endpoint->local_addr();
    server_options.remote_addr = client.local_addr();
    server_options.local_connection_id = server_cid;
    server_options.remote_connection_id = client_cid;
    server_options.ops.create_stream = make_test_stream;
    fiber::quic::QuicConnection server(server_options);
    auto *path = server.active_path();
    if (path == nullptr) {
        done_promise->set_value(std::unexpected(fiber::common::IoErr::Invalid));
        co_return;
    }
    path->validated = true;

    auto server_secret = fiber::quic::quic_set_encryption_secret(
            server.crypto(), fiber::quic::QuicEncryptionLevel::Application, true, suite, app_secret.data(), 32);
    if (!server_secret) {
        done_promise->set_value(std::unexpected(server_secret.error()));
        co_return;
    }

    auto stream = server.get_or_create_peer_stream(0);
    if (!stream) {
        done_promise->set_value(std::unexpected(stream.error()));
        co_return;
    }
    fiber::quic::QuicMaxStreamDataFrame limit{};
    limit.id = 0;
    limit.limit = 1024;
    auto max_stream_data = server.recv_max_stream_data_frame(limit);
    if (!max_stream_data) {
        done_promise->set_value(std::unexpected(max_stream_data.error()));
        co_return;
    }
    fiber::quic::QuicMaxDataFrame max_data{};
    max_data.max_data = 1024;
    auto max_conn_data = server.recv_max_data_frame(max_data);
    if (!max_conn_data) {
        done_promise->set_value(std::unexpected(max_conn_data.error()));
        co_return;
    }
    auto written = (*stream)->try_write(iobuf_of("stream"));
    if (!written) {
        done_promise->set_value(std::unexpected(written.error()));
        co_return;
    }

    fiber::quic::QuicConnection::Options client_options{};
    client_options.role = fiber::quic::QuicConnectionRole::Client;
    fiber::quic::QuicConnection peer(client_options);
    auto peer_secret = fiber::quic::quic_set_encryption_secret(
            peer.crypto(), fiber::quic::QuicEncryptionLevel::Application, false, suite, app_secret.data(), 32);
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
    auto decoded = fiber::quic::quic_decode_packet(peer, response.data(), received->size, client_cid.size(),
                                                   plaintext.data(), plaintext.size());
    if (!decoded) {
        done_promise->set_value(std::unexpected(decoded.error()));
        co_return;
    }

    fiber::quic::QuicReadCursor payload(decoded->payload.data, decoded->payload.len);
    auto parsed = fiber::quic::quic_parse_frame_for_receiver(fiber::quic::QuicConnectionRole::Client,
                                                             fiber::quic::QuicEncryptionLevel::Application, payload);
    if (!parsed) {
        done_promise->set_value(std::unexpected(parsed.error()));
        co_return;
    }
    if (parsed->frame.type != fiber::quic::QuicFrameType::Stream || parsed->frame.data.len > 8) {
        done_promise->set_value(std::unexpected(fiber::common::IoErr::Invalid));
        co_return;
    }

    StreamPacketSummary summary{};
    summary.decoded_frame_count = decoded->frame_count;
    summary.stream_id = parsed->frame.u.stream.stream_id;
    summary.stream_offset = parsed->frame.u.stream.offset;
    summary.stream_length = parsed->frame.u.stream.length;
    summary.has_length = parsed->frame.u.stream.has_length;
    summary.fin = parsed->frame.u.stream.fin;
    std::memcpy(summary.data.data(), parsed->frame.data.data, parsed->frame.data.len);
    done_promise->set_value(summary);
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

fiber::common::IoResult<void> establish_connection_on_loop(fiber::event::EventLoopGroup &group,
                                                           fiber::quic::QuicConnection *connection,
                                                           std::uint64_t active_connection_id_limit) {
    std::promise<fiber::common::IoResult<void>> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return establish_connection(connection, active_connection_id_limit, &promise); });
    EXPECT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    return future.get();
}

fiber::common::IoResult<bool> retire_connection_id_on_loop(fiber::event::EventLoopGroup &group,
                                                           fiber::quic::QuicConnection *connection,
                                                           std::uint64_t sequence_number,
                                                           const fiber::quic::QuicConnectionId &packet_dcid) {
    std::promise<fiber::common::IoResult<bool>> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return retire_connection_id(connection, sequence_number, packet_dcid, &promise); });
    EXPECT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    return future.get();
}

std::size_t collect_pending_new_connection_ids(
        const fiber::quic::QuicConnection &connection,
        std::array<fiber::quic::QuicNewConnectionIdFrame, fiber::quic::kQuicLocalConnectionIdSlotCount> &frames) {
    const auto &space = connection.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);
    std::size_t count = 0;
    for (const fiber::quic::QuicOutputFrame *frame = space.pending_frames.front(); frame != nullptr;
         frame = space.pending_frames.next_of(*frame)) {
        if (frame->type != fiber::quic::QuicFrameType::NewConnectionId) {
            continue;
        }
        if (count < frames.size()) {
            frames[count] = frame->u.new_connection_id;
        }
        ++count;
    }
    return count;
}

void clear_pending_new_connection_ids(fiber::quic::QuicConnection &connection) {
    auto &space = connection.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);
    fiber::quic::QuicOutputFrame *prev = nullptr;
    fiber::quic::QuicOutputFrame *frame = space.pending_frames.front();
    while (frame != nullptr) {
        fiber::quic::QuicOutputFrame *next = space.pending_frames.next_of(*frame);
        if (frame->type == fiber::quic::QuicFrameType::NewConnectionId) {
            space.pending_frames.erase_after(prev, *frame);
            space.release_frame(*frame);
        } else {
            prev = frame;
        }
        frame = next;
    }
}

fiber::quic::QuicConnectionId cid_from_new_connection_id(const fiber::quic::QuicNewConnectionIdFrame &frame) {
    auto cid = fiber::quic::QuicConnectionId::from_bytes(frame.cid, frame.cid_len);
    EXPECT_TRUE(cid.has_value());
    return cid.value_or(fiber::quic::QuicConnectionId{});
}

// Replicates QuicUdpEndpoint::create_stateless_reset_token exactly:
// HMAC-SHA256(secret, [1 byte cid.length | cid bytes]) truncated to 16 bytes.
void compute_expected_stateless_reset_token(
        const std::array<std::uint8_t, fiber::quic::kQuicStatelessResetSecretLength> &secret,
        const fiber::quic::QuicConnectionId &cid, std::uint8_t out[fiber::quic::kStatelessResetTokenLength]) {
    std::uint8_t message[1 + fiber::quic::kMaxConnectionIdLength]{};
    message[0] = cid.length;
    if (!cid.empty()) {
        std::memcpy(message + 1, cid.data(), cid.size());
    }
    std::uint8_t digest[32]{};
    unsigned int digest_len = 0;
    HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()), message, 1 + cid.size(), digest, &digest_len);
    std::memcpy(out, digest, fiber::quic::kStatelessResetTokenLength);
}

// A minimal short-header (1-RTT) datagram: fixed bit set, long-header bit clear,
// followed by a 20-byte DCID and zero padding. Larger than the 41-byte trigger
// minimum so it elicits a stateless reset.
void build_short_header_datagram(std::array<std::uint8_t, 50> &datagram, const fiber::quic::QuicConnectionId &dcid) {
    datagram = {};
    datagram[0] = fiber::quic::kPacketFlagFixed | 0x01; // short header, packet-number length 1
    std::memcpy(datagram.data() + 1, dcid.data(), dcid.size());
}

// A minimal long-header Handshake datagram with an unknown DCID. Parses as a
// long header (not a reset trigger) and is dropped by the existing path.
void build_long_header_handshake_datagram(std::array<std::uint8_t, 50> &datagram,
                                          const fiber::quic::QuicConnectionId &dcid) {
    datagram = {};
    datagram[0] = fiber::quic::kPacketFlagLong | fiber::quic::kPacketFlagFixed | fiber::quic::kLongPacketTypeHandshake;
    datagram[1] = 0x00;
    datagram[2] = 0x00;
    datagram[3] = 0x00;
    datagram[4] = 0x01; // version 1 (big-endian)
    datagram[5] = static_cast<std::uint8_t>(dcid.size());
    std::memcpy(datagram.data() + 6, dcid.data(), dcid.size());
    datagram[6 + dcid.size()] = 0; // SCID length 0
    datagram[7 + dcid.size()] = 0; // length varint 0
}

void build_unsupported_version_long_datagram(std::array<std::uint8_t, 64> &datagram,
                                             const fiber::quic::QuicConnectionId &dcid,
                                             const fiber::quic::QuicConnectionId &scid, std::uint32_t version) {
    datagram = {};
    datagram[0] = fiber::quic::kPacketFlagLong | fiber::quic::kPacketFlagFixed | fiber::quic::kLongPacketTypeInitial;
    datagram[1] = static_cast<std::uint8_t>((version >> 24U) & 0xffU);
    datagram[2] = static_cast<std::uint8_t>((version >> 16U) & 0xffU);
    datagram[3] = static_cast<std::uint8_t>((version >> 8U) & 0xffU);
    datagram[4] = static_cast<std::uint8_t>(version & 0xffU);
    datagram[5] = static_cast<std::uint8_t>(dcid.size());
    std::memcpy(datagram.data() + 6, dcid.data(), dcid.size());
    const std::size_t scid_len_offset = 6 + dcid.size();
    datagram[scid_len_offset] = static_cast<std::uint8_t>(scid.size());
    std::memcpy(datagram.data() + scid_len_offset + 1, scid.data(), scid.size());
}

DetachedTask recv_endpoint_n_times(fiber::quic::QuicUdpEndpoint *endpoint, std::size_t n,
                                   std::promise<void> *done_promise) {
    for (std::size_t i = 0; i < n; ++i) {
        auto result = co_await endpoint->recv_once();
        if (!result &&
            (result.error() == fiber::common::IoErr::Canceled || result.error() == fiber::common::IoErr::BadFd)) {
            break; // endpoint closing
        }
    }
    done_promise->set_value();
}

// Sends a datagram, then waits (bounded) for a response. Resolves to false on
// timeout (no response), true if a response arrived.
DetachedTask send_datagram_and_check_no_response(fiber::event::EventLoop *loop, std::uint16_t port,
                                                 const std::uint8_t *data, std::size_t len,
                                                 std::promise<fiber::common::IoResult<bool>> *done_promise) {
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
    auto readable = co_await client.wait_event(fiber::event::IoEvent::Read, std::chrono::milliseconds(150));
    done_promise->set_value(readable.has_value());
    client.close();
}

// Sends `count` copies of a datagram, then counts responses with a bounded
// receive loop. Used to exercise the stateless-reset rate limit.
DetachedTask send_flood_and_count_responses(fiber::event::EventLoop *loop, std::uint16_t port, const std::uint8_t *data,
                                            std::size_t len, std::size_t count,
                                            std::promise<std::size_t> *done_promise) {
    fiber::net::UdpSocket client(*loop);
    auto bound = client.bind(loopback(0), {});
    if (!bound) {
        done_promise->set_value(0);
        co_return;
    }
    for (std::size_t i = 0; i < count; ++i) {
        co_await client.send_to(data, len, loopback(port));
    }
    std::size_t got = 0;
    for (;;) {
        auto readable = co_await client.wait_event(fiber::event::IoEvent::Read, std::chrono::milliseconds(150));
        if (!readable) {
            break; // timeout => no more responses
        }
        std::array<std::uint8_t, 1500> buf{};
        auto drained = client.try_recv_from(buf.data(), buf.size());
        (void) drained;
        ++got;
    }
    client.close();
    done_promise->set_value(got);
}

} // namespace

TEST(QuicUdpEndpointTest, InitRejectsMissingConnectionFactory) {
    fiber::event::EventLoop loop;

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options{};
    options.bind_addr = loopback(0);

    auto initialized = endpoint.init(loop, options);
    ASSERT_FALSE(initialized.has_value());
    EXPECT_EQ(initialized.error(), fiber::common::IoErr::Invalid);
}

TEST(QuicUdpEndpointTest, CreatesConnectionForNewInitialDcid) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options = make_endpoint_options();
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

TEST(QuicUdpEndpointTest, CustomConnectionFactoryCanEmbedConnection) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    EmbeddedConnectionFactoryState state{};
    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options = make_endpoint_options();
    options.connection_owner = &state;
    options.create_connection = create_embedded_connection;
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    const auto dcid = cid_from_hex("8394c8f03e515708");
    const auto scid = cid_from_hex("11223344");
    std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> datagram{};
    build_initial_datagram(datagram, dcid, scid);

    auto result = recv_after_send(group, endpoint, datagram.data(), datagram.size());

    ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
    ASSERT_NE(result->connection, nullptr);
    EXPECT_EQ(result->connection, state.connection);
    EXPECT_EQ(state.create_calls, 1U);
    EXPECT_EQ(state.destroy_calls, 0U);
    EXPECT_EQ(endpoint.active_connection_count(), 1U);
    EXPECT_EQ(endpoint.find_connection(dcid), result->connection);

    close_endpoint_on_loop(group, endpoint);

    EXPECT_EQ(endpoint.active_connection_count(), 0U);
    EXPECT_EQ(state.destroy_calls, 1U);
    EXPECT_EQ(state.connection, nullptr);

    group.stop();
    group.join();
}

TEST(QuicUdpEndpointTest, ConnectionDestroyWaitsForLastLeaseAfterEndpointDetach) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    EmbeddedConnectionFactoryState state{};
    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options = make_endpoint_options();
    options.connection_owner = &state;
    options.create_connection = create_embedded_connection;
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    const auto dcid = cid_from_hex("8394c8f03e515708");
    const auto scid = cid_from_hex("11223344");
    std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> datagram{};
    build_initial_datagram(datagram, dcid, scid);

    auto result = recv_after_send(group, endpoint, datagram.data(), datagram.size());

    ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
    ASSERT_NE(result->connection, nullptr);
    auto lease = result->connection->lease();
    EXPECT_EQ(state.create_calls, 1U);
    EXPECT_EQ(state.destroy_calls, 0U);

    close_endpoint_on_loop(group, endpoint);

    EXPECT_EQ(endpoint.active_connection_count(), 0U);
    EXPECT_EQ(state.destroy_calls, 0U);
    EXPECT_EQ(state.connection, lease.get());
    EXPECT_TRUE(lease->detached_from_endpoint());

    lease.reset();
    EXPECT_EQ(state.destroy_calls, 1U);
    EXPECT_EQ(state.connection, nullptr);

    group.stop();
    group.join();
}

TEST(QuicUdpEndpointTest, DetachClearsFramesAndSuppressesNewPendingFrames) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options = make_endpoint_options();
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    const auto dcid = cid_from_hex("8394c8f03e515708");
    const auto scid = cid_from_hex("11223344");
    std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> datagram{};
    build_initial_datagram(datagram, dcid, scid);

    auto result = recv_after_send(group, endpoint, datagram.data(), datagram.size());

    ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
    ASSERT_NE(result->connection, nullptr);
    auto lease = result->connection->lease();
    fiber::quic::QuicConnection *connection = lease.get();
    auto &space = connection->packet_number_space(fiber::quic::QuicEncryptionLevel::Application);

    fiber::quic::QuicOutputFrame *pending = space.alloc_frame();
    fiber::quic::QuicOutputFrame *sending = space.alloc_frame();
    fiber::quic::QuicOutputFrame *sent = space.alloc_frame();
    fiber::quic::QuicOutputFrame *path_pending = space.alloc_frame();
    ASSERT_NE(pending, nullptr);
    ASSERT_NE(sending, nullptr);
    ASSERT_NE(sent, nullptr);
    ASSERT_NE(path_pending, nullptr);

    pending->type = fiber::quic::QuicFrameType::Ping;
    sending->type = fiber::quic::QuicFrameType::MaxData;
    sent->type = fiber::quic::QuicFrameType::RetireConnectionId;
    space.pending_frames.push_back(*pending);
    space.sending_frames.push_back(*sending);
    space.sent_frames.push_back(*sent);

    fiber::quic::QuicPath *path = connection->active_path();
    ASSERT_NE(path, nullptr);
    path_pending->type = fiber::quic::QuicFrameType::PathResponse;
    path_pending->path = path;
    path->pending_frames.push_back(*path_pending);

    close_endpoint_on_loop(group, endpoint);

    EXPECT_EQ(endpoint.active_connection_count(), 0U);
    EXPECT_TRUE(connection->closed());
    EXPECT_FALSE(connection->attached_to_endpoint());
    EXPECT_TRUE(connection->detached_from_endpoint());
    EXPECT_TRUE(space.pending_frames.empty());
    EXPECT_TRUE(space.sending_frames.empty());
    EXPECT_TRUE(space.sent_frames.empty());
    EXPECT_TRUE(path->pending_frames.empty());

    const std::array<std::uint8_t, 8> response_data{1, 2, 3, 4, 5, 6, 7, 8};
    auto queued = connection->paths().queue_path_response_frame(*path, response_data.data());
    EXPECT_FALSE(queued.has_value());
    EXPECT_TRUE(path->pending_frames.empty());

    lease.reset();
    group.stop();
    group.join();
}

TEST(QuicUdpEndpointTest, EstablishingConnectionQueuesLocalConnectionIdsUpToPeerLimit) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options = make_endpoint_options();
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    const auto dcid = cid_from_hex("8394c8f03e515708");
    const auto scid = cid_from_hex("11223344");
    std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> datagram{};
    build_initial_datagram(datagram, dcid, scid);

    auto result = recv_after_send(group, endpoint, datagram.data(), datagram.size());
    ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
    ASSERT_NE(result->connection, nullptr);

    auto established = establish_connection_on_loop(group, result->connection, 3);
    ASSERT_TRUE(established.has_value()) << static_cast<int>(established.error());

    std::array<fiber::quic::QuicNewConnectionIdFrame, fiber::quic::kQuicLocalConnectionIdSlotCount> frames{};
    EXPECT_EQ(collect_pending_new_connection_ids(*result->connection, frames), 2U);
    EXPECT_EQ(frames[0].sequence_number, 1U);
    EXPECT_EQ(frames[1].sequence_number, 2U);
    EXPECT_EQ(frames[0].retire_prior_to, 0U);
    EXPECT_EQ(frames[1].retire_prior_to, 0U);

    const auto first_cid = cid_from_new_connection_id(frames[0]);
    const auto second_cid = cid_from_new_connection_id(frames[1]);
    EXPECT_EQ(first_cid.size(), fiber::quic::kQuicConnectionIdLength);
    EXPECT_EQ(second_cid.size(), fiber::quic::kQuicConnectionIdLength);
    EXPECT_EQ(endpoint.find_connection(first_cid), result->connection);
    EXPECT_EQ(endpoint.find_connection(second_cid), result->connection);

    close_endpoint_on_loop(group, endpoint);
    group.stop();
    group.join();
}

TEST(QuicUdpEndpointTest, EstablishingConnectionCapsLocalConnectionIdsAtPeerLimit) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options = make_endpoint_options();
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    const auto dcid = cid_from_hex("7394c8f03e515708");
    const auto scid = cid_from_hex("21223344");
    std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> datagram{};
    build_initial_datagram(datagram, dcid, scid);

    auto result = recv_after_send(group, endpoint, datagram.data(), datagram.size());
    ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
    ASSERT_NE(result->connection, nullptr);

    auto established = establish_connection_on_loop(group, result->connection, 2);
    ASSERT_TRUE(established.has_value()) << static_cast<int>(established.error());

    std::array<fiber::quic::QuicNewConnectionIdFrame, fiber::quic::kQuicLocalConnectionIdSlotCount> frames{};
    EXPECT_EQ(collect_pending_new_connection_ids(*result->connection, frames), 1U);
    EXPECT_EQ(frames[0].sequence_number, 1U);
    EXPECT_EQ(endpoint.find_connection(cid_from_new_connection_id(frames[0])), result->connection);

    close_endpoint_on_loop(group, endpoint);
    group.stop();
    group.join();
}

TEST(QuicUdpEndpointTest, RetiringLocalConnectionIdUnregistersAndReplenishes) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options = make_endpoint_options();
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    const auto dcid = cid_from_hex("6394c8f03e515708");
    const auto scid = cid_from_hex("31223344");
    std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> datagram{};
    build_initial_datagram(datagram, dcid, scid);

    auto result = recv_after_send(group, endpoint, datagram.data(), datagram.size());
    ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
    ASSERT_NE(result->connection, nullptr);
    auto established = establish_connection_on_loop(group, result->connection, 3);
    ASSERT_TRUE(established.has_value()) << static_cast<int>(established.error());

    std::array<fiber::quic::QuicNewConnectionIdFrame, fiber::quic::kQuicLocalConnectionIdSlotCount> frames{};
    ASSERT_EQ(collect_pending_new_connection_ids(*result->connection, frames), 2U);
    const auto retired_cid = cid_from_new_connection_id(frames[0]);
    const auto kept_cid = cid_from_new_connection_id(frames[1]);
    clear_pending_new_connection_ids(*result->connection);

    auto retired =
            retire_connection_id_on_loop(group, result->connection, 1, result->connection->local_connection_id());
    ASSERT_TRUE(retired.has_value()) << static_cast<int>(retired.error());
    EXPECT_TRUE(*retired);
    EXPECT_EQ(endpoint.find_connection(retired_cid), nullptr);
    EXPECT_EQ(endpoint.find_connection(kept_cid), result->connection);

    frames = {};
    ASSERT_EQ(collect_pending_new_connection_ids(*result->connection, frames), 1U);
    EXPECT_EQ(frames[0].sequence_number, 3U);
    EXPECT_EQ(endpoint.find_connection(cid_from_new_connection_id(frames[0])), result->connection);

    close_endpoint_on_loop(group, endpoint);
    group.stop();
    group.join();
}

TEST(QuicUdpEndpointTest, RejectsRetiringCurrentLocalConnectionId) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options = make_endpoint_options();
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    const auto dcid = cid_from_hex("5394c8f03e515708");
    const auto scid = cid_from_hex("41223344");
    std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> datagram{};
    build_initial_datagram(datagram, dcid, scid);

    auto result = recv_after_send(group, endpoint, datagram.data(), datagram.size());
    ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
    ASSERT_NE(result->connection, nullptr);
    auto established = establish_connection_on_loop(group, result->connection, 3);
    ASSERT_TRUE(established.has_value()) << static_cast<int>(established.error());

    std::array<fiber::quic::QuicNewConnectionIdFrame, fiber::quic::kQuicLocalConnectionIdSlotCount> frames{};
    ASSERT_EQ(collect_pending_new_connection_ids(*result->connection, frames), 2U);
    const auto retired_cid = cid_from_new_connection_id(frames[0]);
    clear_pending_new_connection_ids(*result->connection);

    auto retired = retire_connection_id_on_loop(group, result->connection, 1, retired_cid);
    ASSERT_TRUE(retired.has_value()) << static_cast<int>(retired.error());
    EXPECT_TRUE(*retired);
    EXPECT_EQ(result->connection->state(), fiber::quic::QuicConnectionState::Closing);
    EXPECT_EQ(result->connection->close_error(), fiber::quic::QuicErrorCode::ProtocolViolation);

    close_endpoint_on_loop(group, endpoint);
    group.stop();
    group.join();
}

TEST(QuicUdpEndpointTest, IdleTimeoutDeletesConnection) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options = make_endpoint_options();
    options.transport.max_idle_timeout = std::chrono::milliseconds(5);
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    const auto dcid = cid_from_hex("8394c8f03e515708");
    const auto scid = cid_from_hex("11223344");
    std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> datagram{};
    build_initial_datagram(datagram, dcid, scid);

    auto result = recv_after_send(group, endpoint, datagram.data(), datagram.size());
    ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
    EXPECT_EQ(endpoint.active_connection_count(), 1U);

    std::promise<std::size_t> count_promise;
    auto count_future = count_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return observe_endpoint_connection_count_after_delay(&endpoint, std::chrono::milliseconds(30), &count_promise);
    });

    ASSERT_EQ(count_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(count_future.get(), 0U);

    group.join();
}

TEST(QuicUdpEndpointTest, RetryEnabledSendsRetryWithoutCreatingConnection) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options = make_endpoint_options();
    options.retry = true;
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    const auto dcid = cid_from_hex("1020304050607080");
    const auto scid = cid_from_hex("a1a2a3a4");
    std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> datagram{};
    build_initial_datagram(datagram, dcid, scid);

    std::array<std::uint8_t, 512> response{};
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
    EXPECT_FALSE(recv_result.has_value());
    EXPECT_EQ(recv_result.error(), fiber::common::IoErr::WouldBlock);
    auto response_size = response_future.get();
    ASSERT_TRUE(response_size.has_value()) << static_cast<int>(response_size.error());

    auto retry = fiber::quic::quic_parse_packet_header(response.data(), *response_size, 0);
    ASSERT_TRUE(retry.has_value()) << static_cast<int>(retry.error());
    EXPECT_EQ(retry->type, fiber::quic::QuicPacketType::Retry);
    EXPECT_EQ(retry->dcid.size(), scid.size());
    EXPECT_EQ(std::memcmp(retry->dcid.data(), scid.data(), scid.size()), 0);
    EXPECT_EQ(retry->scid.size(), fiber::quic::kQuicConnectionIdLength);
    EXPECT_FALSE(retry->token.empty());
    EXPECT_EQ(endpoint.active_connection_count(), 0U);
    EXPECT_EQ(endpoint.find_connection(dcid), nullptr);

    close_endpoint_on_loop(group, endpoint);
    group.stop();
    group.join();
}

TEST(QuicUdpEndpointTest, ValidRetryTokenCreatesValidatedConnectionWithRetryIds) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options = make_endpoint_options();
    options.retry = true;
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    const auto original_dcid = cid_from_hex("0102030405060708");
    const auto client_scid = cid_from_hex("aabbccdd");
    std::promise<TwoEndpointResults> recv_promise;
    std::promise<fiber::common::IoResult<RetryInitialResult>> send_promise;
    auto recv_future = recv_promise.get_future();
    auto send_future = send_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() { return recv_endpoint_twice(&endpoint, &recv_promise); });
    fiber::async::spawn(group.at(0), [&]() {
        return send_initial_then_retry_initial(&group.at(0), endpoint.local_addr().port(), original_dcid, client_scid,
                                               &send_promise);
    });

    ASSERT_EQ(send_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_EQ(recv_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    auto send_result = send_future.get();
    ASSERT_TRUE(send_result.has_value()) << static_cast<int>(send_result.error());
    auto recv_results = recv_future.get();
    EXPECT_FALSE(recv_results.first.has_value());
    EXPECT_EQ(recv_results.first.error(), fiber::common::IoErr::WouldBlock);
    ASSERT_TRUE(recv_results.second.has_value()) << static_cast<int>(recv_results.second.error());

    fiber::quic::QuicConnection *connection = recv_results.second->connection;
    ASSERT_NE(connection, nullptr);
    EXPECT_TRUE(recv_results.second->created);
    EXPECT_TRUE(connection->retried());
    EXPECT_EQ(connection->original_destination_connection_id().size(), original_dcid.size());
    EXPECT_EQ(std::memcmp(connection->original_destination_connection_id().data(), original_dcid.data(),
                          original_dcid.size()),
              0);
    EXPECT_EQ(connection->initial_destination_connection_id().size(), send_result->retry_scid.size());
    EXPECT_EQ(std::memcmp(connection->initial_destination_connection_id().data(), send_result->retry_scid.data(),
                          send_result->retry_scid.size()),
              0);
    EXPECT_EQ(connection->retry_source_connection_id().size(), send_result->retry_scid.size());
    EXPECT_EQ(std::memcmp(connection->retry_source_connection_id().data(), send_result->retry_scid.data(),
                          send_result->retry_scid.size()),
              0);
    ASSERT_NE(connection->active_path(), nullptr);
    EXPECT_TRUE(connection->active_path()->validated);
    EXPECT_GT(send_result->token_len, 0U);
    EXPECT_EQ(endpoint.active_connection_count(), 1U);

    close_endpoint_on_loop(group, endpoint);
    group.stop();
    group.join();
}

TEST(QuicUdpEndpointTest, SendsNewTokenWhenPathValidationCompletes) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::array<std::uint8_t, fiber::quic::kQuicAddressValidationKeyLength> key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>(0x50U + i);
    }

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options = make_endpoint_options();
    options.issue_new_token = true;
    options.address_validation_key_set = true;
    options.address_validation_key = key;
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    const auto dcid = cid_from_hex("a0a1a2a3a4a5a6a7");
    const auto client_scid = cid_from_hex("b0b1b2b3");
    std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> initial{};
    build_initial_datagram(initial, dcid, client_scid);

    std::array<std::uint8_t, 1400> initial_response{};
    std::promise<EndpointResult> initial_recv_promise;
    std::promise<fiber::common::IoResult<PortResponseResult>> initial_send_promise;
    auto initial_recv_future = initial_recv_promise.get_future();
    auto initial_send_future = initial_send_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() { return recv_endpoint_once(&endpoint, &initial_recv_promise); });
    fiber::async::spawn(group.at(0), [&]() {
        return send_datagram_recv_response_with_port(&group.at(0), endpoint.local_addr().port(), initial.data(),
                                                     initial.size(), initial_response.data(), initial_response.size(),
                                                     &initial_send_promise);
    });

    ASSERT_EQ(initial_recv_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_EQ(initial_send_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto initial_recv = initial_recv_future.get();
    ASSERT_TRUE(initial_recv.has_value()) << static_cast<int>(initial_recv.error());
    auto initial_send = initial_send_future.get();
    ASSERT_TRUE(initial_send.has_value()) << static_cast<int>(initial_send.error());

    fiber::quic::QuicConnection *connection = initial_recv->connection;
    ASSERT_NE(connection, nullptr);

    constexpr fiber::quic::QuicCryptoSuite suite = fiber::quic::QuicCryptoSuite::Aes128GcmSha256;
    std::array<std::uint8_t, fiber::quic::kQuicMaxSecretLength> app_secret{};
    fill_new_token_test_secret(app_secret);

    std::promise<fiber::common::IoResult<PathChallengeBytes>> prepare_promise;
    auto prepare_future = prepare_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return prepare_connection_for_new_token(connection, &prepare_promise); });
    ASSERT_EQ(prepare_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto challenge = prepare_future.get();
    ASSERT_TRUE(challenge.has_value()) << static_cast<int>(challenge.error());

    fiber::quic::QuicConnection::Options peer_options{};
    peer_options.role = fiber::quic::QuicConnectionRole::Client;
    peer_options.loop = &group.at(0);
    fiber::quic::QuicConnection peer(peer_options);
    ASSERT_TRUE(fiber::quic::quic_set_encryption_secret(peer.crypto(), fiber::quic::QuicEncryptionLevel::Application,
                                                        true, suite, app_secret.data(), 32));
    ASSERT_TRUE(fiber::quic::quic_set_encryption_secret(peer.crypto(), fiber::quic::QuicEncryptionLevel::Application,
                                                        false, suite, app_secret.data(), 32));

    fiber::quic::QuicOutputFrame path_response{};
    path_response.type = fiber::quic::QuicFrameType::PathResponse;
    std::memcpy(path_response.u.path_response.data, challenge->data(), challenge->size());

    std::array<std::uint8_t, 1200> app_datagram{};
    std::array<std::uint8_t, 1200> app_plaintext{};
    fiber::quic::QuicPacketEncodeSpec app_spec{};
    app_spec.level = fiber::quic::QuicEncryptionLevel::Application;
    app_spec.dcid = connection->local_connection_id();
    app_spec.frames = &path_response;
    app_spec.frame_count = 1;
    auto encoded_app = fiber::quic::quic_encode_packet(peer, app_spec, {app_plaintext.data(), app_plaintext.size()},
                                                       app_datagram.data(), app_datagram.size());
    ASSERT_TRUE(encoded_app.has_value()) << static_cast<int>(encoded_app.error());

    std::array<std::uint8_t, 1400> new_token_response{};
    std::promise<EndpointResult> app_recv_promise;
    std::promise<fiber::common::IoResult<std::size_t>> app_send_promise;
    auto app_recv_future = app_recv_promise.get_future();
    auto app_send_future = app_send_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() { return recv_endpoint_once(&endpoint, &app_recv_promise); });
    fiber::async::spawn(group.at(0), [&]() {
        return send_datagram_from_port_and_recv_response(
                &group.at(0), endpoint.local_addr().port(), initial_send->port, app_datagram.data(),
                encoded_app->packet_len, new_token_response.data(), new_token_response.size(), &app_send_promise);
    });

    ASSERT_EQ(app_recv_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_EQ(app_send_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto app_recv = app_recv_future.get();
    ASSERT_TRUE(app_recv.has_value()) << static_cast<int>(app_recv.error());
    EXPECT_TRUE(app_recv->packet.path_validated);
    EXPECT_EQ(app_recv->packet.validated_path, connection->active_path());
    auto app_response_size = app_send_future.get();
    ASSERT_TRUE(app_response_size.has_value()) << static_cast<int>(app_response_size.error());

    std::array<std::uint8_t, 1400> plaintext{};
    auto decoded = fiber::quic::quic_decode_packet(peer, new_token_response.data(), *app_response_size,
                                                   static_cast<std::uint8_t>(client_scid.size()), plaintext.data(),
                                                   plaintext.size());
    ASSERT_TRUE(decoded.has_value()) << static_cast<int>(decoded.error());

    fiber::quic::QuicSlice token{};
    fiber::quic::QuicReadCursor payload(decoded->payload.data, decoded->payload.len);
    while (!payload.empty()) {
        auto parsed = fiber::quic::quic_parse_frame_for_receiver(
                fiber::quic::QuicConnectionRole::Client, fiber::quic::QuicEncryptionLevel::Application, payload);
        ASSERT_TRUE(parsed.has_value()) << static_cast<int>(parsed.error());
        if (parsed->frame.type == fiber::quic::QuicFrameType::NewToken) {
            token = parsed->frame.data;
            break;
        }
    }
    ASSERT_FALSE(token.empty());

    auto checked = fiber::quic::quic_validate_address_token(key, loopback(initial_send->port),
                                                            fiber::quic::quic_unix_seconds_now(), token);
    ASSERT_TRUE(checked.has_value()) << static_cast<int>(checked.error());
    EXPECT_EQ(checked->status, fiber::quic::QuicAddressTokenValidationStatus::Valid);
    EXPECT_EQ(checked->kind, fiber::quic::QuicAddressTokenKind::NewToken);

    close_endpoint_on_loop(group, endpoint);
    group.stop();
    group.join();
}

TEST(QuicUdpEndpointTest, SendsInitialAckAfterProcessingAckElicitingInitial) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options = make_endpoint_options();
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
    client_options.loop = &group.at(0);
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
    fiber::quic::QuicUdpEndpoint::Options options = make_endpoint_options();
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
    fiber::quic::QuicUdpEndpoint::Options options = make_endpoint_options();
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
    EXPECT_EQ(response->ecn, fiber::net::UdpEcn::Ect0);
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
    fiber::quic::QuicUdpEndpoint::Options options = make_endpoint_options();
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
    fiber::quic::QuicUdpEndpoint::Options options = make_endpoint_options();
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

TEST(QuicUdpEndpointTest, EncodesApplicationStreamFrameFromSendQueue) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options = make_endpoint_options();
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    std::promise<fiber::common::IoResult<StreamPacketSummary>> response_promise;
    auto response_future = response_promise.get_future();

    fiber::async::spawn(group.at(0),
                        [&]() { return recv_application_stream_frame(&group.at(0), &endpoint, &response_promise); });

    ASSERT_EQ(response_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto response = response_future.get();
    ASSERT_TRUE(response.has_value()) << static_cast<int>(response.error());
    EXPECT_EQ(response->decoded_frame_count, 1U);
    EXPECT_EQ(response->stream_id, 0U);
    EXPECT_EQ(response->stream_offset, 0U);
    EXPECT_EQ(response->stream_length, 6U);
    EXPECT_TRUE(response->has_length);
    EXPECT_FALSE(response->fin);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(response->data.data()), response->stream_length),
              "stream");

    close_endpoint_on_loop(group, endpoint);
    group.stop();
    group.join();
}

TEST(QuicUdpEndpointTest, DoesNotPadInitialAckOnlyPacketToMinInitialSize) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options = make_endpoint_options();
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

TEST(QuicUdpEndpointTest, ReusesExistingConnectionForSameDcid) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options = make_endpoint_options();
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
    fiber::quic::QuicUdpEndpoint::Options options = make_endpoint_options();
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
    fiber::quic::QuicUdpEndpoint::Options options = make_endpoint_options();
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
    fiber::quic::QuicUdpEndpoint::Options options = make_endpoint_options();
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

TEST(QuicUdpEndpointTest, SendsStatelessResetForUnknownShortHeaderDcid) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options = make_endpoint_options();
    // Fixed secret so the reset token can be recomputed in the test.
    options.stateless_reset_secret_set = true;
    for (std::size_t i = 0; i < fiber::quic::kQuicStatelessResetSecretLength; ++i) {
        options.stateless_reset_secret[i] = static_cast<std::uint8_t>(0xa0U + i);
    }
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    const auto dcid = cid_from_hex("0102030405060708090a0b0c0d0e0f1011121314"); // 20 bytes, unknown
    std::array<std::uint8_t, 50> datagram{};
    build_short_header_datagram(datagram, dcid);

    std::array<std::uint8_t, 512> response{};
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
    EXPECT_FALSE(recv_result.has_value());
    EXPECT_EQ(recv_result.error(), fiber::common::IoErr::WouldBlock); // reset sent, no connection created

    auto response_size = response_future.get();
    ASSERT_TRUE(response_size.has_value()) << static_cast<int>(response_size.error());
    const std::size_t len = *response_size;

    // Length within nginx's range and never larger than the triggering datagram.
    EXPECT_GE(len, fiber::quic::kQuicStatelessResetMinPacket);
    EXPECT_LE(len, datagram.size()); // anti-amplification
    EXPECT_LT(len, fiber::quic::kQuicStatelessResetMaxPacket);

    // Shaped as a short header: fixed bit set, long-header bit clear.
    EXPECT_NE(response[0] & fiber::quic::kPacketFlagFixed, 0u);
    EXPECT_EQ(response[0] & fiber::quic::kPacketFlagLong, 0u);

    // Final 16 bytes == HMAC-SHA256(secret, [len|dcid])[:16].
    std::array<std::uint8_t, fiber::quic::kStatelessResetTokenLength> expected_token{};
    compute_expected_stateless_reset_token(options.stateless_reset_secret, dcid, expected_token.data());
    EXPECT_EQ(std::memcmp(response.data() + len - fiber::quic::kStatelessResetTokenLength, expected_token.data(),
                          fiber::quic::kStatelessResetTokenLength),
              0);

    // Fully stateless: no connection was created.
    EXPECT_EQ(endpoint.active_connection_count(), 0U);
    EXPECT_EQ(endpoint.find_connection(dcid), nullptr);

    close_endpoint_on_loop(group, endpoint);
    group.stop();
    group.join();
}

TEST(QuicUdpEndpointTest, DoesNotSendStatelessResetForLongHeader) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options = make_endpoint_options();
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    const auto dcid = cid_from_hex("0102030405060708"); // 8 bytes, unknown
    std::array<std::uint8_t, 50> datagram{};
    build_long_header_handshake_datagram(datagram, dcid);

    std::promise<EndpointResult> recv_promise;
    std::promise<fiber::common::IoResult<bool>> response_promise;
    auto recv_future = recv_promise.get_future();
    auto response_future = response_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() { return recv_endpoint_once(&endpoint, &recv_promise); });
    fiber::async::spawn(group.at(0), [&]() {
        return send_datagram_and_check_no_response(&group.at(0), endpoint.local_addr().port(), datagram.data(),
                                                   datagram.size(), &response_promise);
    });

    ASSERT_EQ(recv_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_EQ(response_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    auto recv_result = recv_future.get();
    EXPECT_FALSE(recv_result.has_value());
    // Long-header non-Initial for an unknown DCID is dropped, not reset.
    EXPECT_EQ(recv_result.error(), fiber::common::IoErr::Invalid);
    EXPECT_EQ(endpoint.dropped_datagram_count(), 1U);
    EXPECT_EQ(endpoint.active_connection_count(), 0U);

    auto response = response_future.get();
    ASSERT_TRUE(response.has_value()) << static_cast<int>(response.error());
    EXPECT_FALSE(*response); // no stateless reset sent

    close_endpoint_on_loop(group, endpoint);
    group.stop();
    group.join();
}

TEST(QuicUdpEndpointTest, SendsVersionNegotiationForUnsupportedVersionLongHeader) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options = make_endpoint_options();
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    const auto dcid = cid_from_hex("0102030405060708");
    const auto scid = cid_from_hex("11223344");
    std::array<std::uint8_t, 64> datagram{};
    build_unsupported_version_long_datagram(datagram, dcid, scid, 0xfaceb00cU);

    std::array<std::uint8_t, 512> response{};
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
    EXPECT_FALSE(recv_result.has_value());
    EXPECT_EQ(recv_result.error(), fiber::common::IoErr::WouldBlock);

    auto response_size = response_future.get();
    ASSERT_TRUE(response_size.has_value()) << static_cast<int>(response_size.error());
    const std::size_t len = *response_size;

    auto packet = fiber::quic::quic_parse_packet_header(response.data(), len, 0);
    ASSERT_TRUE(packet.has_value()) << static_cast<int>(packet.error());
    EXPECT_EQ(packet->type, fiber::quic::QuicPacketType::VersionNegotiation);
    EXPECT_EQ(packet->version, 0U);
    EXPECT_EQ(packet->dcid.size(), scid.size());
    EXPECT_EQ(std::memcmp(packet->dcid.data(), scid.data(), scid.size()), 0);
    EXPECT_EQ(packet->scid.size(), dcid.size());
    EXPECT_EQ(std::memcmp(packet->scid.data(), dcid.data(), dcid.size()), 0);

    const std::size_t supported_versions_offset = 1U + 4U + 1U + scid.size() + 1U + dcid.size();
    ASSERT_GE(len, supported_versions_offset + 4U);
    EXPECT_EQ(response[supported_versions_offset], 0U);
    EXPECT_EQ(response[supported_versions_offset + 1], 0U);
    EXPECT_EQ(response[supported_versions_offset + 2], 0U);
    EXPECT_EQ(response[supported_versions_offset + 3], 1U);
    EXPECT_EQ(endpoint.active_connection_count(), 0U);
    EXPECT_EQ(endpoint.find_connection(dcid), nullptr);
    EXPECT_EQ(endpoint.dropped_datagram_count(), 0U);

    close_endpoint_on_loop(group, endpoint);
    group.stop();
    group.join();
}

TEST(QuicUdpEndpointTest, DoesNotRespondToVersionNegotiationPacket) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options = make_endpoint_options();
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    const auto dcid = cid_from_hex("0102030405060708");
    const auto scid = cid_from_hex("11223344");
    std::array<std::uint8_t, 64> datagram{};
    build_unsupported_version_long_datagram(datagram, dcid, scid, 0);

    std::promise<EndpointResult> recv_promise;
    std::promise<fiber::common::IoResult<bool>> response_promise;
    auto recv_future = recv_promise.get_future();
    auto response_future = response_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() { return recv_endpoint_once(&endpoint, &recv_promise); });
    fiber::async::spawn(group.at(0), [&]() {
        return send_datagram_and_check_no_response(&group.at(0), endpoint.local_addr().port(), datagram.data(),
                                                   datagram.size(), &response_promise);
    });

    ASSERT_EQ(recv_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_EQ(response_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    auto recv_result = recv_future.get();
    EXPECT_FALSE(recv_result.has_value());
    EXPECT_EQ(recv_result.error(), fiber::common::IoErr::Invalid);
    EXPECT_EQ(endpoint.dropped_datagram_count(), 1U);
    EXPECT_EQ(endpoint.active_connection_count(), 0U);

    auto response = response_future.get();
    ASSERT_TRUE(response.has_value()) << static_cast<int>(response.error());
    EXPECT_FALSE(*response);

    close_endpoint_on_loop(group, endpoint);
    group.stop();
    group.join();
}

TEST(QuicUdpEndpointTest, DoesNotSendStatelessResetForTooSmallShortHeader) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options = make_endpoint_options();
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    const auto dcid = cid_from_hex("0102030405060708090a0b0c0d0e0f1011121314"); // 20 bytes, unknown
    // 30-byte short header: below the 41-byte trigger minimum (nginx
    // NGX_QUIC_MIN_PKT_LEN). Must not elicit a reset (also serves as
    // loop-prevention against tiny/spurious packets).
    std::array<std::uint8_t, 30> datagram{};
    datagram[0] = fiber::quic::kPacketFlagFixed | 0x01;
    std::memcpy(datagram.data() + 1, dcid.data(), dcid.size());

    std::promise<EndpointResult> recv_promise;
    std::promise<fiber::common::IoResult<bool>> response_promise;
    auto recv_future = recv_promise.get_future();
    auto response_future = response_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() { return recv_endpoint_once(&endpoint, &recv_promise); });
    fiber::async::spawn(group.at(0), [&]() {
        return send_datagram_and_check_no_response(&group.at(0), endpoint.local_addr().port(), datagram.data(),
                                                   datagram.size(), &response_promise);
    });

    ASSERT_EQ(recv_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_EQ(response_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    auto recv_result = recv_future.get();
    EXPECT_FALSE(recv_result.has_value());
    EXPECT_EQ(recv_result.error(), fiber::common::IoErr::Invalid); // dropped, not reset
    EXPECT_EQ(endpoint.dropped_datagram_count(), 1U);

    auto response = response_future.get();
    ASSERT_TRUE(response.has_value()) << static_cast<int>(response.error());
    EXPECT_FALSE(*response); // no stateless reset sent

    close_endpoint_on_loop(group, endpoint);
    group.stop();
    group.join();
}

TEST(QuicUdpEndpointTest, StatelessResetIsRateLimited) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::Options options = make_endpoint_options();
    ASSERT_TRUE(endpoint.init(group.at(0), options));

    const auto dcid = cid_from_hex("0102030405060708090a0b0c0d0e0f1011121314"); // 20 bytes, unknown
    std::array<std::uint8_t, 50> datagram{};
    build_short_header_datagram(datagram, dcid);

    const std::size_t flood = 50;
    const std::uint16_t port = endpoint.local_addr().port();

    std::promise<void> recv_done;
    std::promise<std::size_t> count_promise;
    auto recv_future = recv_done.get_future();
    auto count_future = count_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() { return recv_endpoint_n_times(&endpoint, flood, &recv_done); });
    fiber::async::spawn(group.at(0), [&]() {
        return send_flood_and_count_responses(&group.at(0), port, datagram.data(), datagram.size(), flood,
                                              &count_promise);
    });

    ASSERT_EQ(count_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    ASSERT_EQ(recv_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    recv_future.get();

    const std::size_t resets = count_future.get();
    // A flood far larger than the capacity produces a bounded number of resets.
    EXPECT_GE(resets, 1u);
    EXPECT_LE(resets, fiber::quic::kQuicStatelessResetRateLimitCapacity);

    close_endpoint_on_loop(group, endpoint);
    group.stop();
    group.join();
}
