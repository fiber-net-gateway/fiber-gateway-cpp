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
#include "config/AccessConfigCodec.h"
#include "runtime/GrayConfigWatcher.h"

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

    void push(std::string content, std::string md5 = {}) {
        const auto iterator = entries_.find(
                make_key(fiber::access_server::kGrayConfigDataId, fiber::access_server::kDefaultNacosGroup));
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

TEST(GrayConfigTest, DecodesJavaMapAndAppliesRatioAndCidrRules) {
    auto decoded =
            fiber::access_server::parse_gray_match_config(R"({"vdi":{"ratio":1000,"cidrs":["10.0.0.0/8","bad"]},)"
                                                          R"("desktop":{"ratio":10001},"unknown":{"ratio":10000}})");
    ASSERT_TRUE(decoded);
    ASSERT_TRUE(*decoded);
    ASSERT_EQ((*decoded)->size(), 3u);

    fiber::access_server::GrayMatchStore store;
    auto applied = store.apply(*decoded);
    ASSERT_TRUE(applied);
    EXPECT_EQ(store.rule_count(), 2u);

    EXPECT_TRUE(store.matches("vdi", "10.1.2.3", 9999));
    EXPECT_TRUE(store.matches("vdi", "192.0.2.1", 999));
    EXPECT_FALSE(store.matches("vdi", "192.0.2.1", 1000));
    EXPECT_TRUE(store.matches("desktop", "192.0.2.1", 9999));
    EXPECT_FALSE(store.matches("unknown", "10.1.2.3", 0));

    auto empty_wire = fiber::access_server::parse_gray_match_config("");
    ASSERT_TRUE(empty_wire);
    EXPECT_FALSE(*empty_wire);
    EXPECT_EQ(store.apply(*empty_wire), fiber::access_server::GrayMatchUpdateStatus::IgnoredEmpty);
    EXPECT_EQ(store.rule_count(), 2u);

    auto clear = fiber::access_server::parse_gray_match_config("null");
    ASSERT_TRUE(clear);
    ASSERT_TRUE(*clear);
    EXPECT_TRUE((*clear)->empty());
    EXPECT_EQ(store.apply(*clear), fiber::access_server::GrayMatchUpdateStatus::Published);
    EXPECT_EQ(store.rule_count(), 0u);
}

TEST(GrayConfigTest, WatcherRetainsOnEmptyAndInvalidThenAcceptsClear) {
    fiber::event::EventLoop loop;
    FakeConfigService service;
    fiber::access_server::GrayMatchStore store;
    fiber::access_server::GrayConfigWatcher watcher(loop, service, store);
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        EXPECT_TRUE(watcher.start());
        service.push(R"({"internet":{"ratio":5000,"cidrs":["2001:db8::/32"]}})", "v1");
        co_await yield_updates();
        EXPECT_EQ(store.rule_count(), 1u);
        EXPECT_TRUE(store.matches("internet", "2001:db8::1", 9999));

        service.push("", "empty");
        co_await yield_updates();
        EXPECT_EQ(store.rule_count(), 1u);

        service.push("{", "invalid");
        co_await yield_updates();
        EXPECT_EQ(store.rule_count(), 1u);
        EXPECT_EQ(watcher.failed_updates(), 1u);
        EXPECT_TRUE(watcher.last_failure());

        service.push("{}", "clear");
        co_await yield_updates();
        EXPECT_EQ(store.rule_count(), 0u);
        EXPECT_EQ(watcher.successful_updates(), 2u);

        co_await watcher.shutdown();
        completed = true;
        loop.stop();
    });

    loop.run();
    EXPECT_TRUE(completed);
    EXPECT_EQ(watcher.state(), fiber::access_server::GrayConfigWatcherState::Stopped);
}

} // namespace
