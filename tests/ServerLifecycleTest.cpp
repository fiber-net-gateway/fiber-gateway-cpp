#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/async/WaitGroup.h>
#include <fiber/common/IoError.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/Server.h>
#include <fiber/http/endpoint/Http1Endpoint.h>
#include <fiber/http/endpoint/Http3Endpoint.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/net/TlsCredential.h>
#include <fiber/net/TlsServerHandshakeConfig.h>
#include <fiber/quic/QuicUdpEndpoint.h>
#include "QuicTestTlsCertificate.h"

namespace {

using fiber::async::DetachedTask;
using fiber::common::IoErr;
using fiber::http::Endpoint;
using fiber::http::EndpointWorker;
using fiber::http::Server;
using namespace std::chrono_literals;

// Shared observation record for one TestEndpoint. Counters are atomics because
// the worker callbacks run on the worker loops while assertions read them from
// the test thread after a join.
struct EndpointTrace {
    std::atomic<int> start_calls{0};
    std::atomic<int> stop_calls{0};
    std::atomic<int> serve_started{0};
    std::atomic<int> serve_returned{0};
    std::atomic<int> workers_created{0};
    std::atomic<int> workers_destroyed{0};
    std::atomic<int> drain_calls{0};
    std::atomic<int> destroyed_off_own_loop{0};

    std::mutex worker_loops_mu{};
    std::vector<fiber::event::EventLoop *> worker_loops{};
};

// Fake endpoint standing in for the protocol endpoints that arrive in P2-P4.
//
// - on_serve() blocks on a gate that on_stop() opens, imitating an accept loop
//   that only returns once its listener is closed.
// - each worker holds `live_per_worker` fake connections, released `release_delay`
//   after drain(). A non-zero delay stands in for a protocol that needs time to
//   wind down: the server must wait it out rather than cut it short.
class TestEndpoint final : public Endpoint {
public:
    struct Config {
        EndpointTrace *trace = nullptr;
        IoErr start_error = IoErr::None;
        bool fail_worker = false;
        std::size_t live_per_worker = 0;
        std::chrono::milliseconds release_delay{0};
    };

    explicit TestEndpoint(Config config) noexcept : config_(config) { serve_gate_.add(); }

    ~TestEndpoint() override {
        if (!gate_opened_) {
            serve_gate_.done();
        }
    }

    fiber::common::IoResult<void> on_start(Server &) noexcept override {
        config_.trace->start_calls.fetch_add(1, std::memory_order_relaxed);
        if (config_.start_error != IoErr::None) {
            return std::unexpected(config_.start_error);
        }
        return {};
    }

    fiber::async::Task<void> on_serve() noexcept override {
        config_.trace->serve_started.fetch_add(1, std::memory_order_relaxed);
        co_await serve_gate_.join();
        config_.trace->serve_returned.fetch_add(1, std::memory_order_relaxed);
        co_return;
    }

    void on_stop() noexcept override {
        config_.trace->stop_calls.fetch_add(1, std::memory_order_relaxed);
        if (!gate_opened_) {
            gate_opened_ = true;
            serve_gate_.done();
        }
    }

    EndpointWorker *create_worker(fiber::event::EventLoop &loop, std::size_t index) noexcept override {
        if (config_.fail_worker) {
            return nullptr;
        }
        auto *worker = new (std::nothrow) TestWorker(config_, loop, index);
        if (worker == nullptr) {
            return nullptr;
        }
        config_.trace->workers_created.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard guard(config_.trace->worker_loops_mu);
            config_.trace->worker_loops.push_back(&loop);
        }
        return worker;
    }

    void allow_workers() noexcept { config_.fail_worker = false; }

    const fiber::net::SocketAddress &local_addr() const noexcept override { return local_addr_; }

private:
    class TestWorker final : public EndpointWorker {
    public:
        TestWorker(const Config &config, fiber::event::EventLoop &loop, std::size_t index) noexcept :
            config_(config), loop_(&loop), index_(index) {
            if (config_.live_per_worker != 0) {
                live_.add(config_.live_per_worker);
            }
        }

        ~TestWorker() override {
            if (fiber::event::EventLoop::current_or_null() != loop_) {
                config_.trace->destroyed_off_own_loop.fetch_add(1, std::memory_order_relaxed);
            }
            if (outstanding_ != 0) {
                live_.done(); // keeps the WaitGroup invariant when a test drops us early
                outstanding_ = 0;
            }
            config_.trace->workers_destroyed.fetch_add(1, std::memory_order_relaxed);
        }

        void drain() noexcept override {
            config_.trace->drain_calls.fetch_add(1, std::memory_order_relaxed);
            if (config_.release_delay <= std::chrono::milliseconds::zero()) {
                release_all();
                return;
            }
            // Stand-in for a protocol that takes a while to finish: the server
            // has no deadline and must wait for it.
            fiber::async::spawn(*loop_, [this]() -> DetachedTask {
                co_await fiber::async::sleep(config_.release_delay);
                release_all();
                co_return;
            });
        }

        fiber::async::Task<void> wait_stopped() noexcept override {
            co_await live_.join();
            co_return;
        }

        [[nodiscard]] std::size_t index() const noexcept { return index_; }

    private:
        void release_all() noexcept {
            while (outstanding_ != 0) {
                --outstanding_;
                live_.done();
            }
        }

        Config config_;
        fiber::event::EventLoop *loop_;
        std::size_t index_;
        fiber::async::WaitGroup live_{};
        std::size_t outstanding_ = config_.live_per_worker;
    };

    Config config_;
    fiber::net::SocketAddress local_addr_{};
    fiber::async::WaitGroup serve_gate_{};
    bool gate_opened_ = false;
};

// Runs `body` on loop 0 of a freshly started group and blocks until it returns.
template<typename Body>
void run_on_loop(fiber::event::EventLoopGroup &group, Body body) {
    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        co_await body();
        done.set_value();
        co_return;
    });
    future.get();
}

TEST(ServerLifecycleTest, StartWithoutEndpointsFails) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    Server server(group.at(0), {});
    auto result = server.start();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), IoErr::Invalid);
    EXPECT_EQ(server.state(), Server::State::Created);

    group.stop();
    group.join();
}

TEST(ServerLifecycleTest, StartTwiceReportsAlready) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    EndpointTrace trace;
    Server server(group.at(0), {});
    ASSERT_NE(server.add_endpoint<TestEndpoint>(TestEndpoint::Config{.trace = &trace}), nullptr);
    ASSERT_TRUE(server.start().has_value());

    auto again = server.start();
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error(), IoErr::Already);
    EXPECT_EQ(server.add_endpoint<TestEndpoint>(TestEndpoint::Config{.trace = &trace}), nullptr);

    run_on_loop(group, [&] { return server.stop_and_wait(); });
    EXPECT_EQ(server.state(), Server::State::Stopped);

    group.stop();
    group.join();
}

TEST(ServerLifecycleTest, EndpointStartFailureRollsBackEarlierEndpoints) {
    fiber::event::EventLoopGroup group(2);
    group.start();

    EndpointTrace first_trace;
    EndpointTrace failing_trace;
    EndpointTrace never_trace;
    Server server(group.at(0), {}, &group);
    ASSERT_NE(server.add_endpoint<TestEndpoint>(TestEndpoint::Config{.trace = &first_trace}), nullptr);
    ASSERT_NE(server.add_endpoint<TestEndpoint>(
                      TestEndpoint::Config{.trace = &failing_trace, .start_error = IoErr::AddrInUse}),
              nullptr);
    ASSERT_NE(server.add_endpoint<TestEndpoint>(TestEndpoint::Config{.trace = &never_trace}), nullptr);

    auto result = server.start();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), IoErr::AddrInUse);

    EXPECT_EQ(first_trace.start_calls.load(), 1);
    EXPECT_EQ(first_trace.stop_calls.load(), 1); // rolled back
    EXPECT_EQ(failing_trace.start_calls.load(), 1);
    EXPECT_EQ(failing_trace.stop_calls.load(), 0); // never bound, never stopped
    EXPECT_EQ(never_trace.start_calls.load(), 0);
    EXPECT_EQ(first_trace.workers_created.load(), 0);
    // Back to Created: the server can be started again once the conflict is gone.
    EXPECT_EQ(server.state(), Server::State::Created);

    group.stop();
    group.join();
}

TEST(ServerLifecycleTest, ServeReturnsAfterStopFromAnotherThread) {
    fiber::event::EventLoopGroup group(3);
    group.start();

    EndpointTrace trace;
    Server server(group.at(0), {}, &group);
    ASSERT_NE(server.add_endpoint<TestEndpoint>(TestEndpoint::Config{.trace = &trace}), nullptr);
    ASSERT_TRUE(server.start().has_value());
    EXPECT_EQ(server.state(), Server::State::Started);
    EXPECT_EQ(trace.workers_created.load(), 3);

    std::promise<void> serve_done;
    auto serve_future = serve_done.get_future();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        co_await server.serve();
        serve_done.set_value();
        co_return;
    });

    // Let the accept loop reach its gate before stopping from a foreign thread.
    while (trace.serve_started.load() == 0) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(server.state(), Server::State::Serving);

    std::thread stopper([&] { server.stop(); });
    stopper.join();
    serve_future.get();

    EXPECT_EQ(server.state(), Server::State::Stopped);
    EXPECT_TRUE(server.draining());
    EXPECT_EQ(trace.stop_calls.load(), 1);
    EXPECT_EQ(trace.serve_returned.load(), 1);
    EXPECT_EQ(trace.drain_calls.load(), 3);
    EXPECT_EQ(trace.workers_destroyed.load(), 3);
    EXPECT_EQ(trace.destroyed_off_own_loop.load(), 0);

    group.stop();
    group.join();
}

TEST(ServerLifecycleTest, WorkerSlotsFollowGroupLoops) {
    fiber::event::EventLoopGroup group(3);
    group.start();

    EndpointTrace first;
    EndpointTrace second;
    Server server(group.at(0), {}, &group);
    ASSERT_NE(server.add_endpoint<TestEndpoint>(TestEndpoint::Config{.trace = &first}), nullptr);
    ASSERT_NE(server.add_endpoint<TestEndpoint>(TestEndpoint::Config{.trace = &second}), nullptr);
    ASSERT_TRUE(server.start().has_value());

    EXPECT_EQ(server.worker_count(), 3u);
    EXPECT_EQ(server.endpoint_count(), 2u);
    EXPECT_EQ(first.workers_created.load(), 3);
    EXPECT_EQ(second.workers_created.load(), 3);
    {
        std::lock_guard guard(first.worker_loops_mu);
        ASSERT_EQ(first.worker_loops.size(), 3u);
        for (std::size_t i = 0; i < 3; ++i) {
            EXPECT_EQ(first.worker_loops[i], &group.at(i));
        }
    }

    run_on_loop(group, [&] { return server.stop_and_wait(); });
    EXPECT_EQ(first.workers_destroyed.load(), 3);
    EXPECT_EQ(second.workers_destroyed.load(), 3);
    EXPECT_EQ(first.destroyed_off_own_loop.load(), 0);
    EXPECT_EQ(second.destroyed_off_own_loop.load(), 0);

    group.stop();
    group.join();
}

TEST(ServerLifecycleTest, WithoutWorkerGroupTheOwnerLoopIsTheOnlyWorker) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    EndpointTrace trace;
    Server server(group.at(0), {});
    ASSERT_NE(server.add_endpoint<TestEndpoint>(TestEndpoint::Config{.trace = &trace}), nullptr);
    ASSERT_TRUE(server.start().has_value());

    EXPECT_EQ(server.worker_count(), 1u);
    {
        std::lock_guard guard(trace.worker_loops_mu);
        ASSERT_EQ(trace.worker_loops.size(), 1u);
        EXPECT_EQ(trace.worker_loops[0], &group.at(0));
    }

    run_on_loop(group, [&] { return server.stop_and_wait(); });
    EXPECT_EQ(trace.workers_destroyed.load(), 1);

    group.stop();
    group.join();
}

TEST(ServerLifecycleTest, StopIsIdempotentAcrossThreads) {
    fiber::event::EventLoopGroup group(2);
    group.start();

    EndpointTrace trace;
    Server server(group.at(0), {}, &group);
    ASSERT_NE(server.add_endpoint<TestEndpoint>(TestEndpoint::Config{.trace = &trace}), nullptr);
    ASSERT_TRUE(server.start().has_value());

    std::promise<void> serve_done;
    auto serve_future = serve_done.get_future();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        co_await server.serve();
        serve_done.set_value();
        co_return;
    });
    while (trace.serve_started.load() == 0) {
        std::this_thread::sleep_for(1ms);
    }

    std::vector<std::thread> stoppers;
    stoppers.reserve(4);
    for (int i = 0; i < 4; ++i) {
        stoppers.emplace_back([&] {
            server.stop();
            server.stop();
        });
    }
    for (auto &thread: stoppers) {
        thread.join();
    }
    serve_future.get();

    // The listener is closed and every worker is drained exactly once.
    EXPECT_EQ(trace.stop_calls.load(), 1);
    EXPECT_EQ(trace.drain_calls.load(), 2);
    EXPECT_EQ(server.state(), Server::State::Stopped);

    server.stop(); // still a no-op after the terminal state
    EXPECT_EQ(server.state(), Server::State::Stopped);

    group.stop();
    group.join();
}

TEST(ServerLifecycleTest, StopBeforeServeMakesServeReturnImmediately) {
    fiber::event::EventLoopGroup group(2);
    group.start();

    EndpointTrace trace;
    Server server(group.at(0), {}, &group);
    ASSERT_NE(server.add_endpoint<TestEndpoint>(TestEndpoint::Config{.trace = &trace}), nullptr);
    ASSERT_TRUE(server.start().has_value());

    server.stop();
    run_on_loop(group, [&] { return server.serve(); });

    EXPECT_EQ(server.state(), Server::State::Stopped);
    EXPECT_EQ(trace.serve_started.load(), 0); // accept loop never ran
    EXPECT_EQ(trace.stop_calls.load(), 1);
    EXPECT_EQ(trace.workers_destroyed.load(), 2);

    group.stop();
    group.join();
}

TEST(ServerLifecycleTest, StopOnNeverStartedServerReachesTerminalState) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    EndpointTrace trace;
    {
        Server server(group.at(0), {});
        ASSERT_NE(server.add_endpoint<TestEndpoint>(TestEndpoint::Config{.trace = &trace}), nullptr);
        server.stop();
        EXPECT_EQ(server.state(), Server::State::Stopped);

        run_on_loop(group, [&] { return server.stop_and_wait(); });
        EXPECT_EQ(trace.start_calls.load(), 0);
        EXPECT_EQ(trace.workers_created.load(), 0);
    }

    group.stop();
    group.join();
}

TEST(ServerLifecycleTest, DestroyingAFreshServerIsSafe) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    EndpointTrace trace;
    {
        Server server(group.at(0), {});
        ASSERT_NE(server.add_endpoint<TestEndpoint>(TestEndpoint::Config{.trace = &trace}), nullptr);
    }
    EXPECT_EQ(trace.start_calls.load(), 0);

    group.stop();
    group.join();
}

TEST(ServerLifecycleTest, DrainReleasesLiveConnections) {
    fiber::event::EventLoopGroup group(2);
    group.start();

    EndpointTrace trace;
    Server server(group.at(0), {}, &group);
    ASSERT_NE(server.add_endpoint<TestEndpoint>(TestEndpoint::Config{
                      .trace = &trace,
                      .live_per_worker = 4,
              }),
              nullptr);
    ASSERT_TRUE(server.start().has_value());

    run_on_loop(group, [&] { return server.stop_and_wait(); });

    EXPECT_EQ(trace.drain_calls.load(), 2);
    EXPECT_EQ(trace.workers_destroyed.load(), 2);
    EXPECT_EQ(server.state(), Server::State::Stopped);

    group.stop();
    group.join();
}

// Shutdown has no deadline: an endpoint that needs time to wind down gets it.
TEST(ServerLifecycleTest, ShutdownWaitsOutASlowDrain) {
    fiber::event::EventLoopGroup group(2);
    group.start();

    EndpointTrace trace;
    Server server(group.at(0), {}, &group);
    ASSERT_NE(server.add_endpoint<TestEndpoint>(TestEndpoint::Config{
                      .trace = &trace,
                      .live_per_worker = 2,
                      .release_delay = 150ms,
              }),
              nullptr);
    ASSERT_TRUE(server.start().has_value());

    const auto began = std::chrono::steady_clock::now();
    run_on_loop(group, [&] { return server.stop_and_wait(); });
    const auto elapsed = std::chrono::steady_clock::now() - began;

    // Nothing cut the drain short, and nothing gave up on it either.
    EXPECT_GE(elapsed, 140ms);
    EXPECT_LT(elapsed, 10s);
    EXPECT_EQ(trace.drain_calls.load(), 2);
    EXPECT_EQ(trace.workers_destroyed.load(), 2);
    EXPECT_EQ(server.state(), Server::State::Stopped);

    group.stop();
    group.join();
}

// Two endpoints sharing one worker loop wind down at their own pace; the
// worker is done only when both are.
TEST(ServerLifecycleTest, WaitsForEveryEndpointOnAWorker) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    EndpointTrace quick;
    EndpointTrace slow;
    Server server(group.at(0), {}, &group);
    ASSERT_NE(server.add_endpoint<TestEndpoint>(TestEndpoint::Config{
                      .trace = &quick,
                      .live_per_worker = 1,
              }),
              nullptr);
    ASSERT_NE(server.add_endpoint<TestEndpoint>(TestEndpoint::Config{
                      .trace = &slow,
                      .live_per_worker = 1,
                      .release_delay = 120ms,
              }),
              nullptr);
    ASSERT_TRUE(server.start().has_value());

    const auto began = std::chrono::steady_clock::now();
    run_on_loop(group, [&] { return server.stop_and_wait(); });
    const auto elapsed = std::chrono::steady_clock::now() - began;

    EXPECT_GE(elapsed, 110ms);
    EXPECT_EQ(quick.drain_calls.load(), 1);
    EXPECT_EQ(slow.drain_calls.load(), 1);
    EXPECT_EQ(quick.workers_destroyed.load(), 1);
    EXPECT_EQ(slow.workers_destroyed.load(), 1);
    EXPECT_EQ(server.state(), Server::State::Stopped);

    group.stop();
    group.join();
}

TEST(ServerLifecycleTest, ServeAndStopAndWaitAllComplete) {
    fiber::event::EventLoopGroup group(2);
    group.start();

    EndpointTrace trace;
    Server server(group.at(0), {}, &group);
    ASSERT_NE(server.add_endpoint<TestEndpoint>(TestEndpoint::Config{.trace = &trace}), nullptr);
    ASSERT_TRUE(server.start().has_value());

    std::promise<void> serve_done;
    std::promise<void> joiner_a_done;
    std::promise<void> joiner_b_done;
    auto serve_future = serve_done.get_future();
    auto joiner_a_future = joiner_a_done.get_future();
    auto joiner_b_future = joiner_b_done.get_future();

    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        co_await server.serve();
        serve_done.set_value();
        co_return;
    });
    // One joiner on the owner loop, one on a worker loop: WaitGroup resumes
    // each waiter on its own loop.
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        co_await server.stop_and_wait();
        joiner_a_done.set_value();
        co_return;
    });
    fiber::async::spawn(group.at(1), [&]() -> DetachedTask {
        co_await server.stop_and_wait();
        joiner_b_done.set_value();
        co_return;
    });

    serve_future.get();
    joiner_a_future.get();
    joiner_b_future.get();

    EXPECT_EQ(server.state(), Server::State::Stopped);
    EXPECT_EQ(trace.stop_calls.load(), 1);

    group.stop();
    group.join();
}


TEST(ServerLifecycleTest, WorkerAllocationFailureRollsBackTcpAndUdpAndAllowsRetry) {
    fiber::event::EventLoopGroup group(2);
    group.start();
    EndpointTrace trace;
    run_on_loop(group, [&]() -> fiber::async::Task<void> {
        fiber::net::TlsCredentialOptions credentials{};
        credentials.certificate_chain = fiber::net::TlsPemSource::from_content(fiber::test::kQuicTestCertificatePem);
        credentials.private_key = fiber::net::TlsPemSource::from_content(fiber::test::kQuicTestPrivateKeyPem);
        auto credential = fiber::net::TlsCredential::create(credentials);
        EXPECT_TRUE(credential);
        if (!credential) {
            co_return;
        }
        Server server(group.at(0), {}, &group);
        fiber::http::Http1Endpoint::Options tcp{};
        tcp.address = {fiber::net::IpAddress::loopback_v4(), 0};
        auto *endpoint = server.add_endpoint<fiber::http::Http1Endpoint>(tcp);
        fiber::http::Http3Endpoint::Options h3{};
        h3.address = tcp.address;
        h3.inherit_port_from = endpoint;
        h3.tls.configure_callback = &fiber::net::configure_tls_with_credential;
        h3.tls.configure_ctx = credential->get();
        EXPECT_NE(server.add_endpoint<fiber::http::Http3Endpoint>(h3), nullptr);
        auto *failing = server.add_endpoint<TestEndpoint>(TestEndpoint::Config{.trace = &trace, .fail_worker = true});
        EXPECT_NE(failing, nullptr);
        for (int attempt = 0; attempt < 2; ++attempt) {
            auto result = server.start();
            EXPECT_FALSE(result);
            EXPECT_EQ(result.error(), IoErr::NoMem);
            EXPECT_EQ(server.state(), Server::State::Created);
            EXPECT_EQ(endpoint->listener_fd(), -1);
            // Every UDP shard must have released the inherited port too.
            fiber::quic::QuicUdpEndpoint probe(group.at(0));
            fiber::quic::QuicUdpEndpoint::EndpointOptions opts{};
            opts.bind_addr = endpoint->local_addr();
            EXPECT_TRUE(probe.init(opts));
            probe.close();
        }
        failing->allow_workers();
        EXPECT_TRUE(server.start());
        co_await server.stop_and_wait();
    });
    group.stop();
    group.join();
}

} // namespace
