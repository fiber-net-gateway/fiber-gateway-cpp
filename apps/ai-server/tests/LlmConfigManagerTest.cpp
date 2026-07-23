#include <gtest/gtest.h>

#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <async/Spawn.h>
#include <async/Watch.h>
#include <async/Yield.h>
#include <event/EventLoop.h>
#include <event/EventLoopGroup.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/Subscription.h>

#include "AiServer.h"
#include "config/LlmConfigManager.h"

namespace {

class FakeConfigService final : public fiber::nacos::ConfigService {
public:
    using Result = fiber::nacos::SubscriptionResult<fiber::nacos::ConfigData>;
    using Watch = fiber::async::Watch<Result>;

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
    subscribe(std::string_view data_id, std::string_view group) override {
        const std::string key = make_key(data_id, group);
        auto [it, inserted] = entries_.try_emplace(key, std::make_unique<Entry>());
        (void) inserted;
        return fiber::nacos::Subscription<fiber::nacos::ConfigData>({}, it->second->watch.subscribe());
    }

    void push(std::string_view data_id, std::string content, std::string md5) {
        const auto it = entries_.find(make_key(data_id, fiber::ai_server::kLlmConfigGroup));
        ASSERT_NE(it, entries_.end());
        it->second->publisher->publish(Result{
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
        const auto it = entries_.find(make_key(data_id, fiber::ai_server::kLlmConfigGroup));
        ASSERT_NE(it, entries_.end());
        it->second->publisher->publish(Result{
                .kind = fiber::nacos::ResultKind::Success,
                .data =
                        fiber::nacos::ConfigData{
                                .state = fiber::nacos::ConfigState::NotFound,
                        },
        });
    }

private:
    struct Entry {
        Entry() {
            publisher = watch.acquire_publisher();
            EXPECT_TRUE(publisher.has_value());
        }

        Watch watch;
        std::optional<Watch::Publisher> publisher;
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
    co_await fiber::async::yield();
    co_await fiber::async::yield();
    co_await fiber::async::yield();
}

TEST(LlmConfigManagerTest, PublishesSnapshotsAndRetainsLastValidDynamicConfig) {
    fiber::event::EventLoop loop;
    FakeConfigService service;
    fiber::ai_server::LlmConfigManager manager(loop, service);
    auto serving = manager.subscribe_serving();
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto started = manager.start();
        EXPECT_TRUE(started);

        service.push(fiber::ai_server::kBt1KeysDataId,
                     R"({"version":1,"data":{"keys":[{"kid":"main","secret":"secret"}]}})", "bt1-v1");
        service.push(fiber::ai_server::kModelsDataId,
                     R"({"version":1,"data":[{
                         "model-name":"chat",
                         "providers":["openai"],
                         "allow-user-groups":["staff"]
                     }]})",
                     "models-v1");
        co_await yield_updates();

        EXPECT_FALSE(manager.ready());
        EXPECT_EQ(serving.current().value, nullptr);
        auto initial_project = manager.current_project();
        EXPECT_NE(initial_project, nullptr);
        EXPECT_EQ(initial_project->metadata().version, 1);
        EXPECT_EQ(manager.provider_subscription_count(), 1u);
        EXPECT_EQ(manager.user_group_subscription_count(), 1u);
        const auto *initial_provider = initial_project->find_provider("openai");
        EXPECT_NE(initial_provider, nullptr);
        EXPECT_EQ(initial_provider->config, nullptr);

        service.push("ploto.ai-llm.provider.openai",
                     R"({"version":2,"data":{
                         "provider":"openai",
                         "baseurl":"https://api.example.test",
                         "api-tokens":[],
                         "protocol":[{
                             "type":"openai-chat-completions",
                             "path":"/v1/chat/completions",
                             "model":"gpt-test"
                         }]
                     }})",
                     "provider-v2");
        co_await yield_updates();

        auto provider_project = manager.current_project();
        EXPECT_NE(provider_project, nullptr);
        EXPECT_GT(provider_project->generation(), initial_project->generation());
        const auto *provider = provider_project->find_provider("openai");
        EXPECT_NE(provider, nullptr);
        EXPECT_NE(provider->config, nullptr);
        EXPECT_EQ(provider->config->metadata.version, 2);
        EXPECT_FALSE(manager.ready());
        EXPECT_EQ(serving.current().value, nullptr);

        const auto *route = provider_project->find_model("chat");
        EXPECT_NE(route, nullptr);
        EXPECT_EQ(route->allow_user_groups.size(), 1u);
        const auto group_state = route->allow_user_groups[0];
        const std::uint64_t generation_before_group = provider_project->generation();
        service.push("ploto.ai-llm.user-group.staff",
                     R"({"version":3,"data":{"name":"staff","users":["alice","alice","bob"]}})", "group-v3");
        co_await yield_updates();

        EXPECT_EQ(manager.current_project()->generation(), generation_before_group);
        auto group = group_state->current();
        EXPECT_NE(group, nullptr);
        EXPECT_TRUE(group->contains("alice"));
        EXPECT_TRUE(group->contains("bob"));
        EXPECT_FALSE(group->contains("mallory"));
        EXPECT_TRUE(manager.ready());
        auto serving_snapshot = serving.current();
        EXPECT_NE(serving_snapshot.value, nullptr);
        EXPECT_EQ(serving_snapshot.value->bt1_keys->metadata.version, 1);
        EXPECT_EQ(serving_snapshot.value->project->generation(), provider_project->generation());

        const std::uint64_t failed_before = manager.failed_updates();
        service.push("ploto.ai-llm.provider.openai",
                     R"({"version":0,"data":{"provider":"wrong","baseurl":"https://bad","protocol":[]}})",
                     "provider-invalid");
        co_await yield_updates();

        EXPECT_EQ(manager.failed_updates(), failed_before + 1);
        EXPECT_EQ(manager.current_project()->generation(), generation_before_group);
        const auto *retained = manager.current_project()->find_provider("openai");
        EXPECT_NE(retained, nullptr);
        EXPECT_NE(retained->config, nullptr);
        EXPECT_EQ(retained->config->metadata.version, 2);

        service.push(fiber::ai_server::kModelsDataId, R"({"version":0,"data":[]})", "models-empty");
        co_await yield_updates();
        EXPECT_EQ(manager.current_project()->metadata().version, 0);
        EXPECT_EQ(manager.provider_subscription_count(), 0u);
        EXPECT_EQ(manager.user_group_subscription_count(), 0u);
        EXPECT_NE(initial_project->find_model("chat"), nullptr);

        co_await manager.shutdown();
        completed = true;
        loop.stop();
    });

    loop.run();
    EXPECT_TRUE(completed);
    EXPECT_EQ(manager.state(), fiber::ai_server::LlmConfigManagerState::Stopped);
}

TEST(LlmConfigManagerTest, InitialSyncWaitsForLatestReferencedConfiguration) {
    fiber::event::EventLoop loop;
    FakeConfigService service;
    fiber::ai_server::LlmConfigManager manager(loop, service);
    auto serving = manager.subscribe_serving();
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        EXPECT_TRUE(manager.start());
        service.push(fiber::ai_server::kBt1KeysDataId,
                     R"({"version":1,"data":{"keys":[{"kid":"main","secret":"secret"}]}})", "bt1");
        service.push(fiber::ai_server::kModelsDataId,
                     R"({"version":1,"data":[{"model-name":"chat","providers":["old"]}]})", "models-old");
        co_await yield_updates();
        service.push_not_found("ploto.ai-llm.provider.old");
        co_await yield_updates();
        EXPECT_FALSE(manager.ready());

        service.push(fiber::ai_server::kModelsDataId,
                     R"({"version":2,"data":[{"model-name":"chat","providers":["new"]}]})", "models-new");
        co_await yield_updates();
        EXPECT_EQ(manager.provider_subscription_count(), 1u);
        EXPECT_FALSE(manager.ready());

        service.push("ploto.ai-llm.provider.new",
                     R"({"version":1,"data":{
                         "provider":"new",
                         "baseurl":"https://api.example.test",
                         "api-tokens":[],
                         "protocol":[{
                             "type":"openai-chat-completions",
                             "path":"/v1/chat/completions",
                             "model":"gpt-test"
                         }]
                     }})",
                     "provider-new");
        for (std::size_t i = 0; i < 16 && manager.successful_updates() != 4; ++i) {
            co_await fiber::async::yield();
        }

        EXPECT_EQ(manager.successful_updates(), 4u);
        EXPECT_TRUE(manager.ready());
        auto snapshot = serving.current();
        EXPECT_NE(snapshot.value, nullptr);
        if (!snapshot.value) {
            co_await manager.shutdown();
            completed = true;
            loop.stop();
            co_return;
        }
        EXPECT_EQ(snapshot.value->project->metadata().version, 2);
        EXPECT_NE(snapshot.value->project->find_provider("new")->config, nullptr);
        EXPECT_EQ(snapshot.value->project->find_provider("old"), nullptr);

        co_await manager.shutdown();
        completed = true;
        loop.stop();
    });

    loop.run();
    EXPECT_TRUE(completed);
}

TEST(LlmConfigManagerTest, HttpWorkersInstallInitialSnapshotBeforeBind) {
    fiber::event::EventLoop accept_loop;
    fiber::event::EventLoopGroup workers(2);
    workers.start();
    FakeConfigService service;
    fiber::ai_server::LlmConfigManager manager(accept_loop, service);
    fiber::ai_server::AiServer server(accept_loop, workers);
    bool completed = false;

    fiber::async::spawn(accept_loop, [&]() -> fiber::async::DetachedTask {
        EXPECT_TRUE(manager.start());
        service.push(fiber::ai_server::kBt1KeysDataId,
                     R"({"version":1,"data":{"keys":[{"kid":"main","secret":"secret"}]}})", "bt1");
        service.push(fiber::ai_server::kModelsDataId, R"({"version":1,"data":[]})", "models");
        co_await yield_updates();
        EXPECT_TRUE(manager.ready());

        EXPECT_LT(server.fd(), 0);
        EXPECT_TRUE(co_await server.start_config_workers(manager));
        EXPECT_LT(server.fd(), 0);

        fiber::net::ListenOptions options;
        auto bound = server.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), options);
        EXPECT_TRUE(bound);
        EXPECT_GE(server.fd(), 0);

        co_await server.shutdown_and_wait();
        co_await manager.shutdown();
        completed = true;
        accept_loop.stop();
    });

    accept_loop.run();
    workers.stop();
    workers.join();
    EXPECT_TRUE(completed);
}

} // namespace
