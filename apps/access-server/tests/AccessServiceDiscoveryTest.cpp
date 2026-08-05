#include <gtest/gtest.h>

#include <array>
#include <expected>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include <async/Spawn.h>
#include <async/Yield.h>
#include <event/EventLoop.h>
#include <fiber/nacos/NamingService.h>
#include <fiber/nacos/Subscription.h>

#include "../../../tests/NacosSnapshotTestBuilder.h"
#include "../../../tests/NacosSubscriptionStub.h"
#include "runtime/AccessServiceDiscovery.h"
#include "runtime/RouteConfigStore.h"

namespace {

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
        auto [iterator, inserted] = entries_.try_emplace(key, std::make_unique<Entry>());
        (void) inserted;
        return iterator->second->subscriptions.subscribe(on_notify, ctx);
    }

    std::expected<fiber::nacos::InstanceRegistration, fiber::nacos::NamingServiceError>
    registry(std::string_view, std::string_view, fiber::nacos::Instance) override {
        return std::unexpected(fiber::nacos::NamingServiceError{
                .code = fiber::nacos::NamingServiceErrorCode::Server,
                .message = "not implemented by fake naming service",
        });
    }

    void push(std::string_view service_name, fiber::tests::ServiceInfoTestData info) {
        const auto iterator = entries_.find(make_key(service_name, fiber::access_server::kDefaultNacosGroup));
        ASSERT_NE(iterator, entries_.end());
        iterator->second->subscriptions.publish(Result{
                .kind = fiber::nacos::ResultKind::Success,
                .data = fiber::tests::make_service_info(std::move(info)),
        });
    }

    [[nodiscard]] std::size_t subscriptions(std::string_view service_name) const {
        const auto iterator = entries_.find(make_key(service_name, fiber::access_server::kDefaultNacosGroup));
        return iterator == entries_.end() ? 0 : iterator->second->subscriptions.subscription_count();
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

fiber::access_server::ProjectConfig project_config(std::string service) {
    fiber::access_server::RouteConfig route;
    route.path = "/";
    route.service = service;
    fiber::access_server::RouteConfig alternate;
    alternate.path = "/alternate";
    alternate.service = std::move(service);

    fiber::access_server::ProjectConfig config;
    config.version = 1;
    config.hosts = std::vector<fiber::access_server::HostConfigEntry>{
            fiber::access_server::HostConfigEntry{
                    .pattern = "api.example.com",
                    .strategy = fiber::access_server::HostStrategyConfig{},
            },
    };
    config.routes =
            std::vector<std::optional<fiber::access_server::RouteConfig>>{std::move(route), std::move(alternate)};
    return config;
}

std::string selected_host(const fiber::access_server::ProxyUpstreamEndpoint &endpoint) {
    if (endpoint.connection_key == nullptr) {
        return {};
    }
    if (endpoint.connection_key->is_ip()) {
        return endpoint.connection_key->ip_address().to_string();
    }
    return std::string(endpoint.connection_key->host_name());
}

TEST(AccessServiceDiscoveryTest, WaitsBeforePublishAndPinsDiscoveryGeneration) {
    fiber::event::EventLoop loop;
    FakeNamingService naming;
    fiber::access_server::AccessServiceDiscoveryOptions options{
            .group = "DEFAULT_GROUP",
            .default_cluster = "default",
            .zone = "sh",
    };
    fiber::access_server::AccessServiceDiscovery discovery(
            loop, naming, fiber::access_server::AccessServiceOps{.zone = options.zone});
    fiber::access_server::RouteConfigStore store({}, discovery, options);
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto prepared = store.prepare("demo", project_config("orders"));
        EXPECT_TRUE(prepared);
        if (!prepared) {
            co_await discovery.shutdown();
            loop.stop();
            co_return;
        }
        EXPECT_TRUE(store.pin()->projects().empty());
        EXPECT_EQ(discovery.size(), 1u);
        EXPECT_EQ(naming.subscriptions("orders"), 1u);

        fiber::tests::ServiceInfoTestData info;
        info.name = "orders";
        info.group_name = "DEFAULT_GROUP";
        info.hosts = {
                fiber::nacos::Instance{
                        .ip = "10.0.0.1",
                        .port = 8080,
                        .weight = 2.0,
                        .cluster_name = "sh-default",
                },
                fiber::nacos::Instance{
                        .ip = "10.0.0.2",
                        .port = 8080,
                        .weight = 10.0,
                        .cluster_name = "bj-default",
                },
                fiber::nacos::Instance{
                        .ip = "10.0.0.5",
                        .port = 8080,
                        .cluster_name = "sh-default",
                },
                fiber::nacos::Instance{
                        .ip = "10.0.0.3",
                        .port = 8081,
                        .cluster_name = "sh-gray",
                },
                fiber::nacos::Instance{
                        .ip = "10.0.0.4",
                        .port = 8082,
                        .healthy = false,
                        .cluster_name = "sh-default",
                },
        };
        naming.push("orders", std::move(info));
        auto ready = co_await prepared->project_snapshot->wait_ready();
        EXPECT_TRUE(ready);
        if (!ready) {
            prepared = std::unexpected(fiber::access_server::AccessConfigError{});
            co_await discovery.shutdown();
            loop.stop();
            co_return;
        }
        auto committed = store.commit(std::move(*prepared));
        EXPECT_TRUE(committed);
        if (!committed) {
            store.clear();
            co_await discovery.shutdown();
            loop.stop();
            co_return;
        }

        std::shared_ptr<const fiber::access_server::AccessRouteSnapshot> route_snapshot = store.pin();
        const auto &compiled_route = route_snapshot->projects().front()->routes().front();
        EXPECT_TRUE(compiled_route.proxy);
        fiber::access_server::ProxyAddressSelector *address_selector =
                compiled_route.proxy ? compiled_route.proxy->address_selector.get() : nullptr;
        EXPECT_NE(address_selector, nullptr);
        if (address_selector == nullptr) {
            store.clear();
            co_await discovery.shutdown();
            loop.stop();
            co_return;
        }
        auto stable = address_selector->select_address(std::nullopt, {});
        EXPECT_TRUE(stable);
        if (stable) {
            EXPECT_EQ(selected_host(*stable), "10.0.0.1");
            EXPECT_EQ(stable->host_header, "10.0.0.1:8080");
            EXPECT_NE(stable->connection_key, nullptr);
            if (stable->connection_key) {
                EXPECT_TRUE(stable->connection_key->is_ip());
            }
            stable->report(false);
            const std::uint64_t excluded = stable->selection_token;
            auto retry = address_selector->select_address(std::nullopt, std::span(&excluded, 1));
            EXPECT_TRUE(retry);
            if (retry) {
                EXPECT_EQ(selected_host(*retry), "10.0.0.5");
                const std::array excluded_local{excluded, retry->selection_token};
                auto remote = address_selector->select_address(std::nullopt, excluded_local);
                EXPECT_TRUE(remote);
                if (remote) {
                    EXPECT_EQ(selected_host(*remote), "10.0.0.2");
                    EXPECT_NE(remote->selection_token, excluded);
                    EXPECT_NE(remote->selection_token, retry->selection_token);
                }
            }
        }

        auto gray = address_selector->select_address("gray", {});
        EXPECT_TRUE(gray);
        if (gray) {
            EXPECT_EQ(selected_host(*gray), "10.0.0.3");
        }

        fiber::tests::ServiceInfoTestData changed;
        changed.name = "orders";
        changed.hosts = {
                fiber::nacos::Instance{
                        .ip = "orders.internal",
                        .port = 9090,
                        .cluster_name = "default",
                },
        };
        naming.push("orders", std::move(changed));
        co_await yield_updates();

        if (stable) {
            EXPECT_EQ(selected_host(*stable), "10.0.0.1");
        }
        auto hostname = address_selector->select_address(std::nullopt, {});
        EXPECT_TRUE(hostname);
        if (hostname) {
            EXPECT_EQ(selected_host(*hostname), "orders.internal");
            EXPECT_EQ(hostname->host_header, "orders.internal:9090");
            EXPECT_NE(hostname->connection_key, nullptr);
            if (hostname->connection_key) {
                EXPECT_TRUE(hostname->connection_key->is_name());
            }
            if (stable) {
                EXPECT_NE(hostname->selection_token, stable->selection_token);
            }
        }
        EXPECT_TRUE(store.remove_project("demo"));
        EXPECT_EQ(discovery.size(), 1u);
        EXPECT_TRUE(address_selector->select_address(std::nullopt, {}));

        committed = std::unexpected(fiber::access_server::AccessConfigError{});
        route_snapshot.reset();
        co_await yield_updates();
        EXPECT_TRUE(discovery.empty());
        store.clear();
        co_await discovery.shutdown();
        completed = true;
        loop.stop();
    });

    loop.run();
    EXPECT_TRUE(completed);
}

TEST(AccessServiceDiscoveryTest, ClosedBeforeInitialUpdateRejectsCandidate) {
    fiber::event::EventLoop loop;
    FakeNamingService naming;
    fiber::access_server::AccessServiceDiscoveryOptions options;
    fiber::access_server::AccessServiceDiscovery discovery(loop, naming, fiber::access_server::AccessServiceOps{});
    fiber::access_server::RouteConfigStore store({}, discovery, options);
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto prepared = store.prepare("demo", project_config("orders"));
        EXPECT_TRUE(prepared);
        EXPECT_TRUE(store.pin()->projects().empty());
        if (prepared) {
            fiber::async::spawn([&]() -> fiber::async::DetachedTask {
                co_await fiber::async::yield();
                co_await discovery.shutdown();
            });
            auto ready = co_await prepared->project_snapshot->wait_ready();
            EXPECT_FALSE(ready);
            EXPECT_TRUE(store.pin()->projects().empty());
            prepared = std::unexpected(fiber::access_server::AccessConfigError{});
        }
        co_await discovery.shutdown();
        completed = true;
        loop.stop();
    });

    loop.run();
    EXPECT_TRUE(completed);
}

} // namespace
