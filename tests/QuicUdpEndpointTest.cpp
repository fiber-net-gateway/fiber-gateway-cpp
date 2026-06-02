#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <string_view>

#include "async/Spawn.h"
#include "common/IoError.h"
#include "event/EventLoopGroup.h"
#include "net/IpAddress.h"
#include "net/SocketAddress.h"
#include "net/UdpSocket.h"
#include "quic/QuicCrypto.h"
#include "quic/QuicTransportCodec.h"
#include "quic/QuicUdpEndpoint.h"

namespace {

using DetachedTask = fiber::async::DetachedTask;
using EndpointResult = fiber::common::IoResult<fiber::quic::QuicUdpReceiveResult>;

struct TwoEndpointResults {
    EndpointResult first;
    EndpointResult second;
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

    fiber::quic::QuicFrame frame{};
    frame.type = fiber::quic::QuicFrameType::Ping;
    frame.ack_eliciting = true;

    std::array<std::uint8_t, 16> payload{};
    fiber::quic::QuicWriteCursor payload_out(payload.data(), payload.size());
    auto payload_len = fiber::quic::quic_create_frame(&payload_out, frame);
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

DetachedTask close_endpoint(fiber::quic::QuicUdpEndpoint *endpoint, std::promise<void> *done_promise) {
    endpoint->close();
    done_promise->set_value();
    co_return;
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
