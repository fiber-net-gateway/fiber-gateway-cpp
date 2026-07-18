#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <future>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

#include "async/Sleep.h"
#include "async/Spawn.h"
#include "common/IoError.h"
#include "event/EventLoopGroup.h"
#include "net/SocketAddress.h"
#include "net/UdpSocket.h"

namespace {

using DetachedTask = fiber::async::DetachedTask;

struct PacketMetadataOutcome {
    fiber::common::IoErr err = fiber::common::IoErr::Unknown;
    fiber::net::SocketAddress peer{};
    fiber::net::SocketAddress local{};
    fiber::net::UdpEcn ecn = fiber::net::UdpEcn::Unspecified;
};

struct ReadCallbackContext {
    fiber::net::UdpSocket *socket = nullptr;
    std::promise<fiber::common::IoErr> *done_promise = nullptr;
    bool completed = false;
};

void on_udp_read_ready(void *opaque, fiber::common::IoErr err) noexcept {
    auto *ctx = static_cast<ReadCallbackContext *>(opaque);
    if (ctx == nullptr || ctx->completed) {
        return;
    }
    if (err == fiber::common::IoErr::None) {
        std::array<char, 16> buf{};
        auto received = ctx->socket->try_recv_from(buf.data(), buf.size());
        if (!received && received.error() == fiber::common::IoErr::WouldBlock) {
            return;
        }
        err = received ? fiber::common::IoErr::None : received.error();
    }

    ctx->completed = true;
    (void) ctx->socket->clear_read_callback(&on_udp_read_ready, ctx);
    ctx->socket->close();
    ctx->done_promise->set_value(err);
}

fiber::common::IoResult<fiber::net::SocketAddress> get_bound_address(int fd) {
    sockaddr_storage bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    fiber::net::SocketAddress out;
    if (!fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<const sockaddr *>(&bound), len, out)) {
        return std::unexpected(fiber::common::IoErr::NotSupported);
    }
    return out;
}

DetachedTask server_echo(fiber::event::EventLoop *loop, std::promise<uint16_t> *port_promise,
                         std::promise<fiber::common::IoErr> *done_promise) {
    auto *server = new fiber::net::UdpSocket(*loop);
    fiber::net::UdpBindOptions options{};
    fiber::net::SocketAddress addr(fiber::net::IpAddress::loopback_v4(), 0);

    auto bind_result = server->bind(addr, options);
    if (!bind_result) {
        port_promise->set_value(0);
        done_promise->set_value(bind_result.error());
        delete server;
        co_return;
    }

    auto local_result = get_bound_address(server->fd());
    if (!local_result) {
        port_promise->set_value(0);
        done_promise->set_value(local_result.error());
        server->close();
        delete server;
        co_return;
    }
    port_promise->set_value(local_result->port());

    char buf[64] = {};
    auto recv_result = co_await server->recv_from(buf, sizeof(buf));
    if (!recv_result) {
        done_promise->set_value(recv_result.error());
        server->close();
        delete server;
        co_return;
    }

    constexpr std::string_view reply = "pong";
    auto send_result = co_await server->send_to(reply.data(), reply.size(), recv_result->peer);
    if (!send_result) {
        done_promise->set_value(send_result.error());
    } else {
        done_promise->set_value(fiber::common::IoErr::None);
    }
    server->close();
    delete server;
    co_return;
}

DetachedTask client_echo(fiber::event::EventLoop *loop, uint16_t port,
                         std::promise<fiber::common::IoErr> *done_promise) {
    auto *client = new fiber::net::UdpSocket(*loop);
    fiber::net::UdpBindOptions options{};
    fiber::net::SocketAddress local(fiber::net::IpAddress::loopback_v4(), 0);

    auto bind_result = client->bind(local, options);
    if (!bind_result) {
        done_promise->set_value(bind_result.error());
        delete client;
        co_return;
    }

    constexpr std::string_view message = "ping";
    fiber::net::SocketAddress server_addr(fiber::net::IpAddress::loopback_v4(), port);
    auto send_result = co_await client->send_to(message.data(), message.size(), server_addr);
    if (!send_result) {
        done_promise->set_value(send_result.error());
        client->close();
        delete client;
        co_return;
    }

    char buf[64] = {};
    auto recv_result = co_await client->recv_from(buf, sizeof(buf));
    if (!recv_result) {
        done_promise->set_value(recv_result.error());
        client->close();
        delete client;
        co_return;
    }

    std::string_view received(buf, recv_result->size);
    if (received != "pong") {
        done_promise->set_value(fiber::common::IoErr::Unknown);
    } else {
        done_promise->set_value(fiber::common::IoErr::None);
    }
    client->close();
    delete client;
    co_return;
}

#ifdef SO_REUSEPORT
DetachedTask bind_reuse_port(fiber::event::EventLoop *loop, std::promise<fiber::common::IoErr> *done_promise) {
    auto *first = new fiber::net::UdpSocket(*loop);
    auto *second = new fiber::net::UdpSocket(*loop);
    fiber::net::UdpBindOptions options{};
    options.reuse_port = true;

    fiber::net::SocketAddress addr(fiber::net::IpAddress::loopback_v4(), 0);
    auto bind_first = first->bind(addr, options);
    if (!bind_first) {
        done_promise->set_value(bind_first.error());
        delete first;
        delete second;
        co_return;
    }

    auto local_result = get_bound_address(first->fd());
    if (!local_result) {
        done_promise->set_value(local_result.error());
        first->close();
        delete first;
        delete second;
        co_return;
    }

    fiber::net::SocketAddress second_addr(fiber::net::IpAddress::loopback_v4(), local_result->port());
    auto bind_second = second->bind(second_addr, options);
    if (!bind_second) {
        done_promise->set_value(bind_second.error());
    } else {
        done_promise->set_value(fiber::common::IoErr::None);
    }

    first->close();
    second->close();
    delete first;
    delete second;
    co_return;
}
#endif

DetachedTask server_recv_packet(fiber::event::EventLoop *loop, std::promise<uint16_t> *port_promise,
                                std::promise<PacketMetadataOutcome> *done_promise) {
    auto *server = new fiber::net::UdpSocket(*loop);
    fiber::net::UdpBindOptions options{};
    options.recv_packet_info = true;
    options.recv_ecn = true;

    auto bind_result = server->bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), options);
    if (!bind_result) {
        port_promise->set_value(0);
        done_promise->set_value(PacketMetadataOutcome{.err = bind_result.error()});
        delete server;
        co_return;
    }

    port_promise->set_value(server->local_addr().port());

    std::array<char, 32> buf{};
    auto recv_result = co_await server->recv_packet(buf.data(), buf.size());
    PacketMetadataOutcome outcome;
    if (!recv_result) {
        outcome.err = recv_result.error();
    } else {
        outcome.err = fiber::common::IoErr::None;
        outcome.peer = recv_result->peer;
        outcome.local = recv_result->local;
        outcome.ecn = recv_result->ecn;
    }

    server->close();
    delete server;
    done_promise->set_value(outcome);
}

DetachedTask client_send_packet_with_ecn(fiber::event::EventLoop *loop, uint16_t port,
                                         std::promise<fiber::common::IoErr> *done_promise) {
    auto *client = new fiber::net::UdpSocket(*loop);
    auto bind_result = client->bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), {});
    if (!bind_result) {
        done_promise->set_value(bind_result.error());
        delete client;
        co_return;
    }

    constexpr std::string_view message = "ecn";
    fiber::net::UdpPacketSendSpec spec;
    spec.buf = message.data();
    spec.len = message.size();
    spec.peer = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);
    spec.ecn = fiber::net::UdpEcn::Ect0;

    auto send_result = co_await client->send_packet(spec);
    done_promise->set_value(send_result ? fiber::common::IoErr::None : send_result.error());
    client->close();
    delete client;
}

DetachedTask server_recv_two_packets(fiber::event::EventLoop *loop, std::promise<uint16_t> *port_promise,
                                     std::promise<fiber::common::IoErr> *done_promise) {
    auto *server = new fiber::net::UdpSocket(*loop);
    auto bind_result = server->bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), {});
    if (!bind_result) {
        port_promise->set_value(0);
        done_promise->set_value(bind_result.error());
        delete server;
        co_return;
    }

    port_promise->set_value(server->local_addr().port());

    std::array<char, 32> buf{};
    for (int i = 0; i < 2; ++i) {
        auto recv_result = co_await server->recv_from(buf.data(), buf.size());
        if (!recv_result) {
            done_promise->set_value(recv_result.error());
            server->close();
            delete server;
            co_return;
        }
    }

    server->close();
    delete server;
    done_promise->set_value(fiber::common::IoErr::None);
}

DetachedTask client_batch_send_packets(fiber::event::EventLoop *loop, uint16_t port,
                                       std::promise<fiber::common::IoResult<size_t>> *done_promise) {
    auto *client = new fiber::net::UdpSocket(*loop);
    auto bind_result = client->bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), {});
    if (!bind_result) {
        done_promise->set_value(std::unexpected(bind_result.error()));
        delete client;
        co_return;
    }

    constexpr std::string_view first = "one";
    constexpr std::string_view second = "two";
    fiber::net::SocketAddress peer(fiber::net::IpAddress::loopback_v4(), port);
    fiber::net::UdpPacketSendSpec specs[2]{};
    specs[0].buf = first.data();
    specs[0].len = first.size();
    specs[0].peer = peer;
    specs[1].buf = second.data();
    specs[1].len = second.size();
    specs[1].peer = peer;

    done_promise->set_value(client->try_send_packets(specs, 2));
    client->close();
    delete client;
}

DetachedTask server_wait_readable_then_recv(fiber::event::EventLoop *loop, std::promise<uint16_t> *port_promise,
                                            std::promise<fiber::common::IoErr> *done_promise) {
    auto *server = new fiber::net::UdpSocket(*loop);
    auto bind_result = server->bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), {});
    if (!bind_result) {
        port_promise->set_value(0);
        done_promise->set_value(bind_result.error());
        delete server;
        co_return;
    }

    port_promise->set_value(server->local_addr().port());

    auto wait_result = co_await server->wait_readable(std::chrono::seconds(2));
    if (!wait_result) {
        done_promise->set_value(wait_result.error());
        server->close();
        delete server;
        co_return;
    }
    std::array<char, 16> buf{};
    auto recv_result = co_await server->recv_from(buf.data(), buf.size());
    done_promise->set_value(recv_result ? fiber::common::IoErr::None : recv_result.error());
    server->close();
    delete server;
}

DetachedTask client_sleep_then_send(fiber::event::EventLoop *loop, uint16_t port,
                                    std::promise<fiber::common::IoErr> *done_promise) {
    auto *client = new fiber::net::UdpSocket(*loop);
    auto bind_result = client->bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), {});
    if (!bind_result) {
        done_promise->set_value(bind_result.error());
        delete client;
        co_return;
    }

    co_await fiber::async::sleep(std::chrono::milliseconds(20));

    constexpr std::string_view message = "wake";
    auto send_result = co_await client->send_to(message.data(), message.size(),
                                                fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port));
    done_promise->set_value(send_result ? fiber::common::IoErr::None : send_result.error());
    client->close();
    delete client;
}

DetachedTask wait_writable_succeeds(fiber::event::EventLoop *loop,
                                    std::promise<fiber::common::IoResult<void>> *done_promise) {
    auto *socket = new fiber::net::UdpSocket(*loop);
    auto bind_result = socket->bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), {});
    if (!bind_result) {
        done_promise->set_value(std::unexpected(bind_result.error()));
        delete socket;
        co_return;
    }

    auto result = co_await socket->wait_writable(std::chrono::seconds(1));
    socket->close();
    delete socket;
    done_promise->set_value(result);
}

DetachedTask register_udp_read_callback(fiber::net::UdpSocket *socket, ReadCallbackContext *ctx,
                                        std::promise<fiber::common::IoErr> *registered_promise) {
    const fiber::common::IoErr err = socket->set_read_callback(&on_udp_read_ready, ctx);
    registered_promise->set_value(err);
    if (err != fiber::common::IoErr::None) {
        ctx->completed = true;
        socket->close();
        ctx->done_promise->set_value(err);
    }
    co_return;
}

} // namespace

TEST(UdpSocketTest, RecvFromSendToRoundTrip) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::common::IoErr> server_promise;
    std::promise<fiber::common::IoErr> client_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    auto client_future = client_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() { return server_echo(&group.at(0), &port_promise, &server_promise); });

    uint16_t port = port_future.get();
    if (port == 0) {
        group.stop();
        group.join();
        FAIL() << "bind did not produce a valid port";
        return;
    }

    fiber::async::spawn(group.at(0), [&]() { return client_echo(&group.at(0), port, &client_promise); });

    if (server_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "server did not complete in time";
        return;
    }
    if (client_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "client did not complete in time";
        return;
    }

    EXPECT_EQ(server_future.get(), fiber::common::IoErr::None);
    EXPECT_EQ(client_future.get(), fiber::common::IoErr::None);

    group.stop();
    group.join();
}

#ifdef SO_REUSEPORT
TEST(UdpSocketTest, ReusePortAllowsSecondBind) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<fiber::common::IoErr> result_promise;
    auto result_future = result_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() { return bind_reuse_port(&group.at(0), &result_promise); });

    if (result_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "reuse_port bind did not complete in time";
        return;
    }

    fiber::common::IoErr result = result_future.get();
    if (result == fiber::common::IoErr::NotSupported) {
        group.stop();
        group.join();
        GTEST_SKIP() << "SO_REUSEPORT not supported";
    }

    EXPECT_EQ(result, fiber::common::IoErr::None);
    group.stop();
    group.join();
}
#endif

TEST(UdpSocketTest, RecvPacketReportsLocalAddressAndEcn) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<PacketMetadataOutcome> server_promise;
    std::promise<fiber::common::IoErr> client_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    auto client_future = client_promise.get_future();

    fiber::async::spawn(group.at(0),
                        [&]() { return server_recv_packet(&group.at(0), &port_promise, &server_promise); });

    uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    fiber::async::spawn(group.at(0),
                        [&]() { return client_send_packet_with_ecn(&group.at(0), port, &client_promise); });

    ASSERT_EQ(server_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_EQ(client_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    PacketMetadataOutcome server_result = server_future.get();
    EXPECT_EQ(client_future.get(), fiber::common::IoErr::None);
    EXPECT_EQ(server_result.err, fiber::common::IoErr::None);
    EXPECT_EQ(server_result.local.port(), port);
    EXPECT_EQ(server_result.local.to_string(),
              fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port).to_string());
    EXPECT_EQ(server_result.ecn, fiber::net::UdpEcn::Ect0);

    group.stop();
    group.join();
}

TEST(UdpSocketTest, TrySendPacketsSendsWholeBatch) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::common::IoErr> server_promise;
    std::promise<fiber::common::IoResult<size_t>> client_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    auto client_future = client_promise.get_future();

    fiber::async::spawn(group.at(0),
                        [&]() { return server_recv_two_packets(&group.at(0), &port_promise, &server_promise); });

    uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    fiber::async::spawn(group.at(0), [&]() { return client_batch_send_packets(&group.at(0), port, &client_promise); });

    ASSERT_EQ(server_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_EQ(client_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    auto client_result = client_future.get();
    ASSERT_TRUE(client_result.has_value()) << static_cast<int>(client_result.error());
    EXPECT_EQ(*client_result, 2U);
    EXPECT_EQ(server_future.get(), fiber::common::IoErr::None);

    group.stop();
    group.join();
}

TEST(UdpSocketTest, WaitReadableWakesOnIncomingPacket) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::common::IoErr> server_promise;
    std::promise<fiber::common::IoErr> client_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    auto client_future = client_promise.get_future();

    fiber::async::spawn(group.at(0),
                        [&]() { return server_wait_readable_then_recv(&group.at(0), &port_promise, &server_promise); });

    uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    fiber::async::spawn(group.at(0), [&]() { return client_sleep_then_send(&group.at(0), port, &client_promise); });

    ASSERT_EQ(server_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    ASSERT_EQ(client_future.wait_for(std::chrono::seconds(3)), std::future_status::ready);

    EXPECT_EQ(server_future.get(), fiber::common::IoErr::None);
    EXPECT_EQ(client_future.get(), fiber::common::IoErr::None);

    group.stop();
    group.join();
}

TEST(UdpSocketTest, ReadCallbackWakesOnIncomingPacket) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::net::UdpSocket server(group.at(0));
    ASSERT_TRUE(server.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), {}));

    std::promise<fiber::common::IoErr> registered_promise;
    std::promise<fiber::common::IoErr> callback_promise;
    std::promise<fiber::common::IoErr> client_promise;
    auto registered_future = registered_promise.get_future();
    auto callback_future = callback_promise.get_future();
    auto client_future = client_promise.get_future();
    ReadCallbackContext ctx{&server, &callback_promise};

    fiber::async::spawn(group.at(0), [&]() { return register_udp_read_callback(&server, &ctx, &registered_promise); });
    ASSERT_EQ(registered_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_EQ(registered_future.get(), fiber::common::IoErr::None);

    fiber::async::spawn(group.at(0), [&]() {
        return client_sleep_then_send(&group.at(0), server.local_addr().port(), &client_promise);
    });

    ASSERT_EQ(callback_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_EQ(client_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(callback_future.get(), fiber::common::IoErr::None);
    EXPECT_EQ(client_future.get(), fiber::common::IoErr::None);

    group.stop();
    group.join();
}

TEST(UdpSocketTest, WaitWritableSucceeds) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<fiber::common::IoResult<void>> result_promise;
    auto result_future = result_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() { return wait_writable_succeeds(&group.at(0), &result_promise); });

    ASSERT_EQ(result_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto result = result_future.get();
    ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());

    group.stop();
    group.join();
}
