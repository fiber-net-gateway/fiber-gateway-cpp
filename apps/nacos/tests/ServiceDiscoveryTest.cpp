#include <gtest/gtest.h>

#include <algorithm>
#include <expected>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <async/Spawn.h>
#include <async/Yield.h>
#include <event/EventLoop.h>
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

fiber::async::Task<void> yield_updates() {
    for (std::size_t i = 0; i < 8; ++i) {
        co_await fiber::async::yield();
    }
}

struct ObserverState {
    std::size_t updates = 0;
    std::size_t first_updates = 0;
};

void observe_update(void *context, fiber::nacos::LoadBalancer &, std::string_view, std::string_view, bool first_update,
                    fiber::nacos::LoadBalancerUpdateResult) {
    auto &state = *static_cast<ObserverState *>(context);
    ++state.updates;
    state.first_updates += first_update ? 1U : 0U;
}

TEST(ServiceDiscoveryTest, SharesSubscriptionsAndPublishesEmptyThenHostnameGenerations) {
    fiber::event::EventLoop loop;
    FakeNamingService naming;
    ObserverState observer;
    fiber::nacos::ServiceDiscovery discovery(loop, naming, {},
                                             fiber::nacos::ServiceDiscoveryObserver{
                                                     .context = &observer,
                                                     .on_update = &observe_update,
                                             });
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto first = discovery.acquire("orders", "DEFAULT_GROUP");
        auto second = discovery.acquire("orders", "DEFAULT_GROUP");
        EXPECT_TRUE(first);
        EXPECT_TRUE(second);
        EXPECT_EQ(discovery.size(), 1U);
        EXPECT_EQ(naming.subscriptions("orders", "DEFAULT_GROUP"), 1U);

        fiber::tests::ServiceInfoTestData empty;
        empty.name = "orders";
        empty.group_name = "DEFAULT_GROUP";
        naming.push("orders", "DEFAULT_GROUP", std::move(empty));
        co_await yield_updates();

        if (first) {
            EXPECT_TRUE(first->load_balancer().initialized());
            EXPECT_EQ(first->load_balancer().configured_instance_count(), 0U);
        }
        EXPECT_EQ(observer.first_updates, 1U);

        fiber::tests::ServiceInfoTestData hostname;
        hostname.name = "orders";
        hostname.group_name = "DEFAULT_GROUP";
        hostname.hosts = {
                fiber::nacos::Instance{
                        .ip = "orders.internal",
                        .port = 8080,
                        .cluster_name = "sh-default",
                },
        };
        naming.push("orders", "DEFAULT_GROUP", std::move(hostname));
        co_await yield_updates();

        if (first && second) {
            EXPECT_EQ(first->shared_load_balancer(), second->shared_load_balancer());
            auto selected = first->load_balancer().load_balance(fiber::nacos::ServiceInstanceSelection{
                    .cluster = "default",
                    .preferred_zone = "sh",
            });
            EXPECT_TRUE(selected);
            if (selected) {
                EXPECT_EQ(selected->host(), "orders.internal");
                EXPECT_EQ(selected->authority(), "orders.internal:8080");
                EXPECT_FALSE(selected->ip_address());
                selected->report(fiber::nacos::InstanceReportOutcome::Neutral);
            }
        }
        EXPECT_EQ(observer.updates, 2U);

        if (first) {
            first->reset();
        }
        EXPECT_EQ(discovery.size(), 1U);
        if (second) {
            second->reset();
        }
        EXPECT_TRUE(discovery.empty());
        co_await discovery.shutdown();
        completed = true;
        loop.stop();
    });

    loop.run();
    EXPECT_TRUE(completed);
}

} // namespace
