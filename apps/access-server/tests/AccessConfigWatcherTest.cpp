#include <gtest/gtest.h>

#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <async/Spawn.h>
#include <async/Yield.h>
#include <event/EventLoop.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/Subscription.h>

#include "../../../tests/NacosSubscriptionStub.h"
#include "runtime/AccessConfigWatcher.h"

namespace {

class FakeConfigService final : public fiber::nacos::ConfigService {
public:
    using Result = fiber::nacos::SubscriptionResult<fiber::nacos::ConfigData>;

    fiber::common::IoResult<void> start() noexcept override { return {}; }
    fiber::async::Task<void> shutdown() noexcept override { co_return; }

    fiber::async::Task<std::expected<std::optional<fiber::nacos::ConfigData>, fiber::nacos::ConfigServiceError>>
    get_config(std::string, std::string) noexcept override {
        co_return std::optional<fiber::nacos::ConfigData>{};
    }

    fiber::async::Task<std::expected<void, fiber::nacos::ConfigServiceError>>
    publish(std::string, std::string, std::string, fiber::nacos::ConfigType,
            std::optional<std::string>) noexcept override {
        co_return std::expected<void, fiber::nacos::ConfigServiceError>{};
    }

    fiber::async::Task<std::expected<void, fiber::nacos::ConfigServiceError>>
    remove_config(std::string, std::string) noexcept override {
        co_return std::expected<void, fiber::nacos::ConfigServiceError>{};
    }

    std::expected<fiber::nacos::Subscription<fiber::nacos::ConfigData>, fiber::nacos::ConfigServiceError>
    subscribe(std::string_view data_id, std::string_view group,
              fiber::nacos::Subscription<fiber::nacos::ConfigData>::NotifyCallback on_notify, void *ctx) override {
        const std::string key = make_key(data_id, group);
        auto [iterator, inserted] = entries_.try_emplace(key, std::make_unique<Entry>());
        (void) inserted;
        return iterator->second->subscriptions.subscribe(on_notify, ctx);
    }

    void push(std::string_view data_id, std::string content, std::string md5 = {}) {
        const auto iterator = entries_.find(make_key(data_id, fiber::access_server::kProjectRouteGroup));
        ASSERT_NE(iterator, entries_.end());
        iterator->second->subscriptions.publish(Result{
                .kind = fiber::nacos::ResultKind::Success,
                .data =
                        fiber::nacos::ConfigData{
                                .state = fiber::nacos::ConfigState::Present,
                                .md5 = std::move(md5),
                                .content = std::move(content),
                        },
        });
    }

    void push_not_found(std::string_view data_id) {
        const auto iterator = entries_.find(make_key(data_id, fiber::access_server::kProjectRouteGroup));
        ASSERT_NE(iterator, entries_.end());
        iterator->second->subscriptions.publish(Result{
                .kind = fiber::nacos::ResultKind::Success,
                .data =
                        fiber::nacos::ConfigData{
                                .state = fiber::nacos::ConfigState::NotFound,
                        },
        });
    }

    [[nodiscard]] std::size_t subscriptions(std::string_view data_id) const {
        const auto iterator = entries_.find(make_key(data_id, fiber::access_server::kProjectRouteGroup));
        return iterator == entries_.end() ? 0 : iterator->second->subscriptions.subscription_count();
    }

private:
    struct Entry {
        fiber::tests::NacosSubscriptionStub<fiber::nacos::ConfigData> subscriptions;
    };

    static std::string make_key(std::string_view data_id, std::string_view group) {
        std::string key(data_id);
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

std::string route_config(std::int32_t version, std::string_view host, std::string_view service) {
    return std::string("{\"version\":") + std::to_string(version) + ",\"host\":{\"" + std::string(host) +
           "\":{}},\"routes\":[{\"path\":\"/\",\"service\":\"" + std::string(service) + "\"}]}";
}

TEST(AccessConfigWatcherTest, ReconcilesProjectsAndRetainsLastValidSnapshots) {
    fiber::event::EventLoop loop;
    FakeConfigService service;
    fiber::access_server::RouteConfigStore store;
    std::size_t observer_updates = 0;
    fiber::access_server::RouteSnapshotObserver observer{
            .context = &observer_updates,
            .on_update =
                    [](void *context, std::shared_ptr<const fiber::access_server::AccessRouteSnapshot>) noexcept {
                        ++*static_cast<std::size_t *>(context);
                    },
    };
    fiber::access_server::AccessConfigWatcher watcher(loop, service, store, {}, observer);
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto ready = watcher.subscribe_ready();
        auto ready_snapshot = ready.current();
        EXPECT_TRUE(ready_snapshot.value);
        if (ready_snapshot.value) {
            EXPECT_FALSE(*ready_snapshot.value);
        }
        EXPECT_TRUE(watcher.start());
        EXPECT_EQ(service.subscriptions(fiber::access_server::kProjectListDataId), 1u);

        service.push(fiber::access_server::kProjectListDataId, "a;b");
        ready_snapshot = co_await ready.next(ready_snapshot.version);
        EXPECT_TRUE(ready_snapshot.value);
        if (ready_snapshot.value) {
            EXPECT_TRUE(*ready_snapshot.value);
        }
        EXPECT_TRUE(watcher.initial_project_list_received());
        EXPECT_EQ(watcher.project_subscription_count(), 2u);
        EXPECT_EQ(service.subscriptions("ploto.unified-access.route.a"), 1u);
        EXPECT_EQ(service.subscriptions("ploto.unified-access.route.b"), 1u);

        service.push("ploto.unified-access.route.a", route_config(1, "a.example.com", "orders"), "a1");
        service.push("ploto.unified-access.route.b", route_config(1, "b.example.com", "billing"), "b1");
        co_await yield_updates();
        EXPECT_TRUE(store.pin()->match_host("a.example.com"));
        EXPECT_TRUE(store.pin()->match_host("b.example.com"));
        const auto valid = store.pin();

        service.push("ploto.unified-access.route.a", route_config(1, "changed.example.com", "orders"), "same");
        service.push("ploto.unified-access.route.b", "{", "invalid");
        co_await yield_updates();
        EXPECT_EQ(store.pin(), valid);
        EXPECT_FALSE(store.pin()->match_host("changed.example.com"));
        EXPECT_EQ(watcher.failed_updates(), 1u);
        EXPECT_TRUE(watcher.last_failure());
        if (watcher.last_failure()) {
            EXPECT_EQ(watcher.last_failure()->data_id, "ploto.unified-access.route.b");
        }

        service.push("ploto.unified-access.route.a", "", "empty");
        co_await yield_updates();
        EXPECT_EQ(store.pin(), valid);

        service.push(fiber::access_server::kProjectListDataId, "a;b;c");
        co_await yield_updates();
        EXPECT_EQ(watcher.project_subscription_count(), 3u);
        EXPECT_EQ(service.subscriptions("ploto.unified-access.route.c"), 1u);

        service.push(fiber::access_server::kProjectListDataId, "b;c");
        co_await yield_updates();
        EXPECT_EQ(watcher.project_subscription_count(), 2u);
        EXPECT_FALSE(store.pin()->match_host("a.example.com"));
        EXPECT_TRUE(store.pin()->match_host("b.example.com"));

        service.push_not_found(fiber::access_server::kProjectListDataId);
        co_await yield_updates();
        EXPECT_EQ(watcher.project_subscription_count(), 0u);
        EXPECT_TRUE(store.pin()->projects().empty());
        EXPECT_GE(observer_updates, 4u);

        co_await watcher.shutdown();
        completed = true;
        loop.stop();
    });

    loop.run();
    EXPECT_TRUE(completed);
    EXPECT_EQ(watcher.state(), fiber::access_server::AccessConfigWatcherState::Stopped);
}

TEST(AccessConfigWatcherTest, ShutdownWinsAgainstQueuedConfigCallbacks) {
    fiber::event::EventLoop loop;
    FakeConfigService service;
    fiber::access_server::RouteConfigStore store;
    fiber::access_server::AccessConfigWatcher watcher(loop, service, store);
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        EXPECT_TRUE(watcher.start());
        service.push(fiber::access_server::kProjectListDataId, "a;b");
        co_await yield_updates();
        service.push("ploto.unified-access.route.a", route_config(1, "a.example.com", "orders"));
        service.push(fiber::access_server::kProjectListDataId, "b;c");
        co_await watcher.shutdown();
        EXPECT_EQ(watcher.project_subscription_count(), 0u);
        completed = true;
        loop.stop();
    });

    loop.run();
    EXPECT_TRUE(completed);
}

} // namespace
