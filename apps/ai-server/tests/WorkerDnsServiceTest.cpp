#include "../src/provider/WorkerDnsService.h"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <future>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <async/Spawn.h>
#include <event/EventLoopGroup.h>

namespace fiber::ai_server {
namespace {

using namespace std::chrono_literals;

struct ResolveOutcome {
    bool initialized = false;
    common::IoErr first_error = common::IoErr::Unknown;
    common::IoErr second_error = common::IoErr::Unknown;
    bool first_backoff_hit = false;
    bool second_backoff_hit = false;
};

std::uint16_t bind_dropping_dns_socket(int &fd) {
    fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return 0;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
        return 0;
    }
    socklen_t length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
        return 0;
    }
    return ntohs(address.sin_port);
}

std::size_t drain_datagrams(int fd) {
    std::size_t count = 0;
    std::uint8_t packet[512];
    for (;;) {
        const ssize_t received = ::recv(fd, packet, sizeof(packet), MSG_DONTWAIT);
        if (received >= 0) {
            ++count;
            continue;
        }
        EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
        return count;
    }
}

TEST(WorkerDnsServiceTest, SuppressesRepeatedTimeoutForNormalizedHostname) {
    int dns_fd = -1;
    const std::uint16_t dns_port = bind_dropping_dns_socket(dns_fd);
    ASSERT_GE(dns_fd, 0);
    ASSERT_NE(dns_port, 0);

    event::EventLoopGroup workers(1);
    WorkerDnsService service(WorkerDnsService::Options{
            .nameserver = net::SocketAddress(net::IpAddress::loopback_v4(), dns_port),
            .timeout = 20ms,
            .transient_failure_ttl = 500ms,
            .attempts = 1,
    });
    std::promise<ResolveOutcome> completed;
    auto future = completed.get_future();
    workers.start();
    async::spawn(workers.at(0), [&]() -> async::DetachedTask {
        ResolveOutcome outcome;
        outcome.initialized = co_await service.init(workers);
        if (outcome.initialized) {
            auto first = co_await service.resolve("EXAMPLE.invalid.");
            if (!first) {
                outcome.first_error = first.error().io_error;
                outcome.first_backoff_hit = first.error().backoff_hit;
            }
            auto second = co_await service.resolve("example.INVALID");
            if (!second) {
                outcome.second_error = second.error().io_error;
                outcome.second_backoff_hit = second.error().backoff_hit;
            }
        }
        co_await service.shutdown();
        completed.set_value(outcome);
    });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    const ResolveOutcome outcome = future.get();
    workers.stop();
    workers.join();

    EXPECT_TRUE(outcome.initialized);
    EXPECT_EQ(outcome.first_error, common::IoErr::TimedOut);
    EXPECT_FALSE(outcome.first_backoff_hit);
    EXPECT_EQ(outcome.second_error, common::IoErr::TimedOut);
    EXPECT_TRUE(outcome.second_backoff_hit);
    EXPECT_EQ(drain_datagrams(dns_fd), 2u);
    (void) ::close(dns_fd);
}

} // namespace
} // namespace fiber::ai_server
