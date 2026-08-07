#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <expected>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/async/Yield.h>
#include <fiber/event/EventLoop.h>
#include <fiber/nacos/NamingService.h>
#include <fiber/nacos/Subscription.h>
#include <fiber/nacos/discovery/ServiceDiscovery.h>

#include "../../../tests/NacosSnapshotTestBuilder.h"

namespace {

class FakeNamingService final : public fiber::nacos::NamingService {
public:
    using Result = fiber::nacos::SubscriptionResult<fiber::nacos::ServiceInfo>;
    using Subscription = fiber::nacos::Subscription<fiber::nacos::ServiceInfo>;

    fiber::common::IoResult<void> start() noexcept override { return {}; }
    fiber::async::Task<void> shutdown() noexcept override { co_return; }

    fiber::async::Task<
            std::expected<std::shared_ptr<const fiber::nacos::ServiceInfo>, fiber::nacos::NamingServiceError>>
    get(std::string, std::string) noexcept override {
        co_return std::unexpected(fiber::nacos::NamingServiceError{
                .code = fiber::nacos::NamingServiceErrorCode::Server,
                .message = "not implemented by fake naming service",
        });
    }

    std::expected<Subscription, fiber::nacos::NamingServiceError> subscribe(std::string_view service_name,
                                                                            std::string_view group,
                                                                            Subscription::NotifyCallback on_notify,
                                                                            void *ctx) override {
        const std::string key = make_key(service_name, group);
        auto [iterator, inserted] = entries_.try_emplace(key, std::make_unique<Entry>());
        (void) inserted;
        ++iterator->second->subscriptions;
        auto *node = new Node{.entry = iterator->second.get(), .callback = on_notify, .ctx = ctx};
        iterator->second->nodes.push_back(node);
        if (iterator->second->cached != nullptr) {
            const Result result{.kind = fiber::nacos::ResultKind::Success, .data = iterator->second->cached};
            on_notify(ctx, result);
        }
        return Subscription(node, &close_node, &node_closed);
    }

    std::expected<fiber::nacos::InstanceRegistration, fiber::nacos::NamingServiceError>
    registry(std::string_view, std::string_view, fiber::nacos::Instance) override {
        return std::unexpected(fiber::nacos::NamingServiceError{
                .code = fiber::nacos::NamingServiceErrorCode::Server,
                .message = "not implemented by fake naming service",
        });
    }

    void push(std::string_view service_name, std::string_view group, fiber::tests::ServiceInfoTestData info) {
        const auto iterator = entries_.find(make_key(service_name, group));
        EXPECT_NE(iterator, entries_.end());
        if (iterator == entries_.end()) {
            return;
        }
        Result result{
                .kind = fiber::nacos::ResultKind::Success,
                .data = fiber::tests::make_service_info(std::move(info)),
        };
        const auto nodes = iterator->second->nodes;
        for (Node *node: nodes) {
            if (!node->closed) {
                node->callback(node->ctx, result);
            }
        }
    }

    void cache(std::string_view service_name, std::string_view group, fiber::tests::ServiceInfoTestData info) {
        const std::string key = make_key(service_name, group);
        auto [iterator, inserted] = entries_.try_emplace(key, std::make_unique<Entry>());
        (void) inserted;
        iterator->second->cached = fiber::tests::make_service_info(std::move(info));
    }

    void close(std::string_view service_name, std::string_view group) {
        const auto iterator = entries_.find(make_key(service_name, group));
        EXPECT_NE(iterator, entries_.end());
        if (iterator == entries_.end()) {
            return;
        }
        const Result result{.kind = fiber::nacos::ResultKind::Closed, .data = nullptr};
        const auto nodes = iterator->second->nodes;
        for (Node *node: nodes) {
            if (!node->closed) {
                node->callback(node->ctx, result);
            }
        }
    }

    [[nodiscard]] std::size_t subscriptions(std::string_view service_name, std::string_view group) const {
        const auto iterator = entries_.find(make_key(service_name, group));
        return iterator == entries_.end() ? 0 : iterator->second->subscriptions;
    }

private:
    struct Entry;

    struct Node {
        Entry *entry = nullptr;
        Subscription::NotifyCallback callback = nullptr;
        void *ctx = nullptr;
        bool closed = false;
    };

    struct Entry {
        std::vector<Node *> nodes;
        std::shared_ptr<const fiber::nacos::ServiceInfo> cached;
        std::size_t subscriptions = 0;
    };

    static void close_node(void *context) noexcept {
        auto *node = static_cast<Node *>(context);
        if (node->entry != nullptr) {
            auto &nodes = node->entry->nodes;
            const auto found = std::find(nodes.begin(), nodes.end(), node);
            if (found != nodes.end()) {
                nodes.erase(found);
            }
        }
        node->closed = true;
        delete node;
    }

    [[nodiscard]] static bool node_closed(const void *context) noexcept {
        return static_cast<const Node *>(context)->closed;
    }

    static std::string make_key(std::string_view service_name, std::string_view group) {
        std::string key(service_name);
        key.push_back('\n');
        key.append(group);
        return key;
    }

    std::map<std::string, std::unique_ptr<Entry>, std::less<>> entries_;
};

struct FakeOpsCounters {
    std::size_t creates = 0;
    std::size_t updates = 0;
    std::size_t retires = 0;
    std::size_t destroys = 0;
    fiber::nacos::ServiceRetireReason last_retire_reason = fiber::nacos::ServiceRetireReason::Released;
};

struct FakeState {
    FakeState() noexcept = default;
    FakeState(const FakeState &) = delete;
    FakeState &operator=(const FakeState &) = delete;
    FakeState(FakeState &&) = delete;
    FakeState &operator=(FakeState &&) = delete;

    ~FakeState() noexcept {
        if (counters != nullptr) {
            ++counters->destroys;
        }
    }

    FakeOpsCounters *counters = nullptr;
    std::int64_t last_ref_time = 0;
};

struct FakeStateOps {
    using State = FakeState;

    void on_init(const fiber::nacos::ServiceKeyView &, State &state) noexcept {
        ++counters->creates;
        state.counters = counters;
    }

    void on_update(const fiber::nacos::ServiceKeyView &, State &state,
                   const std::shared_ptr<const fiber::nacos::ServiceInfo> &snapshot) noexcept {
        ++counters->updates;
        state.last_ref_time = snapshot->last_ref_time;
    }

    void on_retire(const fiber::nacos::ServiceKeyView &, State &, fiber::nacos::ServiceRetireReason reason) noexcept {
        ++counters->retires;
        counters->last_retire_reason = reason;
    }

    FakeOpsCounters *counters = nullptr;
};

using FakeServiceDiscovery = fiber::nacos::ServiceDiscovery<FakeStateOps>;

TEST(ServiceDiscoveryTest, WaitReadyCreatesStateOnceAndSeparatesRetireFromDestroy) {
    fiber::event::EventLoop loop;
    FakeNamingService naming;
    FakeOpsCounters counters;
    FakeServiceDiscovery discovery(loop, naming, FakeStateOps{.counters = &counters});
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto first = discovery.acquire("orders", "DEFAULT_GROUP");
        auto second = discovery.acquire("orders", "DEFAULT_GROUP");
        EXPECT_TRUE(first);
        EXPECT_TRUE(second);
        if (!first || !second) {
            if (first) {
                first->reset();
            }
            if (second) {
                second->reset();
            }
            co_await discovery.shutdown();
            completed = true;
            loop.stop();
            co_return;
        }
        EXPECT_EQ(counters.creates, 0U);

        fiber::async::spawn([&]() -> fiber::async::DetachedTask {
            co_await fiber::async::yield();
            fiber::tests::ServiceInfoTestData info;
            info.name = "orders";
            info.group_name = "DEFAULT_GROUP";
            info.last_ref_time = 7;
            naming.push("orders", "DEFAULT_GROUP", std::move(info));
        });

        auto ready = co_await first->wait_ready();
        EXPECT_TRUE(ready);
        if (!ready) {
            first->reset();
            second->reset();
            co_await discovery.shutdown();
            completed = true;
            loop.stop();
            co_return;
        }
        EXPECT_EQ(first->state().last_ref_time, 7);
        EXPECT_EQ(counters.creates, 1U);
        EXPECT_EQ(counters.updates, 1U);
        EXPECT_EQ(&first->state(), &second->state());

        fiber::tests::ServiceInfoTestData update;
        update.name = "orders";
        update.group_name = "DEFAULT_GROUP";
        update.last_ref_time = 9;
        naming.push("orders", "DEFAULT_GROUP", std::move(update));
        EXPECT_EQ(first->state().last_ref_time, 9);
        EXPECT_EQ(counters.updates, 2U);

        first->reset();
        EXPECT_EQ(counters.retires, 0U);
        second->reset();
        EXPECT_EQ(counters.retires, 1U);
        EXPECT_EQ(counters.last_retire_reason, fiber::nacos::ServiceRetireReason::Released);
        EXPECT_TRUE(discovery.empty());

        // The notification callback still pins Entry until the waiter yields
        // back to ServiceDiscovery::apply().
        co_await fiber::async::yield();
        EXPECT_EQ(counters.destroys, 1U);
        co_await discovery.shutdown();
        completed = true;
        loop.stop();
    });

    loop.run();
    EXPECT_TRUE(completed);
}

TEST(ServiceDiscoveryTest, CachedReplayInitializesBeforeAcquireReturns) {
    fiber::event::EventLoop loop;
    FakeNamingService naming;
    FakeOpsCounters counters;
    fiber::tests::ServiceInfoTestData cached;
    cached.name = "orders";
    cached.group_name = "DEFAULT_GROUP";
    cached.last_ref_time = 11;
    naming.cache("orders", "DEFAULT_GROUP", std::move(cached));
    FakeServiceDiscovery discovery(loop, naming, FakeStateOps{.counters = &counters});
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto lease = discovery.acquire("orders", "DEFAULT_GROUP");
        EXPECT_TRUE(lease);
        if (!lease) {
            co_await discovery.shutdown();
            completed = true;
            loop.stop();
            co_return;
        }
        EXPECT_EQ(lease->state().last_ref_time, 11);
        EXPECT_EQ(counters.creates, 1U);
        EXPECT_EQ(counters.updates, 1U);
        auto ready = co_await lease->wait_ready();
        EXPECT_TRUE(ready);
        lease->reset();
        co_await discovery.shutdown();
        completed = true;
        loop.stop();
    });

    loop.run();
    EXPECT_TRUE(completed);
}

TEST(ServiceDiscoveryTest, ClosedBeforeFirstNotifyWakesWaiterAndAllowsReacquire) {
    fiber::event::EventLoop loop;
    FakeNamingService naming;
    FakeOpsCounters counters;
    FakeServiceDiscovery discovery(loop, naming, FakeStateOps{.counters = &counters});
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto lease = discovery.acquire("orders", "DEFAULT_GROUP");
        EXPECT_TRUE(lease);
        if (!lease) {
            co_await discovery.shutdown();
            completed = true;
            loop.stop();
            co_return;
        }

        fiber::async::spawn([&]() -> fiber::async::DetachedTask {
            co_await fiber::async::yield();
            naming.close("orders", "DEFAULT_GROUP");
        });

        auto ready = co_await lease->wait_ready();
        EXPECT_FALSE(ready);
        if (!ready) {
            EXPECT_EQ(ready.error(), fiber::nacos::ServiceReadyError::Closed);
        }
        EXPECT_TRUE(discovery.empty());
        EXPECT_EQ(counters.creates, 0U);

        auto replacement = discovery.acquire("orders", "DEFAULT_GROUP");
        EXPECT_TRUE(replacement);
        EXPECT_EQ(naming.subscriptions("orders", "DEFAULT_GROUP"), 2U);
        if (replacement) {
            replacement->reset();
        }
        lease->reset();
        co_await discovery.shutdown();
        completed = true;
        loop.stop();
    });

    loop.run();
    EXPECT_TRUE(completed);
}

TEST(ServiceDiscoveryTest, ClosedAfterFirstNotifyRetiresStateOnce) {
    fiber::event::EventLoop loop;
    FakeNamingService naming;
    FakeOpsCounters counters;
    FakeServiceDiscovery discovery(loop, naming, FakeStateOps{.counters = &counters});
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto lease = discovery.acquire("orders", "DEFAULT_GROUP");
        EXPECT_TRUE(lease);
        if (!lease) {
            co_await discovery.shutdown();
            completed = true;
            loop.stop();
            co_return;
        }

        fiber::tests::ServiceInfoTestData info;
        info.name = "orders";
        info.group_name = "DEFAULT_GROUP";
        info.last_ref_time = 7;
        naming.push("orders", "DEFAULT_GROUP", std::move(info));
        naming.close("orders", "DEFAULT_GROUP");
        EXPECT_TRUE(discovery.empty());
        EXPECT_EQ(counters.retires, 1U);
        EXPECT_EQ(counters.last_retire_reason, fiber::nacos::ServiceRetireReason::SubscriptionClosed);
        EXPECT_EQ(counters.destroys, 0U);
        EXPECT_EQ(lease->state().last_ref_time, 7);

        lease->reset();
        EXPECT_EQ(counters.retires, 1U);
        EXPECT_EQ(counters.destroys, 1U);
        co_await discovery.shutdown();
        completed = true;
        loop.stop();
    });

    loop.run();
    EXPECT_TRUE(completed);
}

TEST(ServiceDiscoveryTest, ShutdownBeforeFirstNotifyWakesWaiter) {
    fiber::event::EventLoop loop;
    FakeNamingService naming;
    FakeOpsCounters counters;
    FakeServiceDiscovery discovery(loop, naming, FakeStateOps{.counters = &counters});
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto lease = discovery.acquire("orders", "DEFAULT_GROUP");
        EXPECT_TRUE(lease);
        if (!lease) {
            co_await discovery.shutdown();
            completed = true;
            loop.stop();
            co_return;
        }

        fiber::async::spawn([&]() -> fiber::async::DetachedTask {
            co_await fiber::async::yield();
            co_await discovery.shutdown();
        });

        auto ready = co_await lease->wait_ready();
        EXPECT_FALSE(ready);
        if (!ready) {
            EXPECT_EQ(ready.error(), fiber::nacos::ServiceReadyError::Shutdown);
        }
        EXPECT_TRUE(discovery.empty());
        lease->reset();
        co_await discovery.shutdown();
        completed = true;
        loop.stop();
    });

    loop.run();
    EXPECT_TRUE(completed);
}

TEST(ServiceDiscoveryTest, WorkerCanDestroyLastLeaseAndShutdownWaitsForOwnerRelease) {
    fiber::event::EventLoop loop;
    FakeNamingService naming;
    FakeOpsCounters counters;
    FakeServiceDiscovery discovery(loop, naming, FakeStateOps{.counters = &counters});
    bool shutdown_completed = false;
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto acquired = discovery.acquire("orders", "DEFAULT_GROUP");
        EXPECT_TRUE(acquired);
        if (!acquired) {
            co_await discovery.shutdown();
            completed = true;
            loop.stop();
            co_return;
        }

        fiber::tests::ServiceInfoTestData info;
        info.name = "orders";
        info.group_name = "DEFAULT_GROUP";
        info.last_ref_time = 13;
        naming.push("orders", "DEFAULT_GROUP", std::move(info));
        auto ready = co_await acquired->wait_ready();
        EXPECT_TRUE(ready);
        if (!ready) {
            acquired->reset();
            co_await discovery.shutdown();
            completed = true;
            loop.stop();
            co_return;
        }

        fiber::async::spawn([&]() -> fiber::async::DetachedTask {
            co_await discovery.shutdown();
            shutdown_completed = true;
            EXPECT_EQ(counters.destroys, 1U);
            EXPECT_TRUE(discovery.empty());
            co_await discovery.shutdown();
            completed = true;
            loop.stop();
        });
        co_await fiber::async::yield();
        EXPECT_FALSE(shutdown_completed);

        std::thread worker([lease = std::move(*acquired)]() mutable { lease.reset(); });
        worker.join();
        EXPECT_EQ(counters.retires, 1U);
        EXPECT_EQ(counters.destroys, 0U);
        co_return;
    });

    loop.run();
    EXPECT_TRUE(completed);
}

TEST(ServiceDiscoveryTest, ConcurrentWorkerLeaseDestructionIsCoalescedOnOwnerLoop) {
    fiber::event::EventLoop loop;
    FakeNamingService naming;
    FakeOpsCounters counters;
    FakeServiceDiscovery discovery(loop, naming, FakeStateOps{.counters = &counters});
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        constexpr std::size_t kLeaseCount = 16;
        std::vector<FakeServiceDiscovery::Lease> leases;
        leases.reserve(kLeaseCount);
        for (std::size_t i = 0; i < kLeaseCount; ++i) {
            auto acquired = discovery.acquire("orders", "DEFAULT_GROUP");
            EXPECT_TRUE(acquired);
            if (acquired) {
                leases.push_back(std::move(*acquired));
            }
        }
        EXPECT_EQ(leases.size(), kLeaseCount);

        fiber::tests::ServiceInfoTestData info;
        info.name = "orders";
        info.group_name = "DEFAULT_GROUP";
        naming.push("orders", "DEFAULT_GROUP", std::move(info));

        fiber::async::spawn([&]() -> fiber::async::DetachedTask {
            co_await discovery.shutdown();
            EXPECT_EQ(counters.retires, 1U);
            EXPECT_EQ(counters.destroys, 1U);
            completed = true;
            loop.stop();
        });
        co_await fiber::async::yield();

        std::atomic<bool> release{false};
        std::vector<std::thread> workers;
        workers.reserve(leases.size());
        for (FakeServiceDiscovery::Lease &lease: leases) {
            workers.emplace_back([lease = std::move(lease), &release]() mutable {
                while (!release.load(std::memory_order_acquire)) {
                }
                lease.reset();
            });
        }
        release.store(true, std::memory_order_release);
        for (std::thread &worker: workers) {
            worker.join();
        }
        co_return;
    });

    loop.run();
    EXPECT_TRUE(completed);
}

} // namespace
