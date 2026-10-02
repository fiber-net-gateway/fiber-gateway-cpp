#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <sys/socket.h>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/Timeout.h>
#include <fiber/cat/CatClient.h>
#include <fiber/common/mem/IoBuf.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/net/TcpListener.h>
#include <fiber/net/TcpStream.h>

#include "CatClientCore.h"

namespace {

using namespace std::chrono_literals;
using fiber::cat::detail::CatClientCore;

enum class Scenario { Fin, Reset, HalfClose, BufferedHalfClose, Healthy, Flapping, PartialReset };

fiber::async::Task<bool> wait_connections(CatClientCore &core, std::uint64_t count) {
    const auto deadline = core.sender_loop().now() + 2s;
    while (core.stats().connect_successes < count && core.sender_loop().now() < deadline) {
        co_await fiber::async::sleep(1ms);
    }
    co_return core.stats().connect_successes == count;
}

fiber::async::Task<void> exercise_transport(CatClientCore &core, fiber::net::TcpListener &listener, Scenario scenario) {
    auto &loop = core.sender_loop();
    auto accepted = co_await fiber::async::timeout_for([&] { return listener.accept(); }, 2s);
    if (!accepted) {
        ADD_FAILURE() << "initial accept failed";
        co_return;
    }
    std::optional<fiber::net::TcpStream> peer(std::in_place, loop, accepted->release_fd(), accepted->take_peer());
    EXPECT_TRUE(co_await wait_connections(core, 1));
    EXPECT_EQ(core.stats().submitted_messages, 0);

    if (scenario == Scenario::Flapping) {
        auto delay = 20ms;
        for (std::uint64_t count = 2; count <= 5; ++count) {
            const auto closed_at = loop.now();
            peer->close();
            auto next = co_await fiber::async::timeout_for([&] { return listener.accept(); }, 2s);
            if (!next) {
                ADD_FAILURE() << "reconnect after immediate close failed";
                co_return;
            }
            EXPECT_GE(loop.now() - closed_at, delay);
            peer.emplace(loop, next->release_fd(), next->take_peer());
            EXPECT_TRUE(co_await wait_connections(core, count));
            EXPECT_EQ(core.stats().connection_failures, count - 1);
            delay = std::min(delay * 2, 80ms);
        }
        // A connection that survives the maximum backoff window resets the delay.
        co_await fiber::async::sleep(100ms);
        peer->close();
        auto next = co_await fiber::async::timeout_for([&] { return listener.accept(); }, 60ms);
        EXPECT_TRUE(next) << "stable connection did not reset reconnect backoff";
        if (next) {
            peer.emplace(loop, next->release_fd(), next->take_peer());
            EXPECT_TRUE(co_await wait_connections(core, 6));
        }
        co_await core.shutdown();
        EXPECT_EQ(core.stats().connection_failures, 5);
        co_return;
    }

    if (scenario == Scenario::BufferedHalfClose || scenario == Scenario::Healthy) {
        // Exceed the read pump budget and send a second burst after it becomes idle.
        std::array<char, 256 * 1024> bytes{};
        for (int burst = 0; burst < 2; ++burst) {
            std::size_t offset = 0;
            while (offset < bytes.size()) {
                auto written = co_await peer->write(bytes.data() + offset, bytes.size() - offset, 1s);
                if (!written || *written == 0) {
                    ADD_FAILURE() << "collector data write failed";
                    co_return;
                }
                offset += *written;
            }
            if (burst == 0) {
                co_await fiber::async::sleep(10ms);
            }
        }
        EXPECT_EQ(core.stats().connection_failures, 0);
    }
    if (scenario == Scenario::Healthy) {
        // Shutdown with live read/terminal subscriptions (and possibly a deferred read).
        co_await core.shutdown();
        EXPECT_EQ(core.stats().connection_failures, 0);
        EXPECT_EQ(core.stats().read_failures, 0);
        EXPECT_EQ(core.stats().peer_closes, 0);
        co_return;
    }

    if (scenario == Scenario::PartialReset) {
        auto large = fiber::mem::IoBuf::allocate(8 * 1024 * 1024);
        if (!large) {
            ADD_FAILURE() << "frame allocation failed";
            co_return;
        }
        std::fill_n(large.writable_data(), 8 * 1024 * 1024, 1);
        large.commit(8 * 1024 * 1024);
        EXPECT_EQ(core.submit_encoded(std::move(large)), fiber::cat::detail::SubmitResult::Submitted);
        auto complete = fiber::mem::IoBuf::allocate(8);
        if (!complete) {
            ADD_FAILURE() << "frame allocation failed";
            co_return;
        }
        std::fill_n(complete.writable_data(), 8, 7);
        complete.commit(8);
        EXPECT_EQ(core.submit_encoded(std::move(complete)), fiber::cat::detail::SubmitResult::Submitted);
        const auto deadline = loop.now() + 2s;
        while (core.stats().write_would_block == 0 && loop.now() < deadline) {
            co_await fiber::async::sleep(1ms);
        }
        EXPECT_GT(core.stats().write_would_block, 0);
    }
    if (scenario == Scenario::Reset || scenario == Scenario::PartialReset) {
        linger reset{.l_onoff = 1, .l_linger = 0};
        EXPECT_EQ(::setsockopt(peer->fd(), SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)), 0);
        peer->close();
    } else if (scenario == Scenario::HalfClose || scenario == Scenario::BufferedHalfClose) {
        EXPECT_EQ(::shutdown(peer->fd(), SHUT_WR), 0);
    } else {
        peer->close();
    }

    auto reconnected = co_await fiber::async::timeout_for([&] { return listener.accept(); }, 2s);
    if (!reconnected) {
        ADD_FAILURE() << "collector was not reconnected without another submission";
        co_return;
    }
    fiber::net::TcpStream replacement(loop, reconnected->release_fd(), reconnected->take_peer());
    EXPECT_TRUE(co_await wait_connections(core, 2));
    EXPECT_EQ(core.stats().connection_failures, 1);
    if (scenario != Scenario::PartialReset) {
        EXPECT_EQ(core.stats().submitted_messages, 0);
        EXPECT_EQ(core.stats().write_failures, 0);
    }
    if (scenario == Scenario::HalfClose || scenario == Scenario::BufferedHalfClose) {
        EXPECT_EQ(core.stats().peer_closes, 1);
    }

    if (scenario == Scenario::PartialReset) {
        std::array<std::uint8_t, 8> bytes{};
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            auto read = co_await replacement.read(bytes.data() + offset, bytes.size() - offset, 1s);
            if (!read || *read == 0) {
                ADD_FAILURE() << "complete frame was not preserved across reconnect";
                co_return;
            }
            offset += *read;
        }
        EXPECT_TRUE(std::all_of(bytes.begin(), bytes.end(), [](auto byte) { return byte == 7; }));
        // The old write timeout must not affect the replacement connection.
        co_await fiber::async::sleep(150ms);
        EXPECT_EQ(core.stats().connection_failures, 1);
        EXPECT_EQ(core.stats().dropped_partial_frame, 1);
        EXPECT_EQ(core.stats().sent_messages, 1);
        EXPECT_EQ(core.stats().queued_messages, 0);
    }
    co_await core.shutdown();
    EXPECT_EQ(core.stats().connection_failures, 1);
    EXPECT_EQ(core.state(), fiber::cat::CatClientState::Stopped);
}

fiber::async::DetachedTask run_transport(fiber::event::EventLoop &loop, Scenario scenario, std::promise<void> &done) {
    fiber::net::TcpListener listener(loop);
    if (!listener.bind({fiber::net::IpAddress::loopback_v4(), 0}, {})) {
        ADD_FAILURE() << "collector bind failed";
        done.set_value();
        co_return;
    }
    sockaddr_in bound{};
    socklen_t length = sizeof(bound);
    if (::getsockname(listener.fd(), reinterpret_cast<sockaddr *>(&bound), &length) != 0) {
        ADD_FAILURE() << "collector address unavailable";
        done.set_value();
        co_return;
    }
    fiber::cat::CatClientConfigParams params{.app_key = "transport", .hostname = "host", .ip = "127.0.0.1"};
    params.bootstrap_collectors.emplace_back(fiber::net::IpAddress::loopback_v4(), ntohs(bound.sin_port));
    auto config = fiber::cat::CatClientConfig::create(std::move(params));
    if (!config) {
        ADD_FAILURE() << "client configuration failed";
        done.set_value();
        co_return;
    }
    fiber::cat::CatClientOptions options;
    options.enable_heartbeat = false;
    options.reconnect_initial_delay = 20ms;
    options.reconnect_max_delay = 80ms;
    options.collector_write_timeout = 100ms;
    options.shutdown_drain_timeout = 0ms;
    options.max_queued_bytes = 16 * 1024 * 1024;
    options.max_send_bytes_per_pump = 16 * 1024 * 1024;
    auto core = std::make_shared<CatClientCore>(loop, std::move(*config), options, nullptr);
    if (core->start()) {
        co_await exercise_transport(*core, listener, scenario);
        co_await core->shutdown();
    } else {
        ADD_FAILURE() << "client start failed";
    }
    done.set_value();
}

void check_transport(Scenario scenario) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&] { return run_transport(group.at(0), scenario, done); });
    const bool completed = future.wait_for(10s) == std::future_status::ready;
    group.stop();
    group.join();
    EXPECT_TRUE(completed);
}

TEST(CatTransportTest, ReconnectsIdleFinWithoutNewMessages) { check_transport(Scenario::Fin); }
TEST(CatTransportTest, ReconnectsIdleResetWithoutNewMessages) { check_transport(Scenario::Reset); }
TEST(CatTransportTest, ReconnectsIdleHalfCloseWithoutNewMessages) { check_transport(Scenario::HalfClose); }
TEST(CatTransportTest, DrainsBufferedDataBeforeHalfClose) { check_transport(Scenario::BufferedHalfClose); }
TEST(CatTransportTest, InboundDataAndLocalShutdownDoNotCountAsFailure) { check_transport(Scenario::Healthy); }
TEST(CatTransportTest, ImmediatePeerClosesBackOffUntilConnectionIsStable) { check_transport(Scenario::Flapping); }
TEST(CatTransportTest, ResetDropsPartialFrameAndPreservesFollowingFrame) { check_transport(Scenario::PartialReset); }

} // namespace
