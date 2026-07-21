#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <type_traits>
#include <utility>
#include <vector>

#include <async/Sleep.h>
#include <async/Spawn.h>
#include <common/IoError.h>
#include <event/EventLoopGroup.h>
#include <fiber/cat/Cat.h>
#include <net/IpAddress.h>
#include <net/SocketAddress.h>
#include <net/TcpListener.h>
#include <net/TcpStream.h>

namespace {

using namespace std::chrono_literals;

static_assert(sizeof(fiber::cat::MessageTrace) == sizeof(void *));
static_assert(!std::is_copy_constructible_v<fiber::cat::MessageTrace>);

fiber::common::IoResult<std::uint16_t> resolve_port(int fd) {
    sockaddr_storage bound{};
    socklen_t length = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &length) != 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    fiber::net::SocketAddress address;
    if (!fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<sockaddr *>(&bound), length, address)) {
        return std::unexpected(fiber::common::IoErr::NotSupported);
    }
    return address.port();
}

fiber::async::Task<fiber::common::IoResult<void>> read_exact(fiber::net::TcpStream &stream, std::uint8_t *data,
                                                             std::size_t size) noexcept {
    std::size_t offset = 0;
    while (offset < size) {
        auto read = co_await stream.read(data + offset, size - offset, 2s);
        if (!read) {
            co_return std::unexpected(read.error());
        }
        if (*read == 0) {
            co_return std::unexpected(fiber::common::IoErr::NotConnected);
        }
        offset += *read;
    }
    co_return fiber::common::IoResult<void>{};
}

fiber::async::DetachedTask
run_collector(fiber::event::EventLoop *loop, std::promise<std::uint16_t> *port_promise,
              std::promise<fiber::common::IoResult<std::vector<std::uint8_t>>> *frame_promise) {
    fiber::net::TcpListener listener(*loop);
    auto bound = listener.bind({fiber::net::IpAddress::loopback_v4(), 0}, {});
    if (!bound) {
        port_promise->set_value(0);
        frame_promise->set_value(std::unexpected(bound.error()));
        co_return;
    }
    auto port = resolve_port(listener.fd());
    port_promise->set_value(port ? *port : 0);
    if (!port) {
        frame_promise->set_value(std::unexpected(port.error()));
        co_return;
    }

    auto accepted = co_await listener.accept();
    if (!accepted) {
        frame_promise->set_value(std::unexpected(accepted.error()));
        co_return;
    }
    fiber::net::TcpStream stream(*loop, accepted->release_fd(), accepted->take_peer());
    std::array<std::uint8_t, 4> prefix{};
    auto prefix_result = co_await read_exact(stream, prefix.data(), prefix.size());
    if (!prefix_result) {
        stream.close();
        frame_promise->set_value(std::unexpected(prefix_result.error()));
        co_return;
    }
    const std::size_t payload_size = static_cast<std::size_t>(prefix[0]) << 24 |
                                     static_cast<std::size_t>(prefix[1]) << 16 |
                                     static_cast<std::size_t>(prefix[2]) << 8 | prefix[3];
    if (payload_size == 0 || payload_size > 2 * 1024 * 1024) {
        stream.close();
        frame_promise->set_value(std::unexpected(fiber::common::IoErr::MessageTooLarge));
        co_return;
    }
    std::vector<std::uint8_t> frame(prefix.begin(), prefix.end());
    frame.resize(prefix.size() + payload_size);
    auto payload_result = co_await read_exact(stream, frame.data() + prefix.size(), payload_size);
    stream.close();
    listener.close();
    if (!payload_result) {
        frame_promise->set_value(std::unexpected(payload_result.error()));
        co_return;
    }
    frame_promise->set_value(std::move(frame));
}

struct ClientOutcome {
    bool created = false;
    bool started = false;
    bool trace_created = false;
    bool trace_frozen = false;
    bool stopped = false;
    fiber::cat::CatClientStats stats;
};

struct ProducerState {
    std::atomic_bool done{false};
    std::atomic_bool trace_created{false};
    std::atomic_bool trace_frozen{false};
};

fiber::async::DetachedTask run_trace_producer(fiber::cat::CatClient *client, ProducerState *state) {
    auto trace_result = fiber::cat::MessageTrace::create(*client, {}, {.message_id = "m-1"});
    if (trace_result) {
        fiber::cat::MessageTrace trace = std::move(*trace_result);
        state->trace_created.store(true, std::memory_order_release);
        auto root_result = trace.create_transaction("URL", "/orders");
        if (root_result) {
            fiber::cat::Transaction root = std::move(*root_result);
            (void) root.add_data("method", "GET");
            (void) root.log_event("Cache", "miss");
            (void) root.complete();
            auto late = trace.create_event("Late", "ignored");
            state->trace_frozen.store(!trace.valid() && !late && late.error() == fiber::cat::RecordError::Completed,
                                      std::memory_order_release);
        }
    }
    state->done.store(true, std::memory_order_release);
    co_return;
}

fiber::async::DetachedTask run_client(fiber::event::EventLoop *loop, std::uint16_t collector_port,
                                      std::promise<fiber::cat::CatClient *> *ready_promise, ProducerState *producer,
                                      std::promise<ClientOutcome> *outcome_promise) {
    ClientOutcome outcome;
    bool ready_published = false;
    auto publish_ready = [&](fiber::cat::CatClient *client) {
        if (!ready_published) {
            ready_promise->set_value(client);
            ready_published = true;
        }
    };
    fiber::cat::CatClientConfigParams params{
            .app_key = "checkout",
            .hostname = "host-a",
            .ip = "127.0.0.1",
            .thread_group_name = "worker",
            .thread_id = "7",
            .thread_name = "sender",
    };
    params.bootstrap_collectors.emplace_back(fiber::net::IpAddress::loopback_v4(), collector_port);
    auto config = fiber::cat::CatClientConfig::create(std::move(params));
    if (!config) {
        publish_ready(nullptr);
        outcome_promise->set_value(outcome);
        co_return;
    }

    fiber::cat::CatClientOptions options;
    options.collector_connect_timeout = 1s;
    options.collector_write_timeout = 1s;
    options.shutdown_drain_timeout = 1s;
    auto created = fiber::cat::CatClient::create(*loop, std::move(*config), options);
    if (!created) {
        publish_ready(nullptr);
        outcome_promise->set_value(outcome);
        co_return;
    }
    std::unique_ptr<fiber::cat::CatClient> client = std::move(*created);
    outcome.created = true;
    auto started = client->start();
    if (!started) {
        publish_ready(nullptr);
        outcome_promise->set_value(outcome);
        co_return;
    }
    outcome.started = true;
    publish_ready(client.get());

    const auto producer_deadline = loop->now() + 2s;
    while (!producer->done.load(std::memory_order_acquire) && loop->now() < producer_deadline) {
        co_await fiber::async::sleep(1ms);
    }
    outcome.trace_created = producer->trace_created.load(std::memory_order_acquire);
    outcome.trace_frozen = producer->trace_frozen.load(std::memory_order_acquire);

    const auto deadline = loop->now() + 2s;
    while (client->stats().sent_messages == 0 && loop->now() < deadline) {
        co_await fiber::async::sleep(1ms);
    }
    outcome.stats = client->stats();
    co_await client->shutdown();
    outcome.stopped = client->state() == fiber::cat::CatClientState::Stopped;
    outcome_promise->set_value(outcome);
}

TEST(CatClientTest, SendsCompletedMessageTraceAsOneNt1Frame) {
    fiber::event::EventLoopGroup group(2);
    group.start();

    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::promise<fiber::common::IoResult<std::vector<std::uint8_t>>> frame_promise;
    auto frame_future = frame_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return run_collector(&group.at(0), &port_promise, &frame_promise); });

    const bool port_ready = port_future.wait_for(2s) == std::future_status::ready;
    if (!port_ready) {
        group.stop();
        group.join();
        FAIL() << "collector did not publish its port";
    }
    const std::uint16_t port = port_future.get();
    if (port == 0) {
        group.stop();
        group.join();
        FAIL() << "collector failed to bind";
    }

    std::promise<ClientOutcome> outcome_promise;
    auto outcome_future = outcome_promise.get_future();
    std::promise<fiber::cat::CatClient *> ready_promise;
    auto ready_future = ready_promise.get_future();
    ProducerState producer;
    fiber::async::spawn(group.at(0),
                        [&]() { return run_client(&group.at(0), port, &ready_promise, &producer, &outcome_promise); });

    const bool client_ready = ready_future.wait_for(2s) == std::future_status::ready;
    if (!client_ready) {
        group.stop();
        group.join();
        FAIL() << "client did not start";
    }
    fiber::cat::CatClient *client = ready_future.get();
    if (!client) {
        group.stop();
        group.join();
        FAIL() << "client failed to start";
    }
    fiber::async::spawn(group.at(1), [&]() { return run_trace_producer(client, &producer); });

    const bool outcome_ready = outcome_future.wait_for(5s) == std::future_status::ready;
    const bool frame_ready = frame_future.wait_for(5s) == std::future_status::ready;
    group.stop();
    group.join();
    ASSERT_TRUE(outcome_ready);
    ASSERT_TRUE(frame_ready);

    const ClientOutcome outcome = outcome_future.get();
    EXPECT_TRUE(outcome.created);
    EXPECT_TRUE(outcome.started);
    EXPECT_TRUE(outcome.trace_created);
    EXPECT_TRUE(outcome.trace_frozen);
    EXPECT_TRUE(outcome.stopped);
    EXPECT_EQ(outcome.stats.submitted_messages, 1);
    EXPECT_EQ(outcome.stats.sent_messages, 1);
    EXPECT_EQ(outcome.stats.dropped_partial_frame, 0);

    auto frame = frame_future.get();
    ASSERT_TRUE(frame);
    ASSERT_GE(frame->size(), 8);
    const std::size_t declared = static_cast<std::size_t>((*frame)[0]) << 24 |
                                 static_cast<std::size_t>((*frame)[1]) << 16 |
                                 static_cast<std::size_t>((*frame)[2]) << 8 | (*frame)[3];
    EXPECT_EQ(declared + 4, frame->size());
    EXPECT_EQ((*frame)[4], 'N');
    EXPECT_EQ((*frame)[5], 'T');
    EXPECT_EQ((*frame)[6], '1');
}

} // namespace
