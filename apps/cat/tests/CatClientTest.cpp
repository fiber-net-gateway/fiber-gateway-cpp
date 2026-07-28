#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <async/Sleep.h>
#include <async/Spawn.h>
#include <common/IoError.h>
#include <common/mem/IoBuf.h>
#include <dns/DnsCache2.h>
#include <dns/DnsResolver.h>
#include <event/EventLoopGroup.h>
#include <fiber/cat/Cat.h>
#include <net/IpAddress.h>
#include <net/SocketAddress.h>
#include <net/TcpListener.h>
#include <net/TcpStream.h>

#include "CatClientCore.h"

namespace {

using namespace std::chrono_literals;

static_assert(sizeof(fiber::cat::MessageTrace) == sizeof(void *));
static_assert(!std::is_copy_constructible_v<fiber::cat::MessageTrace>);
static_assert(sizeof(fiber::cat::PropagationContext) == sizeof(void *));
static_assert(std::is_copy_constructible_v<fiber::cat::PropagationContext>);

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

fiber::async::Task<fiber::common::IoResult<void>> write_exact(fiber::net::TcpStream &stream, const char *data,
                                                              std::size_t size) noexcept {
    std::size_t offset = 0;
    while (offset < size) {
        auto written = co_await stream.write(data + offset, size - offset, 2s);
        if (!written) {
            co_return std::unexpected(written.error());
        }
        if (*written == 0) {
            co_return std::unexpected(fiber::common::IoErr::BrokenPipe);
        }
        offset += *written;
    }
    co_return fiber::common::IoResult<void>{};
}

struct RouterReply {
    unsigned status = 200;
    std::string body;
    std::optional<std::size_t> content_length;
    std::chrono::milliseconds response_delay{0};
};

fiber::async::DetachedTask run_fake_router(fiber::event::EventLoop *loop, std::vector<RouterReply> replies,
                                           std::promise<std::uint16_t> *port_promise,
                                           std::promise<std::size_t> *requests_promise) {
    fiber::net::TcpListener listener(*loop);
    auto bound = listener.bind({fiber::net::IpAddress::loopback_v4(), 0}, {});
    if (!bound) {
        port_promise->set_value(0);
        requests_promise->set_value(0);
        co_return;
    }
    auto port = resolve_port(listener.fd());
    port_promise->set_value(port ? *port : 0);
    if (!port) {
        requests_promise->set_value(0);
        co_return;
    }

    std::size_t served = 0;
    for (const RouterReply &reply: replies) {
        auto accepted = co_await listener.accept();
        if (!accepted) {
            break;
        }
        fiber::net::TcpStream stream(*loop, accepted->release_fd(), accepted->take_peer());
        std::array<char, 4096> request{};
        std::size_t used = 0;
        bool complete = false;
        while (used < request.size()) {
            auto read = co_await stream.read(request.data() + used, request.size() - used, 2s);
            if (!read || *read == 0) {
                break;
            }
            used += *read;
            complete = std::string_view(request.data(), used).find("\r\n\r\n") != std::string_view::npos;
            if (complete) {
                break;
            }
        }
        if (!complete) {
            stream.close();
            break;
        }
        if (reply.response_delay > 0ms) {
            co_await fiber::async::sleep(reply.response_delay);
        }
        std::string response =
                "HTTP/1.1 " + std::to_string(reply.status) + (reply.status == 200 ? " OK\r\n" : " Error\r\n");
        response.append("Content-Length: ");
        response.append(std::to_string(reply.content_length.value_or(reply.body.size())));
        response.append("\r\nConnection: close\r\n\r\n");
        response.append(reply.body);
        auto written = co_await write_exact(stream, response.data(), response.size());
        stream.close();
        ++served;
        if (!written) {
            continue;
        }
    }
    listener.close();
    requests_promise->set_value(served);
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

fiber::async::DetachedTask run_multi_frame_collector(
        fiber::event::EventLoop *loop, std::size_t target_frames, std::promise<std::uint16_t> *port_promise,
        std::promise<fiber::common::IoResult<std::vector<std::vector<std::uint8_t>>>> *frames_promise) {
    fiber::net::TcpListener listener(*loop);
    auto bound = listener.bind({fiber::net::IpAddress::loopback_v4(), 0}, {});
    if (!bound) {
        port_promise->set_value(0);
        frames_promise->set_value(std::unexpected(bound.error()));
        co_return;
    }
    auto port = resolve_port(listener.fd());
    port_promise->set_value(port ? *port : 0);
    if (!port) {
        frames_promise->set_value(std::unexpected(port.error()));
        co_return;
    }
    auto accepted = co_await listener.accept();
    if (!accepted) {
        frames_promise->set_value(std::unexpected(accepted.error()));
        co_return;
    }
    fiber::net::TcpStream stream(*loop, accepted->release_fd(), accepted->take_peer());
    std::vector<std::vector<std::uint8_t>> frames;
    while (frames.size() < target_frames) {
        std::array<std::uint8_t, 4> prefix{};
        auto prefix_result = co_await read_exact(stream, prefix.data(), prefix.size());
        if (!prefix_result) {
            stream.close();
            frames_promise->set_value(std::unexpected(prefix_result.error()));
            co_return;
        }
        const std::size_t payload_size = static_cast<std::size_t>(prefix[0]) << 24 |
                                         static_cast<std::size_t>(prefix[1]) << 16 |
                                         static_cast<std::size_t>(prefix[2]) << 8 | prefix[3];
        if (payload_size == 0 || payload_size > 2 * 1024 * 1024) {
            stream.close();
            frames_promise->set_value(std::unexpected(fiber::common::IoErr::MessageTooLarge));
            co_return;
        }
        frames.emplace_back(prefix.begin(), prefix.end());
        frames.back().resize(prefix.size() + payload_size);
        auto payload_result = co_await read_exact(stream, frames.back().data() + prefix.size(), payload_size);
        if (!payload_result) {
            stream.close();
            frames_promise->set_value(std::unexpected(payload_result.error()));
            co_return;
        }
    }
    stream.close();
    listener.close();
    frames_promise->set_value(std::move(frames));
}

struct ClientOutcome {
    bool created = false;
    bool started = false;
    bool trace_created = false;
    bool context_verified = false;
    bool trace_frozen = false;
    bool stopped = false;
    fiber::cat::CatClientStats stats;
};

struct ProducerState {
    std::atomic_bool done{false};
    std::atomic_bool trace_created{false};
    std::atomic_bool context_verified{false};
    std::atomic_bool trace_frozen{false};
};

fiber::async::DetachedTask run_trace_producer(fiber::cat::CatClient *client, ProducerState *state) {
    auto trace_result = fiber::cat::MessageTrace::create(*client);
    if (trace_result) {
        fiber::cat::MessageTrace trace = std::move(*trace_result);
        state->trace_created.store(true, std::memory_order_release);
        bool visited = false;
        const bool put_context = trace.put_context("tenant", "blue") == fiber::cat::RecordError::None;
        auto context = trace.get_context("tenant");
        const bool got_context = context && context->has_value() && **context == "blue";
        const bool iterated = trace.for_each_context([&](std::string_view key, std::string_view value) noexcept {
            visited = key == "tenant" && value == "blue";
            return true;
        }) == fiber::cat::RecordError::None;
        auto propagation = trace.propagation_context();
        auto outbound = propagation ? client->create_remote_context(*propagation, "inventory")
                                    : std::expected<fiber::cat::PropagationContext, fiber::cat::RecordError>(
                                              std::unexpected(fiber::cat::RecordError::InvalidContext));
        auto inherited_trace = fiber::cat::MessageTrace::create(*client, {},
                                                                {
                                                                        .root_message_id = "upstream-root",
                                                                        .parent_message_id = "upstream-parent",
                                                                });
        auto inherited = inherited_trace ? inherited_trace->propagation_context()
                                         : std::expected<fiber::cat::PropagationContext, fiber::cat::RecordError>(
                                                   std::unexpected(fiber::cat::RecordError::InvalidContext));
        const bool inherited_context = inherited && !inherited->message_id().empty() &&
                                       inherited->root_message_id() == "upstream-root" &&
                                       inherited->parent_message_id() == "upstream-parent";
        const bool propagated = propagation && outbound &&
                                propagation->message_id().starts_with("checkout-7f000001-") &&
                                propagation->root_message_id().empty() && propagation->parent_message_id().empty() &&
                                outbound->message_id().starts_with("inventory-7f000001-") &&
                                outbound->root_message_id() == propagation->message_id() &&
                                outbound->parent_message_id() == propagation->message_id() && inherited_context;
        state->context_verified.store(put_context && got_context && iterated && visited && propagated,
                                      std::memory_order_release);
        auto root_result = trace.create_transaction("URL", "/orders");
        if (root_result) {
            fiber::cat::Transaction root = std::move(*root_result);
            (void) root.add_data("method", "GET");
            (void) root.log_event("Cache", "miss");
            (void) root.complete();
            auto late = trace.create_event("Late", "ignored");
            auto expired_context = trace.get_context("tenant");
            state->trace_frozen.store(!trace.valid() && !late && late.error() == fiber::cat::RecordError::Completed &&
                                              !expired_context &&
                                              expired_context.error() == fiber::cat::RecordError::Completed,
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
    outcome.context_verified = producer->context_verified.load(std::memory_order_acquire);
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
    EXPECT_TRUE(outcome.context_verified);
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

fiber::async::DetachedTask run_heartbeat_client(fiber::event::EventLoop *loop, std::uint16_t collector_port,
                                                std::promise<fiber::cat::CatClientStats> *promise) {
    fiber::cat::CatClientConfigParams params{
            .app_key = "heartbeat",
            .hostname = "host",
            .ip = "127.0.0.1",
    };
    params.bootstrap_collectors.emplace_back(fiber::net::IpAddress::loopback_v4(), collector_port);
    auto config = fiber::cat::CatClientConfig::create(std::move(params));
    if (!config) {
        promise->set_value({});
        co_return;
    }
    fiber::cat::CatClientOptions options;
    options.heartbeat_initial_delay = 0ms;
    options.heartbeat_interval = 5ms;
    options.aggregation_flush_interval = 1h;
    options.collector_connect_timeout = 1s;
    options.collector_write_timeout = 1s;
    options.shutdown_drain_timeout = 100ms;
    auto created = fiber::cat::CatClient::create(*loop, std::move(*config), options);
    if (!created || !(*created)->start()) {
        promise->set_value({});
        co_return;
    }
    std::unique_ptr<fiber::cat::CatClient> client = std::move(*created);
    const auto deadline = loop->now() + 2s;
    while (client->stats().heartbeat_sent < 2 && loop->now() < deadline) {
        co_await fiber::async::sleep(1ms);
    }
    const auto stats = client->stats();
    co_await client->shutdown();
    promise->set_value(stats);
}

TEST(CatClientTest, SendsStartupAndRepeatedHeartbeatFrames) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::promise<fiber::common::IoResult<std::vector<std::vector<std::uint8_t>>>> frames_promise;
    auto frames_future = frames_promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&] { return run_multi_frame_collector(&group.at(0), 3, &port_promise, &frames_promise); });
    if (port_future.wait_for(2s) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "heartbeat collector did not publish its port";
    }
    const std::uint16_t port = port_future.get();
    if (port == 0) {
        group.stop();
        group.join();
        FAIL() << "heartbeat collector failed to bind";
    }

    std::promise<fiber::cat::CatClientStats> stats_promise;
    auto stats_future = stats_promise.get_future();
    fiber::async::spawn(group.at(0), [&] { return run_heartbeat_client(&group.at(0), port, &stats_promise); });
    const bool stats_ready = stats_future.wait_for(5s) == std::future_status::ready;
    const bool frames_ready = frames_future.wait_for(5s) == std::future_status::ready;
    group.stop();
    group.join();
    ASSERT_TRUE(stats_ready);
    ASSERT_TRUE(frames_ready);
    const auto stats = stats_future.get();
    EXPECT_GE(stats.heartbeat_submitted, 2);
    EXPECT_GE(stats.heartbeat_sent, 2);
    EXPECT_EQ(stats.heartbeat_dropped, 0);
    EXPECT_EQ(stats.heartbeat_provider_failures, 0);

    auto frames = frames_future.get();
    ASSERT_TRUE(frames);
    ASSERT_EQ(frames->size(), 3);
    const auto contains = [](const std::vector<std::uint8_t> &frame, std::string_view text) {
        return std::search(frame.begin(), frame.end(), text.begin(), text.end()) != frame.end();
    };
    EXPECT_TRUE(contains((*frames)[0], "Reboot"));
    EXPECT_TRUE(contains((*frames)[1], "Status"));
    EXPECT_TRUE(contains((*frames)[1], "Heartbeat"));
    EXPECT_TRUE(contains((*frames)[1], "system.process"));
    EXPECT_TRUE(contains((*frames)[1], "mem.memtotal"));
    EXPECT_TRUE(contains((*frames)[1], "process.rss.bytes"));
    EXPECT_TRUE(contains((*frames)[2], "Heartbeat"));
}

fiber::async::DetachedTask run_router_refresh_client(fiber::event::EventLoop *loop, std::uint16_t router_port,
                                                     std::promise<fiber::cat::CatClientStats> *promise) {
    fiber::cat::CatClientConfigParams params{
            .app_key = "router-test",
            .hostname = "host",
            .ip = "127.0.0.1",
    };
    params.routers.push_back({.host = "127.0.0.1", .port = router_port});
    params.bootstrap_collectors.emplace_back(fiber::net::IpAddress::loopback_v4(), 1);
    auto config = fiber::cat::CatClientConfig::create(std::move(params));
    if (!config) {
        promise->set_value({});
        co_return;
    }
    fiber::cat::CatClientOptions options;
    options.enable_heartbeat = false;
    options.max_router_response_bytes = 128;
    options.router_connect_timeout = 100ms;
    options.router_request_timeout = 100ms;
    options.router_refresh_interval = 5ms;
    options.collector_connect_timeout = 1ms;
    options.reconnect_initial_delay = 1ms;
    options.reconnect_max_delay = 5ms;
    options.aggregation_flush_interval = 1h;
    options.shutdown_drain_timeout = 0ms;
    auto created = fiber::cat::CatClient::create(*loop, std::move(*config), options);
    if (!created || !(*created)->start()) {
        promise->set_value({});
        co_return;
    }
    std::unique_ptr<fiber::cat::CatClient> client = std::move(*created);
    const auto deadline = loop->now() + 2s;
    while ((client->stats().router_successes < 2 || client->stats().router_failures < 3) && loop->now() < deadline) {
        co_await fiber::async::sleep(1ms);
    }
    const auto stats = client->stats();
    co_await client->shutdown();
    promise->set_value(stats);
}

TEST(CatClientTest, RouterFailuresPreserveSnapshotAndDynamicBlockSampleRecover) {
    fiber::event::EventLoopGroup group(2);
    group.start();
    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::promise<std::size_t> requests_promise;
    auto requests_future = requests_promise.get_future();
    std::vector<RouterReply> replies;
    replies.push_back({.status = 503, .body = "unavailable"});
    replies.push_back({.body = "{"});
    replies.push_back({.body = std::string(256, 'x')});
    replies.push_back({.body = "{", .content_length = 64});
    replies.push_back({.body = R"({"kvs":{"routers":"","sample":1,"block":true}})", .response_delay = 150ms});
    replies.push_back({.body = R"({"kvs":{"routers":"","sample":0.5,"block":true}})"});
    replies.push_back({.body = R"({"kvs":{"routers":"127.0.0.1:1","sample":0,"block":false}})"});
    fiber::async::spawn(group.at(0), [&] {
        return run_fake_router(&group.at(0), std::move(replies), &port_promise, &requests_promise);
    });
    if (port_future.wait_for(2s) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "fake router did not publish its port";
    }
    const std::uint16_t port = port_future.get();
    if (port == 0) {
        group.stop();
        group.join();
        FAIL() << "fake router failed to bind";
    }

    std::promise<fiber::cat::CatClientStats> stats_promise;
    auto stats_future = stats_promise.get_future();
    fiber::async::spawn(group.at(1), [&] { return run_router_refresh_client(&group.at(1), port, &stats_promise); });
    const bool stats_ready = stats_future.wait_for(5s) == std::future_status::ready;
    const bool requests_ready = requests_future.wait_for(5s) == std::future_status::ready;
    group.stop();
    group.join();
    ASSERT_TRUE(stats_ready);
    ASSERT_TRUE(requests_ready);
    EXPECT_EQ(requests_future.get(), 7);
    const auto stats = stats_future.get();
    EXPECT_GE(stats.router_failures, 5);
    EXPECT_GE(stats.router_successes, 2);
    EXPECT_EQ(stats.router_blocks, 1);
    EXPECT_EQ(stats.router_unblocks, 1);
    EXPECT_GE(stats.router_sample_changes, 2);
    EXPECT_GE(stats.collector_set_changes, 2);
    EXPECT_GT(stats.connect_failures, 0);
}

fiber::async::DetachedTask run_dns_router_client(fiber::event::EventLoop *loop, std::uint16_t router_port,
                                                 std::promise<fiber::cat::CatClientStats> *promise) {
    fiber::dns::SharedDnsCache2 cache;
    fiber::dns::DnsResolverLocal local;
    fiber::dns::DnsResolver resolver;
    fiber::dns::AddressResolver address_resolver;
    if (!cache.init(*loop)) {
        promise->set_value({});
        co_return;
    }

    const std::array addresses{fiber::net::IpAddress::v4({127, 0, 0, 2}), fiber::net::IpAddress::loopback_v4()};
    const fiber::dns::DnsCacheKey key{.normalized_name = "router.test",
                                      .hash = fiber::dns::dns_cache_hash("router.test")};
    const auto expires = loop->now() + 1min;
    const auto v4 =
            cache.upsert_address_set(key, fiber::net::IpFamily::V4, addresses.data(), addresses.size(), expires);
    const auto v6 = cache.upsert_address_set(key, fiber::net::IpFamily::V6, nullptr, 0, expires);

    fiber::dns::DnsClient::Options dns_options;
    dns_options.server = {fiber::net::IpAddress::loopback_v4(), 1};
    const bool resolver_ready = v4 == fiber::common::IoErr::None && v6 == fiber::common::IoErr::None &&
                                local.init(*loop, cache, dns_options, {}) && resolver.init(local, {}) &&
                                address_resolver.init(resolver, {});
    if (!resolver_ready) {
        address_resolver.release();
        resolver.release();
        local.release();
        cache.shutdown();
        promise->set_value({});
        co_return;
    }

    fiber::cat::CatClientConfigParams params{
            .app_key = "dns-router-test",
            .hostname = "host",
            .ip = "127.0.0.1",
    };
    params.routers.push_back({.host = "router.test", .port = router_port});
    auto config = fiber::cat::CatClientConfig::create(std::move(params));
    if (!config) {
        address_resolver.release();
        resolver.release();
        local.release();
        cache.shutdown();
        promise->set_value({});
        co_return;
    }
    fiber::cat::CatClientOptions options;
    options.enable_heartbeat = false;
    options.router_connect_timeout = 50ms;
    options.router_request_timeout = 100ms;
    options.router_refresh_interval = 1h;
    options.aggregation_flush_interval = 1h;
    options.shutdown_drain_timeout = 0ms;
    auto created = fiber::cat::CatClient::create(*loop, std::move(*config), options, &address_resolver);
    if (!created || !(*created)->start()) {
        address_resolver.release();
        resolver.release();
        local.release();
        cache.shutdown();
        promise->set_value({});
        co_return;
    }

    std::unique_ptr<fiber::cat::CatClient> client = std::move(*created);
    const auto deadline = loop->now() + 2s;
    while (client->stats().router_successes == 0 && loop->now() < deadline) {
        co_await fiber::async::sleep(1ms);
    }
    const auto stats = client->stats();
    co_await client->shutdown();
    client.reset();
    address_resolver.release();
    resolver.release();
    local.release();
    cache.shutdown();
    promise->set_value(stats);
}

TEST(CatClientTest, DnsRouterTriesMultipleResolvedEndpoints) {
    fiber::event::EventLoopGroup group(2);
    group.start();
    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::promise<std::size_t> requests_promise;
    auto requests_future = requests_promise.get_future();
    std::vector<RouterReply> replies;
    replies.push_back({.body = R"({"kvs":{"routers":"","sample":1,"block":true}})"});
    fiber::async::spawn(group.at(0), [&] {
        return run_fake_router(&group.at(0), std::move(replies), &port_promise, &requests_promise);
    });
    if (port_future.wait_for(2s) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "DNS fake router did not publish its port";
    }
    const std::uint16_t port = port_future.get();
    if (port == 0) {
        group.stop();
        group.join();
        FAIL() << "DNS fake router failed to bind";
    }

    std::promise<fiber::cat::CatClientStats> stats_promise;
    auto stats_future = stats_promise.get_future();
    fiber::async::spawn(group.at(1), [&] { return run_dns_router_client(&group.at(1), port, &stats_promise); });
    const bool stats_ready = stats_future.wait_for(5s) == std::future_status::ready;
    const bool requests_ready = requests_future.wait_for(5s) == std::future_status::ready;
    group.stop();
    group.join();
    ASSERT_TRUE(stats_ready);
    ASSERT_TRUE(requests_ready);
    EXPECT_EQ(requests_future.get(), 1);
    const auto stats = stats_future.get();
    EXPECT_EQ(stats.router_successes, 1);
    EXPECT_EQ(stats.router_failures, 0);
    EXPECT_EQ(stats.router_blocks, 1);
}

struct BudgetOutcome {
    bool started = false;
    bool stopped = false;
    fiber::cat::CatClientStats before_shutdown;
    fiber::cat::CatClientStats after_shutdown;
};

fiber::mem::IoBuf make_frame(std::size_t size, std::uint8_t value) {
    fiber::mem::IoBuf frame = fiber::mem::IoBuf::allocate(size);
    if (!frame) {
        return {};
    }
    std::fill_n(frame.writable_data(), size, value);
    frame.commit(size);
    return frame;
}

fiber::async::DetachedTask
run_raw_collector(fiber::event::EventLoop *loop, std::size_t bytes, std::promise<std::uint16_t> *port_promise,
                  std::promise<fiber::common::IoResult<std::vector<std::uint8_t>>> *promise) {
    fiber::net::TcpListener listener(*loop);
    auto bound = listener.bind({fiber::net::IpAddress::loopback_v4(), 0}, {});
    if (!bound) {
        port_promise->set_value(0);
        promise->set_value(std::unexpected(bound.error()));
        co_return;
    }
    auto port = resolve_port(listener.fd());
    port_promise->set_value(port ? *port : 0);
    if (!port) {
        promise->set_value(std::unexpected(port.error()));
        co_return;
    }
    auto accepted = co_await listener.accept();
    if (!accepted) {
        promise->set_value(std::unexpected(accepted.error()));
        co_return;
    }
    fiber::net::TcpStream stream(*loop, accepted->release_fd(), accepted->take_peer());
    std::vector<std::uint8_t> result(bytes);
    auto read = co_await read_exact(stream, result.data(), result.size());
    stream.close();
    listener.close();
    if (!read) {
        promise->set_value(std::unexpected(read.error()));
    } else {
        promise->set_value(std::move(result));
    }
}

fiber::async::DetachedTask run_slow_ordered_collector(fiber::event::EventLoop *loop, std::size_t normal_bytes,
                                                      std::size_t priority_bytes,
                                                      std::promise<std::uint16_t> *port_promise,
                                                      std::promise<bool> *ordered_promise) {
    fiber::net::TcpListener listener(*loop);
    auto bound = listener.bind({fiber::net::IpAddress::loopback_v4(), 0}, {});
    if (!bound) {
        port_promise->set_value(0);
        ordered_promise->set_value(false);
        co_return;
    }
    auto port = resolve_port(listener.fd());
    port_promise->set_value(port ? *port : 0);
    if (!port) {
        ordered_promise->set_value(false);
        co_return;
    }
    auto accepted = co_await listener.accept();
    if (!accepted) {
        ordered_promise->set_value(false);
        co_return;
    }
    fiber::net::TcpStream stream(*loop, accepted->release_fd(), accepted->take_peer());
    co_await fiber::async::sleep(100ms);
    std::array<std::uint8_t, 64 * 1024> chunk{};
    std::size_t offset = 0;
    bool ordered = true;
    const std::size_t total = normal_bytes + priority_bytes;
    while (offset < total) {
        auto read = co_await stream.read(chunk.data(), std::min(chunk.size(), total - offset), 2s);
        if (!read || *read == 0) {
            ordered = false;
            break;
        }
        for (std::size_t index = 0; index < *read; ++index) {
            const std::uint8_t expected = offset + index < normal_bytes ? 1 : 2;
            if (chunk[index] != expected) {
                ordered = false;
            }
        }
        offset += *read;
    }
    stream.close();
    listener.close();
    ordered_promise->set_value(ordered && offset == total);
}

fiber::async::DetachedTask run_reset_collector(fiber::event::EventLoop *loop, std::promise<std::uint16_t> *port_promise,
                                               std::promise<void> *closed_promise) {
    fiber::net::TcpListener listener(*loop);
    auto bound = listener.bind({fiber::net::IpAddress::loopback_v4(), 0}, {});
    if (!bound) {
        port_promise->set_value(0);
        closed_promise->set_value();
        co_return;
    }
    auto port = resolve_port(listener.fd());
    port_promise->set_value(port ? *port : 0);
    if (!port) {
        closed_promise->set_value();
        co_return;
    }
    auto accepted = co_await listener.accept();
    if (!accepted) {
        closed_promise->set_value();
        co_return;
    }
    fiber::net::TcpStream stream(*loop, accepted->release_fd(), accepted->take_peer());
    co_await fiber::async::sleep(50ms);
    std::uint8_t byte = 0;
    (void) co_await stream.read(&byte, 1, 1s);
    linger reset{.l_onoff = 1, .l_linger = 0};
    (void) ::setsockopt(stream.fd(), SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
    stream.close();
    listener.close();
    closed_promise->set_value();
}

fiber::async::DetachedTask run_priority_client(fiber::event::EventLoop *loop, std::uint16_t port,
                                               std::promise<fiber::cat::CatClientStats> *promise) {
    fiber::cat::CatClientConfigParams params{
            .app_key = "priority",
            .hostname = "host",
            .ip = "127.0.0.1",
    };
    params.bootstrap_collectors.emplace_back(fiber::net::IpAddress::loopback_v4(), port);
    auto config = fiber::cat::CatClientConfig::create(std::move(params));
    if (!config) {
        promise->set_value({});
        co_return;
    }
    fiber::cat::CatClientOptions options;
    options.enable_heartbeat = false;
    options.collector_connect_timeout = 1s;
    options.shutdown_drain_timeout = 100ms;
    auto core = std::make_shared<fiber::cat::detail::CatClientCore>(*loop, std::move(*config), options, nullptr);
    if (!core->start()) {
        promise->set_value({});
        co_return;
    }
    (void) core->submit_encoded(make_frame(8, 1));
    (void) core->submit_encoded(make_frame(8, 2), fiber::cat::detail::FramePriority::Problem);
    const auto deadline = loop->now() + 2s;
    while (core->stats().sent_messages < 2 && loop->now() < deadline) {
        co_await fiber::async::sleep(1ms);
    }
    const auto stats = core->stats();
    co_await core->shutdown();
    promise->set_value(stats);
}

TEST(CatClientTest, SendsProblemFramesBeforeQueuedNormalFrames) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::promise<fiber::common::IoResult<std::vector<std::uint8_t>>> bytes_promise;
    auto bytes_future = bytes_promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&] { return run_raw_collector(&group.at(0), 16, &port_promise, &bytes_promise); });
    if (port_future.wait_for(2s) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "priority collector did not publish its port";
    }
    const std::uint16_t port = port_future.get();
    if (port == 0) {
        group.stop();
        group.join();
        FAIL() << "priority collector failed to bind";
    }
    std::promise<fiber::cat::CatClientStats> stats_promise;
    auto stats_future = stats_promise.get_future();
    fiber::async::spawn(group.at(0), [&] { return run_priority_client(&group.at(0), port, &stats_promise); });
    const bool stats_ready = stats_future.wait_for(5s) == std::future_status::ready;
    const bool bytes_ready = bytes_future.wait_for(5s) == std::future_status::ready;
    group.stop();
    group.join();
    ASSERT_TRUE(stats_ready);
    ASSERT_TRUE(bytes_ready);
    EXPECT_EQ(stats_future.get().sent_messages, 2);
    auto bytes = bytes_future.get();
    ASSERT_TRUE(bytes);
    ASSERT_EQ(bytes->size(), 16);
    EXPECT_TRUE(std::all_of(bytes->begin(), bytes->begin() + 8, [](std::uint8_t value) { return value == 2; }));
    EXPECT_TRUE(std::all_of(bytes->begin() + 8, bytes->end(), [](std::uint8_t value) { return value == 1; }));
}

fiber::async::DetachedTask run_would_block_order_client(fiber::event::EventLoop *loop, std::uint16_t port,
                                                        std::size_t normal_bytes, std::size_t priority_bytes,
                                                        std::promise<fiber::cat::CatClientStats> *promise) {
    fiber::cat::CatClientConfigParams params{
            .app_key = "would-block",
            .hostname = "host",
            .ip = "127.0.0.1",
    };
    params.bootstrap_collectors.emplace_back(fiber::net::IpAddress::loopback_v4(), port);
    auto config = fiber::cat::CatClientConfig::create(std::move(params));
    if (!config) {
        promise->set_value({});
        co_return;
    }
    fiber::cat::CatClientOptions options;
    options.enable_heartbeat = false;
    options.max_queued_bytes = 32 * 1024 * 1024;
    options.max_batch_bytes = 32 * 1024 * 1024;
    options.max_send_bytes_per_pump = 32 * 1024 * 1024;
    options.max_send_calls_per_pump = 64;
    options.collector_connect_timeout = 1s;
    options.collector_write_timeout = 2s;
    options.shutdown_drain_timeout = 100ms;
    auto core = std::make_shared<fiber::cat::detail::CatClientCore>(*loop, std::move(*config), options, nullptr);
    if (!core->start()) {
        promise->set_value({});
        co_return;
    }
    (void) core->submit_encoded(make_frame(normal_bytes, 1));
    const auto blocked_deadline = loop->now() + 2s;
    while (core->stats().write_would_block == 0 && loop->now() < blocked_deadline) {
        co_await fiber::async::sleep(1ms);
    }
    (void) core->submit_encoded(make_frame(priority_bytes, 2), fiber::cat::detail::FramePriority::Problem);
    const auto sent_deadline = loop->now() + 3s;
    while (core->stats().sent_messages < 2 && loop->now() < sent_deadline) {
        co_await fiber::async::sleep(1ms);
    }
    const auto stats = core->stats();
    co_await core->shutdown();
    promise->set_value(stats);
}

TEST(CatClientTest, WouldBlockRecoveryDoesNotPreemptPartiallyWrittenFrame) {
    constexpr std::size_t normal_bytes = 8 * 1024 * 1024;
    constexpr std::size_t priority_bytes = 8;
    fiber::event::EventLoopGroup group(1);
    group.start();
    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::promise<bool> ordered_promise;
    auto ordered_future = ordered_promise.get_future();
    fiber::async::spawn(group.at(0), [&] {
        return run_slow_ordered_collector(&group.at(0), normal_bytes, priority_bytes, &port_promise, &ordered_promise);
    });
    if (port_future.wait_for(2s) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "slow collector did not publish its port";
    }
    const std::uint16_t port = port_future.get();
    if (port == 0) {
        group.stop();
        group.join();
        FAIL() << "slow collector failed to bind";
    }
    std::promise<fiber::cat::CatClientStats> stats_promise;
    auto stats_future = stats_promise.get_future();
    fiber::async::spawn(group.at(0), [&] {
        return run_would_block_order_client(&group.at(0), port, normal_bytes, priority_bytes, &stats_promise);
    });
    const bool stats_ready = stats_future.wait_for(6s) == std::future_status::ready;
    const bool ordered_ready = ordered_future.wait_for(6s) == std::future_status::ready;
    group.stop();
    group.join();
    ASSERT_TRUE(stats_ready);
    ASSERT_TRUE(ordered_ready);
    EXPECT_TRUE(ordered_future.get());
    const auto stats = stats_future.get();
    EXPECT_GT(stats.write_would_block, 0);
    EXPECT_EQ(stats.sent_messages, 2);
    EXPECT_EQ(stats.dropped_partial_frame, 0);
}

fiber::async::DetachedTask run_partial_reset_client(fiber::event::EventLoop *loop, std::uint16_t port,
                                                    std::promise<fiber::cat::CatClientStats> *promise) {
    fiber::cat::CatClientConfigParams params{
            .app_key = "partial-reset",
            .hostname = "host",
            .ip = "127.0.0.1",
    };
    params.bootstrap_collectors.emplace_back(fiber::net::IpAddress::loopback_v4(), port);
    auto config = fiber::cat::CatClientConfig::create(std::move(params));
    if (!config) {
        promise->set_value({});
        co_return;
    }
    fiber::cat::CatClientOptions options;
    options.enable_heartbeat = false;
    options.max_queued_bytes = 32 * 1024 * 1024;
    options.max_batch_bytes = 32 * 1024 * 1024;
    options.max_send_bytes_per_pump = 32 * 1024 * 1024;
    options.max_send_calls_per_pump = 64;
    options.collector_connect_timeout = 1s;
    options.collector_write_timeout = 1s;
    options.shutdown_drain_timeout = 0ms;
    auto core = std::make_shared<fiber::cat::detail::CatClientCore>(*loop, std::move(*config), options, nullptr);
    if (!core->start()) {
        promise->set_value({});
        co_return;
    }
    (void) core->submit_encoded(make_frame(8 * 1024 * 1024, 4));
    const auto deadline = loop->now() + 3s;
    while (core->stats().dropped_partial_frame == 0 && loop->now() < deadline) {
        co_await fiber::async::sleep(1ms);
    }
    const auto stats = core->stats();
    co_await core->shutdown();
    promise->set_value(stats);
}

TEST(CatClientTest, ConnectionResetDropsOnlyThePartiallyWrittenFrame) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::promise<void> closed_promise;
    auto closed_future = closed_promise.get_future();
    fiber::async::spawn(group.at(0), [&] { return run_reset_collector(&group.at(0), &port_promise, &closed_promise); });
    if (port_future.wait_for(2s) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "reset collector did not publish its port";
    }
    const std::uint16_t port = port_future.get();
    if (port == 0) {
        group.stop();
        group.join();
        FAIL() << "reset collector failed to bind";
    }
    std::promise<fiber::cat::CatClientStats> stats_promise;
    auto stats_future = stats_promise.get_future();
    fiber::async::spawn(group.at(0), [&] { return run_partial_reset_client(&group.at(0), port, &stats_promise); });
    const bool stats_ready = stats_future.wait_for(5s) == std::future_status::ready;
    const bool closed_ready = closed_future.wait_for(5s) == std::future_status::ready;
    group.stop();
    group.join();
    ASSERT_TRUE(stats_ready);
    ASSERT_TRUE(closed_ready);
    const auto stats = stats_future.get();
    EXPECT_GT(stats.write_would_block, 0);
    EXPECT_GE(stats.write_failures, 1);
    EXPECT_EQ(stats.dropped_partial_frame, 1);
    EXPECT_EQ(stats.sent_messages, 0);
}

fiber::async::DetachedTask run_failover_client(fiber::event::EventLoop *loop, std::uint16_t port,
                                               std::promise<fiber::cat::CatClientStats> *promise) {
    fiber::cat::CatClientConfigParams params{
            .app_key = "failover",
            .hostname = "host",
            .ip = "127.0.0.1",
    };
    params.bootstrap_collectors.emplace_back(fiber::net::IpAddress::loopback_v4(), 1);
    params.bootstrap_collectors.emplace_back(fiber::net::IpAddress::loopback_v4(), port);
    auto config = fiber::cat::CatClientConfig::create(std::move(params));
    if (!config) {
        promise->set_value({});
        co_return;
    }
    fiber::cat::CatClientOptions options;
    options.enable_heartbeat = false;
    options.collector_connect_timeout = 100ms;
    options.reconnect_initial_delay = 1ms;
    options.reconnect_max_delay = 5ms;
    options.shutdown_drain_timeout = 100ms;
    auto core = std::make_shared<fiber::cat::detail::CatClientCore>(*loop, std::move(*config), options, nullptr);
    if (!core->start()) {
        promise->set_value({});
        co_return;
    }
    (void) core->submit_encoded(make_frame(8, 7));
    const auto deadline = loop->now() + 2s;
    while (core->stats().sent_messages == 0 && loop->now() < deadline) {
        co_await fiber::async::sleep(1ms);
    }
    const auto stats = core->stats();
    co_await core->shutdown();
    promise->set_value(stats);
}

TEST(CatClientTest, FailsOverFromFirstCollectorToSecondWithoutDroppingFrame) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::promise<fiber::common::IoResult<std::vector<std::uint8_t>>> bytes_promise;
    auto bytes_future = bytes_promise.get_future();
    fiber::async::spawn(group.at(0), [&] { return run_raw_collector(&group.at(0), 8, &port_promise, &bytes_promise); });
    if (port_future.wait_for(2s) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "failover collector did not publish its port";
    }
    const std::uint16_t port = port_future.get();
    if (port == 0) {
        group.stop();
        group.join();
        FAIL() << "failover collector failed to bind";
    }
    std::promise<fiber::cat::CatClientStats> stats_promise;
    auto stats_future = stats_promise.get_future();
    fiber::async::spawn(group.at(0), [&] { return run_failover_client(&group.at(0), port, &stats_promise); });
    const bool stats_ready = stats_future.wait_for(5s) == std::future_status::ready;
    const bool bytes_ready = bytes_future.wait_for(5s) == std::future_status::ready;
    group.stop();
    group.join();
    ASSERT_TRUE(stats_ready);
    ASSERT_TRUE(bytes_ready);
    const auto stats = stats_future.get();
    EXPECT_GE(stats.connect_failures, 1);
    EXPECT_EQ(stats.connect_successes, 1);
    EXPECT_EQ(stats.sent_messages, 1);
    EXPECT_EQ(stats.dropped_partial_frame, 0);
    auto bytes = bytes_future.get();
    ASSERT_TRUE(bytes);
    EXPECT_TRUE(std::all_of(bytes->begin(), bytes->end(), [](std::uint8_t value) { return value == 7; }));
}

fiber::async::DetachedTask run_budget_case(fiber::event::EventLoop *loop, std::size_t max_messages,
                                           std::size_t max_bytes, std::vector<std::size_t> sizes,
                                           std::promise<BudgetOutcome> *promise) {
    BudgetOutcome outcome;
    fiber::cat::CatClientConfigParams params{
            .app_key = "budget-test",
            .hostname = "host",
            .ip = "127.0.0.1",
    };
    params.bootstrap_collectors.emplace_back(fiber::net::IpAddress::loopback_v4(), 1);
    auto config = fiber::cat::CatClientConfig::create(std::move(params));
    if (!config) {
        promise->set_value(outcome);
        co_return;
    }

    fiber::cat::CatClientOptions options;
    options.max_queued_messages = max_messages;
    options.max_queued_bytes = max_bytes;
    options.collector_connect_timeout = 10ms;
    options.shutdown_drain_timeout = 0ms;
    auto core = std::make_shared<fiber::cat::detail::CatClientCore>(*loop, std::move(*config), options, nullptr);
    auto started = core->start();
    if (!started) {
        promise->set_value(outcome);
        co_return;
    }
    outcome.started = true;

    std::uint8_t value = 0;
    for (const std::size_t size: sizes) {
        core->submit_encoded(make_frame(size, value++));
    }
    outcome.before_shutdown = core->stats();
    co_await core->shutdown();
    outcome.after_shutdown = core->stats();
    outcome.stopped = core->state() == fiber::cat::CatClientState::Stopped;
    promise->set_value(outcome);
}

BudgetOutcome run_budget_case(std::size_t max_messages, std::size_t max_bytes, std::vector<std::size_t> sizes) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    std::promise<BudgetOutcome> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_budget_case(&group.at(0), max_messages, max_bytes, std::move(sizes), &promise);
    });
    const bool ready = future.wait_for(2s) == std::future_status::ready;
    group.stop();
    group.join();
    if (!ready) {
        return {};
    }
    return future.get();
}

TEST(CatClientTest, RejectsFramesAtOutstandingMessageLimitAndDrainsOnShutdown) {
    const BudgetOutcome outcome = run_budget_case(2, 1024, {8, 8, 8});
    ASSERT_TRUE(outcome.started);
    EXPECT_EQ(outcome.before_shutdown.queued_messages, 2);
    EXPECT_EQ(outcome.before_shutdown.queued_bytes, 16);
    EXPECT_EQ(outcome.before_shutdown.submitted_messages, 2);
    EXPECT_EQ(outcome.before_shutdown.dropped_queue_full, 1);
    EXPECT_TRUE(outcome.stopped);
    EXPECT_EQ(outcome.after_shutdown.queued_messages, 0);
    EXPECT_EQ(outcome.after_shutdown.queued_bytes, 0);
    EXPECT_EQ(outcome.after_shutdown.dropped_unavailable, 2);
}

TEST(CatClientTest, RejectsFramesAtOutstandingByteLimit) {
    const BudgetOutcome outcome = run_budget_case(8, 12, {8, 8});
    ASSERT_TRUE(outcome.started);
    EXPECT_EQ(outcome.before_shutdown.queued_messages, 1);
    EXPECT_EQ(outcome.before_shutdown.queued_bytes, 8);
    EXPECT_EQ(outcome.before_shutdown.submitted_messages, 1);
    EXPECT_EQ(outcome.before_shutdown.dropped_queue_full, 1);
    EXPECT_TRUE(outcome.stopped);
    EXPECT_EQ(outcome.after_shutdown.queued_messages, 0);
    EXPECT_EQ(outcome.after_shutdown.queued_bytes, 0);
}

fiber::async::DetachedTask run_system_budget_case(fiber::event::EventLoop *loop,
                                                  std::promise<fiber::cat::CatClientStats> *promise) {
    fiber::cat::CatClientConfigParams params{
            .app_key = "system-budget",
            .hostname = "host",
            .ip = "127.0.0.1",
    };
    params.bootstrap_collectors.emplace_back(fiber::net::IpAddress::loopback_v4(), 1);
    auto config = fiber::cat::CatClientConfig::create(std::move(params));
    if (!config) {
        promise->set_value({});
        loop->stop();
        co_return;
    }
    fiber::cat::CatClientOptions options;
    options.enable_heartbeat = false;
    options.max_system_queued_messages = 1;
    options.max_system_queued_bytes = 8;
    options.collector_connect_timeout = 10ms;
    options.shutdown_drain_timeout = 0ms;
    auto core = std::make_shared<fiber::cat::detail::CatClientCore>(*loop, std::move(*config), options, nullptr);
    if (!core->start()) {
        promise->set_value({});
        loop->stop();
        co_return;
    }
    EXPECT_EQ(core->submit_encoded(make_frame(8, 1), fiber::cat::detail::FramePriority::System),
              fiber::cat::detail::SubmitResult::Submitted);
    EXPECT_EQ(core->submit_encoded(make_frame(8, 2), fiber::cat::detail::FramePriority::System),
              fiber::cat::detail::SubmitResult::Full);
    EXPECT_EQ(core->submit_encoded(make_frame(8, 3)), fiber::cat::detail::SubmitResult::Submitted);
    const auto stats = core->stats();
    co_await core->shutdown();
    promise->set_value(stats);
    loop->stop();
}

TEST(CatClientTest, SystemFramesHaveIndependentBoundedAdmission) {
    fiber::event::EventLoop loop;
    std::promise<fiber::cat::CatClientStats> promise;
    auto future = promise.get_future();
    fiber::async::spawn(loop, [&] { return run_system_budget_case(&loop, &promise); });
    loop.run();
    const auto stats = future.get();
    EXPECT_EQ(stats.queued_messages, 2);
    EXPECT_EQ(stats.queued_bytes, 16);
    EXPECT_EQ(stats.system_queued_messages, 1);
    EXPECT_EQ(stats.system_queued_bytes, 8);
    EXPECT_EQ(stats.submitted_messages, 2);
    EXPECT_EQ(stats.dropped_queue_full, 1);
}

fiber::async::DetachedTask
start_internal_core(fiber::event::EventLoop *loop,
                    std::promise<std::shared_ptr<fiber::cat::detail::CatClientCore>> *promise) {
    fiber::cat::CatClientConfigParams params{
            .app_key = "shutdown-race",
            .hostname = "host",
            .ip = "127.0.0.1",
    };
    params.bootstrap_collectors.emplace_back(fiber::net::IpAddress::loopback_v4(), 1);
    auto config = fiber::cat::CatClientConfig::create(std::move(params));
    if (!config) {
        promise->set_value(nullptr);
        co_return;
    }
    fiber::cat::CatClientOptions options;
    options.max_queued_messages = 2048;
    options.max_queued_bytes = 1024 * 1024;
    options.collector_connect_timeout = 10ms;
    options.shutdown_drain_timeout = 0ms;
    auto core = std::make_shared<fiber::cat::detail::CatClientCore>(*loop, std::move(*config), options, nullptr);
    if (!core->start()) {
        promise->set_value(nullptr);
        co_return;
    }
    promise->set_value(std::move(core));
}

fiber::async::DetachedTask shutdown_internal_core(std::shared_ptr<fiber::cat::detail::CatClientCore> core,
                                                  std::promise<fiber::cat::CatClientStats> *promise) {
    co_await core->shutdown();
    promise->set_value(core->stats());
}

TEST(CatClientTest, ConcurrentSubmittersCannotOutliveImmediateShutdown) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<std::shared_ptr<fiber::cat::detail::CatClientCore>> core_promise;
    auto core_future = core_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return start_internal_core(&group.at(0), &core_promise); });
    if (core_future.wait_for(2s) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "internal CAT core did not start";
    }
    auto core = core_future.get();
    if (!core) {
        group.stop();
        group.join();
        FAIL() << "internal CAT core failed to start";
    }

    core->submit_encoded(make_frame(8, 0));
    std::atomic_bool start{false};
    std::array<std::thread, 4> producers;
    for (std::size_t index = 0; index < producers.size(); ++index) {
        producers[index] = std::thread([core, &start, index] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (std::size_t frame = 0; frame < 100; ++frame) {
                core->submit_encoded(make_frame(8, static_cast<std::uint8_t>(index)));
            }
        });
    }

    std::promise<fiber::cat::CatClientStats> stopped_promise;
    auto stopped_future = stopped_promise.get_future();
    start.store(true, std::memory_order_release);
    fiber::async::spawn(group.at(0), [&]() { return shutdown_internal_core(core, &stopped_promise); });
    for (auto &producer: producers) {
        producer.join();
    }

    const bool stopped = stopped_future.wait_for(2s) == std::future_status::ready;
    group.stop();
    group.join();
    ASSERT_TRUE(stopped);
    const auto stats = stopped_future.get();
    EXPECT_EQ(core->state(), fiber::cat::CatClientState::Stopped);
    EXPECT_EQ(stats.queued_messages, 0);
    EXPECT_EQ(stats.queued_bytes, 0);
    EXPECT_GE(stats.submitted_messages, 1);
}

} // namespace
