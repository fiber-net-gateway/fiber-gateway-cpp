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
#include <fiber/nacos/NamingService.h>
#include <fiber/nacos/Subscription.h>

#include "../../../tests/NacosSnapshotTestBuilder.h"
#include "../../../tests/NacosSubscriptionStub.h"
#include "AiServer.h"
#include "config/LlmConfigManager.h"
#include "discovery/WeightedRendezvous.h"

namespace {

class FakeConfigService final : public fiber::nacos::ConfigService {
public:
    using Result = fiber::nacos::SubscriptionResult<fiber::nacos::ConfigData>;

    fiber::common::IoResult<void> start() noexcept override { return {}; }

    fiber::async::Task<void> shutdown() noexcept override { co_return; }

    fiber::async::Task<std::expected<std::shared_ptr<const fiber::nacos::ConfigData>, fiber::nacos::ConfigServiceError>>
    get_config(std::string, std::string) noexcept override {
        co_return fiber::tests::make_config_data(fiber::nacos::ConfigState::NotFound);
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
        auto [it, inserted] = entries_.try_emplace(key, std::make_unique<Entry>());
        (void) inserted;
        return it->second->subscriptions.subscribe(on_notify, ctx);
    }

    void push(std::string_view data_id, std::string content, std::string md5) {
        const auto it = entries_.find(make_key(data_id, fiber::ai_server::kLlmConfigGroup));
        ASSERT_NE(it, entries_.end());
        it->second->subscriptions.publish(Result{
                .kind = fiber::nacos::ResultKind::Success,
                .data = fiber::tests::make_config_data(fiber::nacos::ConfigState::Present, std::move(md5),
                                                       std::move(content)),
        });
    }

    void push_not_found(std::string_view data_id) {
        const auto it = entries_.find(make_key(data_id, fiber::ai_server::kLlmConfigGroup));
        ASSERT_NE(it, entries_.end());
        it->second->subscriptions.publish(Result{
                .kind = fiber::nacos::ResultKind::Success,
                .data = fiber::tests::make_config_data(fiber::nacos::ConfigState::NotFound),
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

class FakeNamingService final : public fiber::nacos::NamingService {
public:
    using Result = fiber::nacos::SubscriptionResult<fiber::nacos::ServiceInfo>;

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

    std::expected<fiber::nacos::Subscription<fiber::nacos::ServiceInfo>, fiber::nacos::NamingServiceError>
    subscribe(std::string_view service_name, std::string_view group,
              fiber::nacos::Subscription<fiber::nacos::ServiceInfo>::NotifyCallback on_notify, void *ctx) override {
        const std::string key = make_key(service_name, group);
        auto [it, inserted] = entries_.try_emplace(key, std::make_unique<Entry>());
        (void) inserted;
        return it->second->subscriptions.subscribe(on_notify, ctx);
    }

    std::expected<fiber::nacos::InstanceRegistration, fiber::nacos::NamingServiceError>
    registry(std::string_view, std::string_view, fiber::nacos::Instance) override {
        return std::unexpected(fiber::nacos::NamingServiceError{
                .code = fiber::nacos::NamingServiceErrorCode::Server,
                .message = "not implemented by fake naming service",
        });
    }

    void push(std::string_view service_name, fiber::tests::ServiceInfoTestData info) {
        const auto it = entries_.find(make_key(service_name, fiber::ai_server::kDefaultNamingGroup));
        ASSERT_NE(it, entries_.end());
        it->second->subscriptions.publish(Result{
                .kind = fiber::nacos::ResultKind::Success,
                .data = fiber::tests::make_service_info(std::move(info)),
        });
    }

private:
    struct Entry {
        fiber::tests::NacosSubscriptionStub<fiber::nacos::ServiceInfo> subscriptions;
    };

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

TEST(LlmConfigManagerTest, PublishesSnapshotsAndRetainsLastValidDynamicConfig) {
    fiber::event::EventLoop loop;
    FakeConfigService service;
    FakeNamingService naming;
    fiber::ai_server::LlmConfigManager manager(loop, service, naming);
    auto serving = manager.subscribe_snapshot();
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
                         "allow-user-groups":["staff"],
                         "rate-limit":{"window-duration-millis":60000,"max-tokens-per-window":1000}
                     }]})",
                     "models-v1");
        co_await yield_updates();

        EXPECT_FALSE(manager.ready());
        EXPECT_EQ(serving.current().value, nullptr);
        EXPECT_EQ(manager.current_project(), nullptr);
        EXPECT_EQ(manager.provider_subscription_count(), 1u);
        EXPECT_EQ(manager.user_group_subscription_count(), 1u);

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

        EXPECT_EQ(manager.current_project(), nullptr);
        EXPECT_FALSE(manager.ready());
        EXPECT_EQ(serving.current().value, nullptr);

        service.push("ploto.ai-llm.user-group.staff",
                     R"({"version":3,"data":{"name":"staff","users":["alice","alice","bob"]}})", "group-v3");
        co_await yield_updates();

        auto complete_project = manager.current_project();
        EXPECT_NE(complete_project, nullptr);
        const auto *provider = complete_project->find_provider("openai");
        EXPECT_NE(provider, nullptr);
        EXPECT_NE(provider->config, nullptr);
        EXPECT_EQ(provider->config->metadata.version, 2);
        const auto *route = complete_project->find_model("chat");
        EXPECT_NE(route, nullptr);
        EXPECT_EQ(route->providers.size(), 1u);
        EXPECT_EQ(route->providers[0].get(), provider);
        EXPECT_EQ(route->allow_user_groups.size(), 1u);
        EXPECT_TRUE(route->rate_limit);
        const std::int64_t initial_rate_limit_revision = route->rate_limit ? route->rate_limit->revision : 0;
        const auto group = route->allow_user_groups[0];
        EXPECT_TRUE(group->contains("alice"));
        EXPECT_TRUE(group->contains("bob"));
        EXPECT_FALSE(group->contains("mallory"));
        EXPECT_TRUE(manager.ready());
        auto serving_snapshot = serving.current();
        EXPECT_NE(serving_snapshot.value, nullptr);
        EXPECT_EQ(serving_snapshot.value->bt1_keys->metadata.version, 1);
        EXPECT_EQ(serving_snapshot.value->project, complete_project);
        const auto initial_snapshot = serving_snapshot.value;
        const auto initial_bt1_keys = initial_snapshot->bt1_keys;

        const std::uint64_t failed_before_invalid_bt1 = manager.failed_updates();
        service.push(fiber::ai_server::kBt1KeysDataId,
                     R"({"version":2,"data":{"keys":[
                         {"kid":"duplicate","secret":"first"},
                         {"kid":"duplicate","secret":"second"}
                     ]}})",
                     "bt1-invalid");
        co_await yield_updates();

        EXPECT_EQ(manager.failed_updates(), failed_before_invalid_bt1 + 1);
        EXPECT_EQ(serving.current().value, initial_snapshot);
        EXPECT_EQ(manager.current_bt1_keys(), initial_bt1_keys);

        service.push(fiber::ai_server::kBt1KeysDataId,
                     R"({"version":2,"data":{"clockSkewSec":30,"keys":[
                         {"kid":"next","secret":"next-secret"}
                     ]}})",
                     "bt1-v2");
        co_await yield_updates();

        const auto rotated_bt1_snapshot = serving.current().value;
        EXPECT_NE(rotated_bt1_snapshot, initial_snapshot);
        EXPECT_EQ(rotated_bt1_snapshot->project, complete_project);
        EXPECT_EQ(rotated_bt1_snapshot->bt1_keys->metadata.version, 2);
        EXPECT_EQ(rotated_bt1_snapshot->bt1_keys->clock_skew_seconds, 30);
        EXPECT_EQ(rotated_bt1_snapshot->bt1_keys->find_key("main"), nullptr);
        EXPECT_NE(rotated_bt1_snapshot->bt1_keys->find_key("next"), nullptr);
        EXPECT_NE(initial_bt1_keys->find_key("main"), nullptr);
        EXPECT_EQ(initial_bt1_keys->find_key("next"), nullptr);

        service.push("ploto.ai-llm.user-group.staff", R"({"version":4,"data":{"name":"staff","users":["mallory"]}})",
                     "group-v4");
        co_await yield_updates();

        const auto updated_group_snapshot = serving.current().value;
        EXPECT_NE(updated_group_snapshot, initial_snapshot);
        EXPECT_GT(updated_group_snapshot->project->generation(), complete_project->generation());
        const auto *updated_route = updated_group_snapshot->project->find_model("chat");
        EXPECT_TRUE(updated_route->rate_limit);
        EXPECT_EQ(updated_route->rate_limit ? updated_route->rate_limit->revision : 0, initial_rate_limit_revision);
        EXPECT_TRUE(updated_route->allow_user_groups[0]->contains("mallory"));
        EXPECT_FALSE(updated_route->allow_user_groups[0]->contains("alice"));
        EXPECT_TRUE(initial_snapshot->project->find_model("chat")->allow_user_groups[0]->contains("alice"));
        EXPECT_FALSE(initial_snapshot->project->find_model("chat")->allow_user_groups[0]->contains("mallory"));
        const std::uint64_t generation_before_invalid = updated_group_snapshot->project->generation();

        const std::uint64_t failed_before_not_found = manager.failed_updates();
        service.push_not_found("ploto.ai-llm.provider.openai");
        co_await yield_updates();
        EXPECT_EQ(manager.failed_updates(), failed_before_not_found + 1);
        EXPECT_EQ(manager.current_project()->generation(), generation_before_invalid);

        const std::uint64_t failed_before = manager.failed_updates();
        service.push("ploto.ai-llm.provider.openai",
                     R"({"version":0,"data":{"provider":"wrong","baseurl":"https://bad","protocol":[]}})",
                     "provider-invalid");
        co_await yield_updates();

        EXPECT_EQ(manager.failed_updates(), failed_before + 1);
        EXPECT_EQ(manager.current_project()->generation(), generation_before_invalid);
        const auto *retained = manager.current_project()->find_provider("openai");
        EXPECT_NE(retained, nullptr);
        EXPECT_NE(retained->config, nullptr);
        EXPECT_EQ(retained->config->metadata.version, 2);

        service.push(fiber::ai_server::kModelsDataId, R"({"version":0,"data":[]})", "models-empty");
        co_await yield_updates();
        EXPECT_EQ(manager.current_project()->metadata().version, 0);
        EXPECT_EQ(manager.provider_subscription_count(), 0u);
        EXPECT_EQ(manager.user_group_subscription_count(), 0u);
        EXPECT_NE(initial_snapshot->project->find_model("chat"), nullptr);
        EXPECT_TRUE(initial_snapshot->project->find_model("chat")->allow_user_groups[0]->contains("alice"));

        co_await manager.shutdown();
        completed = true;
        loop.stop();
    });

    loop.run();
    EXPECT_TRUE(completed);
    EXPECT_EQ(manager.state(), fiber::ai_server::LlmConfigManagerState::Stopped);
}

TEST(LlmConfigManagerTest, ModelsCandidateRetainsActiveTreeUntilDependenciesAreReady) {
    fiber::event::EventLoop loop;
    FakeConfigService service;
    FakeNamingService naming;
    fiber::ai_server::LlmConfigManager manager(loop, service, naming);
    auto serving = manager.subscribe_snapshot();
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        EXPECT_TRUE(manager.start());
        service.push(fiber::ai_server::kBt1KeysDataId,
                     R"({"version":1,"data":{"keys":[{"kid":"main","secret":"secret"}]}})", "bt1");
        service.push(fiber::ai_server::kModelsDataId,
                     R"({"version":1,"data":[{"model-name":"chat","providers":["old"]}]})", "models-old");
        co_await yield_updates();
        service.push("ploto.ai-llm.provider.old",
                     R"({"version":1,"data":{
                         "provider":"old",
                         "baseurl":"https://old.example.test",
                         "api-tokens":[],
                         "protocol":[{
                             "type":"openai-chat-completions",
                             "path":"/v1/chat/completions",
                             "model":"old-model"
                         }]
                     }})",
                     "provider-old");
        co_await yield_updates();
        EXPECT_TRUE(manager.ready());
        auto old_snapshot = serving.current().value;
        EXPECT_NE(old_snapshot, nullptr);
        EXPECT_EQ(old_snapshot->project->metadata().version, 1);

        service.push(fiber::ai_server::kModelsDataId,
                     R"({"version":2,"data":[{"model-name":"chat","providers":["new"]}]})", "models-new");
        co_await yield_updates();
        EXPECT_EQ(manager.provider_subscription_count(), 2u);
        EXPECT_TRUE(manager.ready());
        EXPECT_EQ(serving.current().value, old_snapshot);
        EXPECT_EQ(manager.current_project()->metadata().version, 1);

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
        co_await yield_updates();

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
        EXPECT_EQ(manager.provider_subscription_count(), 1u);
        EXPECT_NE(old_snapshot->project->find_provider("old"), nullptr);
        EXPECT_EQ(old_snapshot->project->find_provider("new"), nullptr);

        co_await manager.shutdown();
        completed = true;
        loop.stop();
    });

    loop.run();
    EXPECT_TRUE(completed);
}

TEST(LlmConfigManagerTest, ProviderServiceCandidatePublishesSharedRendezvousState) {
    fiber::event::EventLoop loop;
    FakeConfigService service;
    FakeNamingService naming;
    fiber::ai_server::LlmConfigManager manager(loop, service, naming);
    auto snapshots = manager.subscribe_snapshot();
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        EXPECT_TRUE(manager.start());
        service.push(fiber::ai_server::kBt1KeysDataId,
                     R"({"version":1,"data":{"keys":[{"kid":"main","secret":"secret"}]}})", "bt1");
        service.push(fiber::ai_server::kModelsDataId,
                     R"({"version":1,"data":[{"model-name":"chat","providers":["routed"]}]})", "models");
        co_await yield_updates();
        service.push("ploto.ai-llm.provider.routed",
                     R"({"version":1,"data":{
                         "provider":"routed",
                         "baseurl":"service://backend-a",
                         "api-tokens":[],
                         "protocol":[{
                             "type":"openai-chat-completions",
                             "path":"/v1/chat/completions",
                             "model":"model-a"
                         }]
                     }})",
                     "provider-a");
        co_await yield_updates();

        EXPECT_FALSE(manager.ready());
        EXPECT_EQ(manager.service_subscription_count(), 1u);
        EXPECT_EQ(manager.current_project(), nullptr);

        fiber::tests::ServiceInfoTestData backend_a;
        backend_a.name = "backend-a";
        backend_a.group_name = "DEFAULT_GROUP";
        backend_a.last_ref_time = 10;
        backend_a.checksum = "a1";
        backend_a.hosts.push_back(fiber::nacos::Instance{
                .ip = "10.0.0.1",
                .port = 8080,
                .weight = 2.0,
                .cluster_name = "primary",
        });
        naming.push("backend-a", std::move(backend_a));
        co_await yield_updates();

        EXPECT_TRUE(manager.ready());
        auto first = snapshots.current().value;
        EXPECT_NE(first, nullptr);
        const auto *first_provider = first->project->find_provider("routed");
        EXPECT_NE(first_provider, nullptr);
        EXPECT_NE(first_provider->service, nullptr);
        EXPECT_EQ(first_provider->service->configured_instance_count(), 1u);
        auto selected_a = first_provider->service->select(0);
        EXPECT_TRUE(selected_a);
        if (selected_a) {
            EXPECT_EQ(selected_a->ip_address().to_string(), "10.0.0.1");
            selected_a->report(fiber::ai_server::InstanceReportOutcome::Neutral);
        }

        service.push("ploto.ai-llm.provider.routed",
                     R"({"version":2,"data":{
                         "provider":"routed",
                         "baseurl":"service://backend-b",
                         "api-tokens":[],
                         "protocol":[{
                             "type":"openai-chat-completions",
                             "path":"/v1/chat/completions",
                             "model":"model-b"
                         }]
                     }})",
                     "provider-b");
        co_await yield_updates();

        EXPECT_EQ(manager.service_subscription_count(), 2u);
        EXPECT_EQ(snapshots.current().value, first);
        EXPECT_EQ(manager.current_project()->find_provider("routed")->config->metadata.version, 1);

        fiber::tests::ServiceInfoTestData backend_b;
        backend_b.name = "backend-b";
        backend_b.group_name = "DEFAULT_GROUP";
        backend_b.last_ref_time = 20;
        backend_b.checksum = "b1";
        backend_b.hosts.push_back(fiber::nacos::Instance{
                .ip = "10.0.0.2",
                .port = 9090,
                .weight = 1.0,
                .cluster_name = "primary",
        });
        backend_b.hosts.push_back(fiber::nacos::Instance{
                .ip = "10.0.0.3",
                .port = 9091,
                .weight = 1.0,
                .healthy = false,
                .cluster_name = "primary",
        });
        naming.push("backend-b", std::move(backend_b));
        co_await yield_updates();

        auto second = snapshots.current().value;
        EXPECT_NE(second, nullptr);
        EXPECT_NE(second, first);
        const auto *second_provider = second->project->find_provider("routed");
        EXPECT_NE(second_provider, nullptr);
        EXPECT_NE(second_provider->service, nullptr);
        EXPECT_EQ(second_provider->config->metadata.version, 2);
        EXPECT_EQ(second_provider->service->configured_instance_count(), 1u);
        auto selected_b = second_provider->service->select(0);
        EXPECT_TRUE(selected_b);
        if (selected_b) {
            EXPECT_EQ(selected_b->ip_address().to_string(), "10.0.0.2");
            selected_b->report(fiber::ai_server::InstanceReportOutcome::Neutral);
        }
        EXPECT_EQ(manager.service_subscription_count(), 2u);
        EXPECT_EQ(first_provider->config->metadata.version, 1);
        EXPECT_EQ(first_provider->service->configured_instance_count(), 1u);

        fiber::tests::ServiceInfoTestData empty_backend_b;
        empty_backend_b.name = "backend-b";
        empty_backend_b.group_name = "DEFAULT_GROUP";
        empty_backend_b.last_ref_time = 30;
        empty_backend_b.checksum = "b2";
        naming.push("backend-b", std::move(empty_backend_b));
        co_await yield_updates();

        auto empty = snapshots.current().value;
        EXPECT_NE(empty, nullptr);
        EXPECT_EQ(empty, second);
        EXPECT_EQ(empty->project->find_provider("routed")->service->configured_instance_count(), 0u);
        EXPECT_EQ(second_provider->service->configured_instance_count(), 0u);
        EXPECT_EQ(first_provider->service->configured_instance_count(), 1u);

        first.reset();
        second.reset();
        empty.reset();
        co_await yield_updates();
        EXPECT_EQ(manager.service_subscription_count(), 1u);
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
    FakeNamingService naming;
    fiber::ai_server::LlmConfigManager manager(accept_loop, service, naming);
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
