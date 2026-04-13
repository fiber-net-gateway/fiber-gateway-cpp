#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <string>
#include <vector>

#include "async/Sleep.h"
#include "async/Spawn.h"
#include "async/Timeout.h"
#include "common/IoError.h"
#include "dns/DnsClient.h"
#include "dns/DnsMessage.h"
#include "event/EventLoopGroup.h"
#include "net/TcpListener.h"
#include "net/TcpStream.h"
#include "net/UdpSocket.h"

namespace {

using namespace std::chrono_literals;
using fiber::async::DetachedTask;
using fiber::common::IoErr;
using fiber::dns::DnsClient;
using fiber::dns::MessageParser;
using fiber::dns::QuestionSpec;
using fiber::dns::RecordClass;
using fiber::dns::RecordType;

struct ClientOutcome {
    IoErr err = IoErr::Unknown;
    std::vector<std::uint8_t> packet;
};

struct ServerOutcome {
    IoErr err = IoErr::Unknown;
    std::size_t recv_count = 0;
};

fiber::common::IoResult<std::uint16_t> resolve_port(int fd) {
    sockaddr_storage bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    fiber::net::SocketAddress local;
    if (!fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<sockaddr *>(&bound), len, local)) {
        return std::unexpected(IoErr::NotSupported);
    }
    return local.port();
}

std::vector<std::uint8_t> encode_dns_name(std::string_view name) {
    std::vector<std::uint8_t> out;
    std::size_t start = 0;
    while (start < name.size()) {
        std::size_t dot = name.find('.', start);
        if (dot == std::string_view::npos) {
            dot = name.size();
        }
        const std::size_t label_len = dot - start;
        out.push_back(static_cast<std::uint8_t>(label_len));
        for (std::size_t i = start; i < dot; ++i) {
            out.push_back(static_cast<std::uint8_t>(name[i]));
        }
        start = dot + 1;
    }
    out.push_back(0);
    return out;
}

void push_be16(std::vector<std::uint8_t> &out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
    out.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void push_be32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

std::vector<std::uint8_t> make_a_response(std::uint16_t id, std::string_view qname, std::array<std::uint8_t, 4> addr) {
    std::vector<std::uint8_t> packet;
    packet.reserve(64);
    push_be16(packet, id);
    push_be16(packet, 0x8180U);
    push_be16(packet, 1);
    push_be16(packet, 1);
    push_be16(packet, 0);
    push_be16(packet, 0);

    std::vector<std::uint8_t> qname_wire = encode_dns_name(qname);
    packet.insert(packet.end(), qname_wire.begin(), qname_wire.end());
    push_be16(packet, static_cast<std::uint16_t>(RecordType::A));
    push_be16(packet, static_cast<std::uint16_t>(RecordClass::IN));

    push_be16(packet, 0xc00cU);
    push_be16(packet, static_cast<std::uint16_t>(RecordType::A));
    push_be16(packet, static_cast<std::uint16_t>(RecordClass::IN));
    push_be32(packet, 60);
    push_be16(packet, 4);
    packet.insert(packet.end(), addr.begin(), addr.end());
    return packet;
}

std::vector<std::uint8_t> make_truncated_response(std::uint16_t id, std::string_view qname) {
    std::vector<std::uint8_t> packet;
    packet.reserve(48);
    push_be16(packet, id);
    push_be16(packet, 0x8380U);
    push_be16(packet, 1);
    push_be16(packet, 0);
    push_be16(packet, 0);
    push_be16(packet, 0);

    std::vector<std::uint8_t> qname_wire = encode_dns_name(qname);
    packet.insert(packet.end(), qname_wire.begin(), qname_wire.end());
    push_be16(packet, static_cast<std::uint16_t>(RecordType::A));
    push_be16(packet, static_cast<std::uint16_t>(RecordClass::IN));
    return packet;
}

std::uint16_t read_be16(const std::uint8_t *data) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8U) | data[1]);
}

fiber::async::Task<fiber::common::IoResult<void>> read_exact(fiber::net::TcpStream &stream, std::uint8_t *buf,
                                                             std::size_t len) {
    std::size_t total = 0;
    while (total < len) {
        auto read_result =
                co_await fiber::async::timeout_for([&]() { return stream.read(buf + total, len - total); }, 2s);
        if (!read_result) {
            co_return std::unexpected(read_result.error());
        }
        if (*read_result == 0) {
            co_return std::unexpected(IoErr::ConnReset);
        }
        total += *read_result;
    }
    co_return fiber::common::IoResult<void>{};
}

fiber::async::Task<fiber::common::IoResult<void>> write_all(fiber::net::TcpStream &stream, const std::uint8_t *buf,
                                                            std::size_t len) {
    std::size_t total = 0;
    while (total < len) {
        auto write_result =
                co_await fiber::async::timeout_for([&]() { return stream.write(buf + total, len - total); }, 2s);
        if (!write_result) {
            co_return std::unexpected(write_result.error());
        }
        if (*write_result == 0) {
            co_return std::unexpected(IoErr::ConnReset);
        }
        total += *write_result;
    }
    co_return fiber::common::IoResult<void>{};
}

DetachedTask run_udp_success_server(fiber::event::EventLoop *loop, std::promise<std::uint16_t> *port_promise,
                                    std::promise<ServerOutcome> *outcome_promise) {
    ServerOutcome outcome;
    fiber::net::UdpSocket socket(*loop);
    fiber::net::UdpBindOptions bind_options{};
    auto bind_result = socket.bind(fiber::net::SocketAddress::any_v4(), bind_options);
    if (!bind_result) {
        port_promise->set_value(0);
        outcome.err = bind_result.error();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }

    auto port_result = resolve_port(socket.fd());
    port_promise->set_value(port_result ? *port_result : 0);
    if (!port_result) {
        outcome.err = port_result.error();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }

    std::array<std::uint8_t, 512> buf{};
    auto recv_result =
            co_await fiber::async::timeout_for([&]() { return socket.recv_from(buf.data(), buf.size()); }, 2s);
    if (!recv_result) {
        outcome.err = recv_result.error();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }

    ++outcome.recv_count;
    auto response = make_a_response(read_be16(buf.data()), "www.example.com", {1, 2, 3, 4});
    auto send_result = co_await fiber::async::timeout_for(
            [&]() { return socket.send_to(response.data(), response.size(), recv_result->peer); }, 2s);
    outcome.err = send_result ? IoErr::None : send_result.error();
    socket.close();
    outcome_promise->set_value(std::move(outcome));
}

DetachedTask run_udp_retry_server(fiber::event::EventLoop *loop, std::promise<std::uint16_t> *port_promise,
                                  std::promise<ServerOutcome> *outcome_promise) {
    ServerOutcome outcome;
    fiber::net::UdpSocket socket(*loop);
    auto bind_result = socket.bind(fiber::net::SocketAddress::any_v4(), {});
    if (!bind_result) {
        port_promise->set_value(0);
        outcome.err = bind_result.error();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }

    auto port_result = resolve_port(socket.fd());
    port_promise->set_value(port_result ? *port_result : 0);
    if (!port_result) {
        outcome.err = port_result.error();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }

    std::array<std::uint8_t, 512> buf{};
    fiber::net::SocketAddress peer;
    for (std::size_t attempt = 0; attempt < 2; ++attempt) {
        auto recv_result =
                co_await fiber::async::timeout_for([&]() { return socket.recv_from(buf.data(), buf.size()); }, 2s);
        if (!recv_result) {
            outcome.err = recv_result.error();
            outcome_promise->set_value(std::move(outcome));
            co_return;
        }
        ++outcome.recv_count;
        peer = recv_result->peer;
        if (attempt == 0) {
            continue;
        }
        auto response = make_a_response(read_be16(buf.data()), "www.example.com", {5, 6, 7, 8});
        auto send_result = co_await fiber::async::timeout_for(
                [&]() { return socket.send_to(response.data(), response.size(), peer); }, 2s);
        outcome.err = send_result ? IoErr::None : send_result.error();
        socket.close();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }

    outcome.err = IoErr::Unknown;
    socket.close();
    outcome_promise->set_value(std::move(outcome));
}

DetachedTask run_udp_tcp_fallback_server(fiber::event::EventLoop *loop, std::promise<std::uint16_t> *port_promise,
                                         std::promise<ServerOutcome> *outcome_promise) {
    ServerOutcome outcome;
    fiber::net::UdpSocket udp(*loop);
    auto udp_bind = udp.bind(fiber::net::SocketAddress::any_v4(), {});
    if (!udp_bind) {
        port_promise->set_value(0);
        outcome.err = udp_bind.error();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }

    auto port_result = resolve_port(udp.fd());
    if (!port_result) {
        port_promise->set_value(0);
        outcome.err = port_result.error();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }

    fiber::net::TcpListener listener(*loop);
    fiber::net::ListenOptions listen_options{};
    auto bind_result = listener.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), *port_result),
                                     listen_options);
    port_promise->set_value(bind_result ? *port_result : 0);
    if (!bind_result) {
        outcome.err = bind_result.error();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }

    std::array<std::uint8_t, 512> buf{};
    auto recv_result = co_await fiber::async::timeout_for([&]() { return udp.recv_from(buf.data(), buf.size()); }, 2s);
    if (!recv_result) {
        outcome.err = recv_result.error();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }
    ++outcome.recv_count;

    auto truncated = make_truncated_response(read_be16(buf.data()), "www.example.com");
    auto udp_send = co_await fiber::async::timeout_for(
            [&]() { return udp.send_to(truncated.data(), truncated.size(), recv_result->peer); }, 2s);
    if (!udp_send) {
        outcome.err = udp_send.error();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }

    auto accept_result = co_await fiber::async::timeout_for([&]() { return listener.accept(); }, 2s);
    if (!accept_result) {
        outcome.err = accept_result.error();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }

    fiber::net::TcpStream stream(*loop, accept_result->release_fd(), accept_result->take_peer());
    std::array<std::uint8_t, 2> prefix{};
    auto prefix_read = co_await read_exact(stream, prefix.data(), prefix.size());
    if (!prefix_read) {
        outcome.err = prefix_read.error();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }

    const std::size_t query_len = read_be16(prefix.data());
    std::vector<std::uint8_t> tcp_query(query_len);
    auto query_read = co_await read_exact(stream, tcp_query.data(), tcp_query.size());
    if (!query_read) {
        outcome.err = query_read.error();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }

    auto response = make_a_response(read_be16(tcp_query.data()), "www.example.com", {9, 9, 9, 9});
    std::array<std::uint8_t, 2> response_prefix{};
    response_prefix[0] = static_cast<std::uint8_t>(response.size() >> 8U);
    response_prefix[1] = static_cast<std::uint8_t>(response.size() & 0xffU);
    auto write_prefix_result = co_await write_all(stream, response_prefix.data(), response_prefix.size());
    if (!write_prefix_result) {
        outcome.err = write_prefix_result.error();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }
    auto write_payload_result = co_await write_all(stream, response.data(), response.size());
    outcome.err = write_payload_result ? IoErr::None : write_payload_result.error();
    udp.close();
    listener.close();
    stream.close();
    outcome_promise->set_value(std::move(outcome));
}

DetachedTask run_client_query(fiber::event::EventLoop *loop, std::uint16_t port, std::chrono::milliseconds timeout,
                              std::uint8_t attempts, bool enable_tcp_fallback,
                              std::promise<ClientOutcome> *outcome_promise) {
    ClientOutcome outcome;
    DnsClient client;
    DnsClient::Options options{};
    options.server = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);
    options.timeout = timeout;
    options.attempts = attempts;
    options.enable_tcp_fallback = enable_tcp_fallback;
    if (!client.init(*loop, options)) {
        outcome.err = IoErr::Invalid;
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }

    QuestionSpec question;
    question.name = "www.example.com";
    question.type = static_cast<std::uint16_t>(RecordType::A);
    question.dns_class = static_cast<std::uint16_t>(RecordClass::IN);

    std::array<std::uint8_t, 512> packet{};
    auto result = co_await client.query_raw(question, packet.data(), packet.size());
    outcome.err = result ? IoErr::None : result.error();
    if (result) {
        outcome.packet.assign(packet.begin(), packet.begin() + static_cast<std::ptrdiff_t>(*result));
    }
    client.close();
    client.release();
    outcome_promise->set_value(std::move(outcome));
}

DetachedTask run_client_close_while_waiting(fiber::event::EventLoop *loop,
                                            std::promise<ClientOutcome> *outcome_promise) {
    ClientOutcome outcome;
    DnsClient client;
    DnsClient::Options options{};
    options.server = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 65053);
    options.timeout = 500ms;
    options.attempts = 1;
    if (!client.init(*loop, options)) {
        outcome.err = IoErr::Invalid;
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }

    fiber::async::spawn(*loop, [&client]() -> DetachedTask {
        co_await fiber::async::sleep(20ms);
        client.close();
    });

    QuestionSpec question;
    question.name = "www.example.com";
    question.type = static_cast<std::uint16_t>(RecordType::A);
    question.dns_class = static_cast<std::uint16_t>(RecordClass::IN);

    std::array<std::uint8_t, 512> packet{};
    auto result = co_await client.query_raw(question, packet.data(), packet.size());
    outcome.err = result ? IoErr::None : result.error();
    client.release();
    outcome_promise->set_value(std::move(outcome));
}

TEST(DnsClientTest, QueryRawReturnsUdpResponse) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<std::uint16_t> port_promise;
    std::promise<ServerOutcome> server_promise;
    std::promise<ClientOutcome> client_promise;

    fiber::async::spawn(group.at(0),
                        [&]() { return run_udp_success_server(&group.at(0), &port_promise, &server_promise); });
    const auto port = port_promise.get_future().get();
    ASSERT_NE(port, 0);

    fiber::async::spawn(group.at(0),
                        [&]() { return run_client_query(&group.at(0), port, 200ms, 1, true, &client_promise); });

    const auto client = client_promise.get_future().get();
    const auto server = server_promise.get_future().get();
    group.stop();
    group.join();

    ASSERT_EQ(server.err, IoErr::None);
    ASSERT_EQ(client.err, IoErr::None);

    MessageParser parser;
    ASSERT_TRUE(parser.init());
    auto parsed = parser.parse(client.packet.data(), client.packet.size());
    ASSERT_TRUE(parsed.has_value()) << fiber::common::io_err_name(parsed.error());
    ASSERT_EQ(parsed->answer_count, 1u);
    ASSERT_EQ(parsed->answers[0].type, static_cast<std::uint16_t>(RecordType::A));
    ASSERT_EQ(parsed->answers[0].rdata_len, 4u);
    EXPECT_EQ(parsed->answers[0].rdata[0], 1u);
    EXPECT_EQ(parsed->answers[0].rdata[1], 2u);
    EXPECT_EQ(parsed->answers[0].rdata[2], 3u);
    EXPECT_EQ(parsed->answers[0].rdata[3], 4u);
}

TEST(DnsClientTest, QueryRawRetriesAfterTimeout) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<std::uint16_t> port_promise;
    std::promise<ServerOutcome> server_promise;
    std::promise<ClientOutcome> client_promise;

    fiber::async::spawn(group.at(0),
                        [&]() { return run_udp_retry_server(&group.at(0), &port_promise, &server_promise); });
    const auto port = port_promise.get_future().get();
    ASSERT_NE(port, 0);

    fiber::async::spawn(group.at(0),
                        [&]() { return run_client_query(&group.at(0), port, 40ms, 2, true, &client_promise); });

    const auto client = client_promise.get_future().get();
    const auto server = server_promise.get_future().get();
    group.stop();
    group.join();

    EXPECT_EQ(server.err, IoErr::None);
    EXPECT_EQ(server.recv_count, 2u);
    EXPECT_EQ(client.err, IoErr::None);
}

TEST(DnsClientTest, QueryRawFallsBackToTcpOnTruncatedUdpResponse) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<std::uint16_t> port_promise;
    std::promise<ServerOutcome> server_promise;
    std::promise<ClientOutcome> client_promise;

    fiber::async::spawn(group.at(0),
                        [&]() { return run_udp_tcp_fallback_server(&group.at(0), &port_promise, &server_promise); });
    const auto port = port_promise.get_future().get();
    ASSERT_NE(port, 0);

    fiber::async::spawn(group.at(0),
                        [&]() { return run_client_query(&group.at(0), port, 200ms, 1, true, &client_promise); });

    const auto client = client_promise.get_future().get();
    const auto server = server_promise.get_future().get();
    group.stop();
    group.join();

    ASSERT_EQ(server.err, IoErr::None);
    ASSERT_EQ(client.err, IoErr::None);

    MessageParser parser;
    ASSERT_TRUE(parser.init());
    auto parsed = parser.parse(client.packet.data(), client.packet.size());
    ASSERT_TRUE(parsed.has_value()) << fiber::common::io_err_name(parsed.error());
    ASSERT_EQ(parsed->answers[0].rdata[0], 9u);
    ASSERT_EQ(parsed->answers[0].rdata[1], 9u);
    ASSERT_EQ(parsed->answers[0].rdata[2], 9u);
    ASSERT_EQ(parsed->answers[0].rdata[3], 9u);
}

TEST(DnsClientTest, CloseCancelsPendingQuery) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<ClientOutcome> client_promise;
    fiber::async::spawn(group.at(0), [&]() { return run_client_close_while_waiting(&group.at(0), &client_promise); });

    const auto client = client_promise.get_future().get();
    group.stop();
    group.join();

    EXPECT_EQ(client.err, IoErr::Canceled);
}

} // namespace
