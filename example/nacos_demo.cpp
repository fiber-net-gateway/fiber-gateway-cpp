// nacos_demo.cpp
//
// A single-file tour of the nacos module (`apps/nacos`). It connects to a live Nacos
// server (HTTP login on 8848 for auth, gRPC data plane on 9848 for config & naming) and
// exercises every core surface:
//
//   * NacosClient: create / start / shutdown, plus `subscribe_auth()` to observe the auth
//     access state (NotConfigured / Present / Stopped). The client owns auth only.
//   * ConfigService: a standalone service bound to the client via `ConfigService::create`
//     + `start()` / `shutdown()`; ops are get_config, publish (with CAS md5),
//     remove_config, and subscribe (live config-change pushes).
//   * NamingService: likewise standalone; ops are get, subscribe (instance-list pushes),
//     and registry + subscribe_status / update / close (register -> Registered -> update
//     -> deregister -> Closed).
//
// The public data-plane ops return a not-connected error until each service's gRPC
// connection is ready, and readiness is not exposed on the public interface. So the demo
// waits for auth, then probes each data plane through the public API before driving it.
// Every await is bounded, so the demo never hangs when the server is absent; each step
// logs its outcome and the flow always reaches a clean shutdown.
//
// Usage:
//   ./nacos_demo [--server 127.0.0.1] [--http-port 8848] [--grpc-port 9848] \
//                [--username nacos] [--password nacos] [--namespace ""] \
//                [--data-id fiber-demo-config] [--group DEFAULT_GROUP] \
//                [--service fiber-demo-service] [--ip 127.0.0.1] [--port 18080]
//
// Point it at a Nacos 2.x server whose HTTP and gRPC ports are reachable. With no
// arguments it assumes a local server with the default nacos/nacos credentials.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "async/Sleep.h"
#include "async/Spawn.h"
#include "async/Timeout.h"
#include "common/IoError.h"
#include "event/EventLoop.h"
#include "net/IpAddress.h"

#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/NacosClient.h>
#include <fiber/nacos/NamingService.h>

namespace {

using namespace std::chrono_literals;

namespace nacos = fiber::nacos;

// --------------------------------------------------------------------------- //
// CLI
// --------------------------------------------------------------------------- //

struct CliOptions {
    fiber::net::IpAddress server_ip = fiber::net::IpAddress::loopback_v4();
    std::uint16_t http_port = 8848;
    std::uint16_t grpc_port = 9848;
    std::string username = "nacos";
    std::string password = "nacos";
    std::string namespace_id; // empty -> default namespace
    std::string data_id = "fiber-demo-config";
    std::string group = "DEFAULT_GROUP";
    std::string service = "fiber-demo-service";
    std::string instance_ip = "127.0.0.1";
    std::uint16_t instance_port = 18080;
};

bool parse_u16(std::string_view text, std::uint16_t &out) {
    if (text.empty()) {
        return false;
    }
    char *end = nullptr;
    const unsigned long value = std::strtoul(std::string(text).c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || value > 65535) {
        return false;
    }
    out = static_cast<std::uint16_t>(value);
    return true;
}

void print_usage() {
    std::cout << "Usage: nacos_demo [options]\n"
              << "  --server <ip>          nacos server ip (default 127.0.0.1)\n"
              << "  --http-port <port>     http login port (default 8848)\n"
              << "  --grpc-port <port>     gRPC data-plane port (default 9848)\n"
              << "  --username <user>      login user (default nacos; empty = no-auth)\n"
              << "  --password <pass>      login password (default nacos; empty = no-auth)\n"
              << "  --namespace <id>       namespace/tenant (default empty = public)\n"
              << "  --data-id <id>         demo config data-id (default fiber-demo-config)\n"
              << "  --group <group>        demo group (default DEFAULT_GROUP)\n"
              << "  --service <name>       demo service name (default fiber-demo-service)\n"
              << "  --ip <ip>              demo instance ip (default 127.0.0.1)\n"
              << "  --port <port>          demo instance port (default 18080)\n"
              << "  -h, --help             show this help\n";
}

std::optional<CliOptions> parse_args(int argc, char **argv) {
    CliOptions cli;
    auto take = [&](int &i) -> const char * {
        if (i + 1 >= argc) {
            return nullptr;
        }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const char *val = nullptr;
        if (arg == "-h" || arg == "--help") {
            print_usage();
            return std::nullopt;
        } else if (arg == "--server") {
            val = take(i);
            if (!val || !fiber::net::IpAddress::parse(val, cli.server_ip)) {
                std::cerr << "invalid --server: " << (val ? val : "(missing)") << "\n";
                return std::nullopt;
            }
        } else if (arg == "--http-port") {
            val = take(i);
            if (!val || !parse_u16(val, cli.http_port)) {
                std::cerr << "invalid --http-port\n";
                return std::nullopt;
            }
        } else if (arg == "--grpc-port") {
            val = take(i);
            if (!val || !parse_u16(val, cli.grpc_port)) {
                std::cerr << "invalid --grpc-port\n";
                return std::nullopt;
            }
        } else if (arg == "--username") {
            val = take(i);
            if (!val) {
                return std::nullopt;
            }
            cli.username = val;
        } else if (arg == "--password") {
            val = take(i);
            if (!val) {
                return std::nullopt;
            }
            cli.password = val;
        } else if (arg == "--namespace") {
            val = take(i);
            if (!val) {
                return std::nullopt;
            }
            cli.namespace_id = val;
        } else if (arg == "--data-id") {
            val = take(i);
            if (!val) {
                return std::nullopt;
            }
            cli.data_id = val;
        } else if (arg == "--group") {
            val = take(i);
            if (!val) {
                return std::nullopt;
            }
            cli.group = val;
        } else if (arg == "--service") {
            val = take(i);
            if (!val) {
                return std::nullopt;
            }
            cli.service = val;
        } else if (arg == "--ip") {
            val = take(i);
            if (!val) {
                return std::nullopt;
            }
            cli.instance_ip = val;
        } else if (arg == "--port") {
            val = take(i);
            if (!val || !parse_u16(val, cli.instance_port)) {
                std::cerr << "invalid --port\n";
                return std::nullopt;
            }
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            print_usage();
            return std::nullopt;
        }
    }
    return cli;
}

// --------------------------------------------------------------------------- //
// Pretty-printers
// --------------------------------------------------------------------------- //

const char *auth_kind_name(nacos::NacosAuthAccessKind k) {
    switch (k) {
        case nacos::NacosAuthAccessKind::NotConfigured:
            return "NotConfigured";
        case nacos::NacosAuthAccessKind::InitialFailed:
            return "InitialFailed";
        case nacos::NacosAuthAccessKind::Present:
            return "Present";
        case nacos::NacosAuthAccessKind::Stopped:
            return "Stopped";
    }
    return "?";
}

const char *config_type_name(nacos::ConfigType t) {
    switch (t) {
        case nacos::ConfigType::Json:
            return "json";
        case nacos::ConfigType::Text:
            return "text";
        case nacos::ConfigType::Yaml:
            return "yaml";
        case nacos::ConfigType::Properties:
            return "properties";
        case nacos::ConfigType::Xml:
            return "xml";
        case nacos::ConfigType::Html:
            return "html";
    }
    return "?";
}

const char *config_err_name(nacos::ConfigServiceErrorCode c) {
    switch (c) {
        case nacos::ConfigServiceErrorCode::InvalidArgument:
            return "InvalidArgument";
        case nacos::ConfigServiceErrorCode::Shutdown:
            return "Shutdown";
        case nacos::ConfigServiceErrorCode::AuthenticationUnavailable:
            return "AuthenticationUnavailable";
        case nacos::ConfigServiceErrorCode::Transport:
            return "Transport";
        case nacos::ConfigServiceErrorCode::GrpcStatus:
            return "GrpcStatus";
        case nacos::ConfigServiceErrorCode::Protocol:
            return "Protocol";
        case nacos::ConfigServiceErrorCode::Server:
            return "Server";
        case nacos::ConfigServiceErrorCode::ContentTooLarge:
            return "ContentTooLarge";
    }
    return "Unknown";
}

const char *naming_err_name(nacos::NamingServiceErrorCode c) {
    switch (c) {
        case nacos::NamingServiceErrorCode::InvalidArgument:
            return "InvalidArgument";
        case nacos::NamingServiceErrorCode::Shutdown:
            return "Shutdown";
        case nacos::NamingServiceErrorCode::AuthenticationUnavailable:
            return "AuthenticationUnavailable";
        case nacos::NamingServiceErrorCode::Transport:
            return "Transport";
        case nacos::NamingServiceErrorCode::GrpcStatus:
            return "GrpcStatus";
        case nacos::NamingServiceErrorCode::Protocol:
            return "Protocol";
        case nacos::NamingServiceErrorCode::Server:
            return "Server";
        case nacos::NamingServiceErrorCode::ResponseTooLarge:
            return "ResponseTooLarge";
    }
    return "Unknown";
}

const char *reg_state_name(nacos::RegistrationState s) {
    switch (s) {
        case nacos::RegistrationState::Pending:
            return "Pending";
        case nacos::RegistrationState::Registered:
            return "Registered";
        case nacos::RegistrationState::Failed:
            return "Failed";
        case nacos::RegistrationState::Closed:
            return "Closed";
    }
    return "?";
}

void print_config_data(const nacos::ConfigData &c) {
    std::cout << "    state=" << (c.state == nacos::ConfigState::Present ? "Present" : "NotFound") << " md5=" << c.md5
              << " content=\"" << c.content << "\"\n";
}

void print_service_info(const nacos::ServiceInfo &info) {
    std::cout << "    service=" << info.name << " group=" << info.group_name << " clusters=" << info.clusters
              << " hosts=" << info.hosts.size() << "\n";
    for (const nacos::ServiceInstance &h: info.hosts) {
        std::cout << "      - " << h.ip << ":" << h.port << " weight=" << h.weight
                  << " healthy=" << (h.healthy ? "Y" : "N") << " enabled=" << (h.enabled ? "Y" : "N")
                  << " ephemeral=" << (h.ephemeral ? "Y" : "N");
        if (!h.cluster_name.empty()) {
            std::cout << " cluster=" << h.cluster_name;
        }
        if (!h.metadata.empty()) {
            std::cout << " metadata={";
            for (std::size_t i = 0; i < h.metadata.size(); ++i) {
                if (i != 0) {
                    std::cout << ",";
                }
                std::cout << h.metadata[i].key << "=" << h.metadata[i].value;
            }
            std::cout << "}";
        }
        std::cout << "\n";
    }
}

nacos::Instance make_instance(const CliOptions &cli, std::uint16_t port) {
    nacos::Instance instance;
    instance.ip = cli.instance_ip;
    instance.port = port;
    instance.weight = 1.0;
    instance.healthy = true;
    instance.enabled = true;
    instance.ephemeral = true;
    instance.cluster_name = "DEFAULT";
    instance.metadata = {{"version", "1.0"}, {"region", "demo"}};
    return instance;
}

// --------------------------------------------------------------------------- //
// Generic "wait for a Watch subscriber to publish a matching value" helper.
//
// This is used for InstanceRegistration status. `pred` is applied to the most
// recent published value; current()
// is checked first so a value published before we started waiting is still observed.
// Returns true on a match, false on timeout.
// --------------------------------------------------------------------------- //

template<typename Subscriber, typename Pred>
fiber::async::Task<bool> wait_for(Subscriber &sub, Pred pred, std::chrono::milliseconds per_try, int tries) {
    auto cur = sub.current();
    std::uint64_t version = cur.version;
    if (cur.value && pred(*cur.value)) {
        co_return true;
    }
    for (int i = 0; i < tries; ++i) {
        auto snap = co_await fiber::async::timeout_for([&sub, version]() { return sub.next(version); }, per_try);
        if (!snap) {
            continue; // this iteration timed out
        }
        version = snap->version;
        if (snap->value && pred(*snap->value)) {
            co_return true;
        }
    }
    co_return false;
}

template<typename Pred>
fiber::async::Task<bool> wait_until(Pred pred, std::chrono::milliseconds per_try, int tries) {
    if (pred()) {
        co_return true;
    }
    for (int i = 0; i < tries; ++i) {
        co_await fiber::async::sleep(per_try);
        if (pred()) {
            co_return true;
        }
    }
    co_return false;
}

struct ConfigNotifyState {
    bool version_1 = false;
    bool version_2 = false;
    bool conflicting_version = false;
    bool not_found = false;
    bool closed = false;
};

void config_notify(void *context, const nacos::SubscriptionResult<nacos::ConfigData> &result) noexcept {
    auto &state = *static_cast<ConfigNotifyState *>(context);
    if (result.kind == nacos::ResultKind::Closed) {
        state.closed = true;
        return;
    }
    if (!result.data) {
        return;
    }
    if (result.data->state == nacos::ConfigState::NotFound) {
        state.not_found = true;
    } else if (result.data->content == "version-1") {
        state.version_1 = true;
    } else if (result.data->content == "version-2") {
        state.version_2 = true;
    } else if (result.data->content == "should-not-apply") {
        state.conflicting_version = true;
    }
}

struct NamingNotifyState {
    std::string_view ip;
    std::uint16_t initial_port = 0;
    std::uint16_t updated_port = 0;
    bool initial_seen = false;
    bool updated_seen = false;
    bool closed = false;
};

void naming_notify(void *context, const nacos::SubscriptionResult<nacos::ServiceInfo> &result) noexcept {
    auto &state = *static_cast<NamingNotifyState *>(context);
    if (result.kind == nacos::ResultKind::Closed) {
        state.closed = true;
        return;
    }
    if (!result.data) {
        return;
    }
    std::cout << "[naming] current service info -> ";
    print_service_info(*result.data);
    for (const nacos::ServiceInstance &host: result.data->hosts) {
        if (host.ip != state.ip) {
            continue;
        }
        if (host.port == state.initial_port) {
            state.initial_seen = true;
        }
        if (state.updated_port != 0 && host.port == state.updated_port) {
            state.updated_seen = true;
        }
    }
}

// --------------------------------------------------------------------------- //
// Auth + readiness
// --------------------------------------------------------------------------- //

// Auth is "ready" once it reaches Present (creds configured, login ok) or NotConfigured
// (no creds, no-auth server). InitialFailed may recover later; we wait a bounded time.
fiber::async::Task<bool> wait_auth_ready(nacos::NacosClient::AuthSubscriber &auth) {
    const auto ready = [](const nacos::NacosAuthAccess *a) {
        return a != nullptr &&
               (a->kind == nacos::NacosAuthAccessKind::Present || a->kind == nacos::NacosAuthAccessKind::NotConfigured);
    };
    auto cur = auth.current();
    std::uint64_t version = cur.version;
    if (ready(cur.value.get())) {
        co_return true;
    }
    for (int i = 0; i < 15; ++i) { // up to ~15s
        auto snap = co_await fiber::async::timeout_for([&auth, version]() { return auth.next(version); }, 1s);
        if (!snap) {
            continue;
        }
        version = snap->version;
        if (ready(snap->value.get())) {
            co_return true;
        }
    }
    co_return false;
}

// Probe the config data plane: a get_config that returns any outcome other than a
// not-connected-class error means the server responded, i.e. the connection is up.
fiber::async::Task<bool> probe_config_ready(nacos::ConfigService &svc, const CliOptions &cli) {
    for (int i = 0; i < 30; ++i) { // up to ~15s @ 500ms
        auto r = co_await svc.get_config("__fiber_ready_probe__", cli.group);
        if (r) {
            co_return true; // found or NotFound -> server answered
        }
        const auto c = r.error().code;
        if (c != nacos::ConfigServiceErrorCode::Transport &&
            c != nacos::ConfigServiceErrorCode::AuthenticationUnavailable &&
            c != nacos::ConfigServiceErrorCode::GrpcStatus) {
            co_return true; // some other error -> the server still answered
        }
        co_await fiber::async::sleep(500ms);
    }
    co_return false;
}

fiber::async::Task<bool> probe_naming_ready(nacos::NamingService &svc, const CliOptions &cli) {
    for (int i = 0; i < 30; ++i) {
        auto r = co_await svc.get("__fiber_ready_probe__", cli.group);
        if (r) {
            co_return true;
        }
        const auto c = r.error().code;
        if (c != nacos::NamingServiceErrorCode::Transport &&
            c != nacos::NamingServiceErrorCode::AuthenticationUnavailable &&
            c != nacos::NamingServiceErrorCode::GrpcStatus) {
            co_return true;
        }
        co_await fiber::async::sleep(500ms);
    }
    co_return false;
}

// --------------------------------------------------------------------------- //
// Config demo
// --------------------------------------------------------------------------- //

fiber::async::Task<void> config_demo(nacos::ConfigService &svc, const CliOptions &cli) {
    std::cout << "\n[config] ---- ConfigService demo (data_id=" << cli.data_id << " group=" << cli.group << ") ----\n";

    // 1. publish (create)
    auto published = co_await svc.publish(cli.data_id, cli.group, "version-1", nacos::ConfigType::Text);
    if (!published) {
        std::cout << "[config] publish(v1) FAILED: " << config_err_name(published.error().code) << " ("
                  << published.error().message << ")\n";
        co_return;
    }
    std::cout << "[config] publish(v1, text) ok\n";

    // 2. get_config (bounded retry: a freshly created config is not readable via gRPC for a
    //    brief read-after-write window, so retry until it appears)
    std::shared_ptr<const nacos::ConfigData> fetched;
    std::string get_err;
    for (int i = 0; i < 8; ++i) { // up to ~4s @ 500ms
        auto got = co_await svc.get_config(cli.data_id, cli.group);
        if (got && (*got)->state == nacos::ConfigState::Present) {
            fetched = *got;
            break;
        }
        get_err = got ? std::string{"not found"} : std::string{config_err_name(got.error().code)};
        co_await fiber::async::sleep(500ms);
    }
    if (!fetched) {
        std::cout << "[config] get_config FAILED: " << get_err << "\n";
        co_return;
    }
    std::cout << "[config] get_config -> ";
    print_config_data(*fetched);
    const std::string md5_1(fetched->md5);

    // 3. subscribe + await initial push
    ConfigNotifyState notify_state;
    auto sub_result = svc.subscribe(cli.data_id, cli.group, &config_notify, &notify_state);
    if (!sub_result) {
        std::cout << "[config] subscribe FAILED: " << config_err_name(sub_result.error().code) << "\n";
        co_return;
    }
    auto subscription = std::move(*sub_result);
    bool ok = co_await wait_until([&notify_state]() { return notify_state.version_1; }, 2s, 4);
    std::cout << "[config] subscribe -> initial push version-1: " << (ok ? "ok" : "TIMEOUT") << "\n";

    // 4. publish (CAS update with the captured md5)
    auto updated = co_await svc.publish(cli.data_id, cli.group, "version-2", nacos::ConfigType::Text,
                                        std::optional<std::string>(md5_1));
    if (!updated) {
        std::cout << "[config] publish(v2, cas) FAILED: " << config_err_name(updated.error().code) << " ("
                  << updated.error().message << ")\n";
    } else {
        std::cout << "[config] publish(v2, cas=md5_1) ok\n";
        ok = co_await wait_until([&notify_state]() { return notify_state.version_2; }, 2s, 4);
        std::cout << "[config] subscribe -> change push version-2: " << (ok ? "ok" : "TIMEOUT") << "\n";
    }

    // 5. CAS-conflict probe (wrong md5): a Java nacos rejects this; some servers accept it.
    auto conflict = co_await svc.publish(cli.data_id, cli.group, "should-not-apply", nacos::ConfigType::Text,
                                         std::optional<std::string>("deliberately-wrong-md5"));
    if (!conflict) {
        std::cout << "[config] publish(wrong cas) rejected as expected: " << config_err_name(conflict.error().code)
                  << " (" << conflict.error().message << ")\n";
    } else {
        std::cout << "[config] publish(wrong cas) was ACCEPTED by the server (no CAS enforcement)\n";
        ok = co_await wait_until([&notify_state]() { return notify_state.conflicting_version; }, 1s, 2);
        (void) ok;
    }

    // 6. remove_config + await NotFound push
    auto removed = co_await svc.remove_config(cli.data_id, cli.group);
    if (!removed) {
        std::cout << "[config] remove_config FAILED: " << config_err_name(removed.error().code) << "\n";
    } else {
        std::cout << "[config] remove_config ok\n";
        ok = co_await wait_until([&notify_state]() { return notify_state.not_found; }, 2s, 4);
        std::cout << "[config] subscribe -> push NotFound: " << (ok ? "ok" : "TIMEOUT") << "\n";
    }

    // 7. close subscription (drops our listener reference)
    subscription.close();
    std::cout << "[config] subscription closed\n";
}

// --------------------------------------------------------------------------- //
// Naming demo
// --------------------------------------------------------------------------- //

fiber::async::Task<void> naming_demo(nacos::NamingService &svc, const CliOptions &cli) {
    std::cout << "\n[naming] ---- NamingService demo (service=" << cli.service << " group=" << cli.group << ") ----\n";

    // 1. register + await Registered
    auto reg_result = svc.registry(cli.service, cli.group, make_instance(cli, cli.instance_port));
    if (!reg_result) {
        std::cout << "[naming] registry FAILED: " << naming_err_name(reg_result.error().code) << "\n";
        co_return;
    }
    auto registration = std::move(*reg_result);
    auto status = registration.subscribe_status();
    bool ok = co_await wait_for(
            status, [](const nacos::RegistrationStatus &s) { return s.state == nacos::RegistrationState::Registered; },
            2s, 4);
    std::cout << "[naming] registry -> " << cli.instance_ip << ":" << cli.instance_port
              << " Registered: " << (ok ? "ok" : "TIMEOUT") << "\n";
    if (!ok) {
        registration.close();
        co_return;
    }

    // 2. get (bounded retry until our instance shows up)
    bool seen = false;
    for (int i = 0; i < 6; ++i) {
        auto info = co_await svc.get(cli.service, cli.group);
        if (info) {
            for (const nacos::ServiceInstance &h: (*info)->hosts) {
                if (h.ip == cli.instance_ip && h.port == cli.instance_port) {
                    seen = true;
                }
            }
            std::cout << "[naming] get -> ";
            print_service_info(**info);
            if (seen) {
                break;
            }
        }
        if (i == 5) {
            std::cout << "[naming] get: our instance not yet visible in query\n";
        }
        co_await fiber::async::sleep(500ms);
    }

    // 3. subscribe + await a push containing our instance
    NamingNotifyState notify_state{
            .ip = cli.instance_ip,
            .initial_port = cli.instance_port,
    };
    auto sub_result = svc.subscribe(cli.service, cli.group, &naming_notify, &notify_state);
    if (!sub_result) {
        std::cout << "[naming] subscribe FAILED: " << naming_err_name(sub_result.error().code) << "\n";
        registration.close();
        co_return;
    }
    auto subscription = std::move(*sub_result);
    ok = co_await wait_until([&notify_state]() { return notify_state.initial_seen; }, 2s, 4);
    std::cout << "[naming] subscribe -> push with our instance: " << (ok ? "ok" : "TIMEOUT") << "\n";

    // 4. update (change port) -> await Registered + push with the new port
    const std::uint16_t new_port = cli.instance_port + 1;
    notify_state.updated_port = new_port;
    auto update_res = registration.update(make_instance(cli, new_port));
    if (!update_res) {
        std::cout << "[naming] update FAILED: " << naming_err_name(update_res.error().code) << "\n";
    } else {
        std::cout << "[naming] update -> port " << new_port << " requested\n";
        ok = co_await wait_for(
                status,
                [](const nacos::RegistrationStatus &s) { return s.state == nacos::RegistrationState::Registered; }, 2s,
                4);
        const bool pushed = co_await wait_until([&notify_state]() { return notify_state.updated_seen; }, 2s, 4);
        std::cout << "[naming] update -> Registered: " << (ok ? "ok" : "TIMEOUT")
                  << " push with new port: " << (pushed ? "ok" : "TIMEOUT") << "\n";
    }

    // 5. close (deregister) -> await Closed
    registration.close();
    ok = co_await wait_for(
            status, [](const nacos::RegistrationStatus &s) { return s.state == nacos::RegistrationState::Closed; }, 2s,
            4);
    std::cout << "[naming] close -> deregistered (Closed): " << (ok ? "ok" : "TIMEOUT") << "\n";

    // 6. close subscription
    subscription.close();
    std::cout << "[naming] subscription closed\n";
}

// --------------------------------------------------------------------------- //
// Driver
// --------------------------------------------------------------------------- //

fiber::async::DetachedTask run_demo(fiber::event::EventLoop *loop, nacos::NacosClient *client, const CliOptions &cli,
                                    int *exit_code) {
    // Auth subscriber is acquired before start() so we never miss the initial publish.
    auto auth = client->subscribe_auth();
    auto started = client->start();
    if (!started) {
        std::cerr << "[client] start failed: " << fiber::common::io_err_name(started.error()) << "\n";
        *exit_code = 1;
        loop->stop();
        co_return;
    }

    // ConfigService and NamingService are standalone services bound to the client: each
    // shares the client's auth + EventLoop and owns its own gRPC data-plane connection.
    // create() hands them the client's dependencies; start() brings up the connection.
    std::unique_ptr<nacos::ConfigService> config_svc;
    if (auto r = nacos::ConfigService::create(*client)) {
        config_svc = std::move(*r);
    } else {
        std::cerr << "[config] ConfigService::create failed\n";
    }
    std::unique_ptr<nacos::NamingService> naming_svc;
    if (auto r = nacos::NamingService::create(*client)) {
        naming_svc = std::move(*r);
    } else {
        std::cerr << "[naming] NamingService::create failed\n";
    }
    if (config_svc) {
        if (auto s = config_svc->start(); !s) {
            std::cerr << "[config] start failed: " << fiber::common::io_err_name(s.error()) << "\n";
        }
    }
    if (naming_svc) {
        if (auto s = naming_svc->start(); !s) {
            std::cerr << "[naming] start failed: " << fiber::common::io_err_name(s.error()) << "\n";
        }
    }

    std::cout << "[client] started; waiting for auth...\n";
    const bool auth_ok = co_await wait_auth_ready(auth);
    auto auth_cur = auth.current();
    const auto kind = auth_cur.value ? auth_cur.value->kind : nacos::NacosAuthAccessKind::NotConfigured;
    std::cout << "[client] auth state: " << auth_kind_name(kind) << "\n";
    if (!auth_ok) {
        std::cerr << "[client] auth did not become ready; data-plane ops will fail. Proceeding to shutdown.\n";
    }

    bool config_ready = false;
    bool naming_ready = false;
    if (auth_ok && config_svc) {
        std::cout << "[client] probing config data plane...\n";
        config_ready = co_await probe_config_ready(*config_svc, cli);
        std::cout << "[client] config data plane ready: " << (config_ready ? "yes" : "no") << "\n";
    }
    if (auth_ok && naming_svc) {
        std::cout << "[client] probing naming data plane...\n";
        naming_ready = co_await probe_naming_ready(*naming_svc, cli);
        std::cout << "[client] naming data plane ready: " << (naming_ready ? "yes" : "no") << "\n";
    }

    if (config_ready) {
        co_await config_demo(*config_svc, cli);
    } else {
        std::cout << "\n[config] skipped (data plane not ready)\n";
    }

    if (naming_ready) {
        co_await naming_demo(*naming_svc, cli);
    } else {
        std::cout << "\n[naming] skipped (data plane not ready)\n";
    }

    // Shutdown order: stop the NacosClient (auth owner) FIRST, then the services. With a
    // live server, shutting the services down before the client deadlocks client->shutdown()
    // (it never returns from joining the auth task -- the auth-watch publish resumes a waiter
    // left behind by the torn-down services). Stopping the client first publishes auth Stopped
    // while the services are still intact, after which each service shuts down cleanly.
    std::cout << "\n[client] shutting down...\n";
    co_await client->shutdown();
    if (config_svc) {
        co_await config_svc->shutdown();
    }
    if (naming_svc) {
        co_await naming_svc->shutdown();
    }
    auto final_auth = auth.current();
    const auto final_kind = final_auth.value ? final_auth.value->kind : nacos::NacosAuthAccessKind::Stopped;
    std::cout << "[client] shutdown complete; final auth state: " << auth_kind_name(final_kind) << "\n";
    *exit_code = (config_ready && naming_ready) ? 0 : 1;
    loop->stop();
}

} // namespace

int main(int argc, char **argv) {
    auto cli = parse_args(argc, argv);
    if (!cli) {
        return 1;
    }

    nacos::NacosClientConfigParams params;
    params.server_ips.push_back(cli->server_ip);
    params.username = cli->username;
    params.password = cli->password;
    params.http_port = cli->http_port;
    params.grpc_port = cli->grpc_port;
    params.namespace_id = cli->namespace_id;
    params.tenant = cli->namespace_id;
    auto config_result = nacos::NacosClientConfig::create(std::move(params));
    if (!config_result) {
        std::cerr << "invalid nacos client config\n";
        return 1;
    }

    std::cout << "[client] nacos server " << cli->server_ip.to_string() << " http=" << cli->http_port
              << " grpc=" << cli->grpc_port << " namespace=\"" << cli->namespace_id << "\""
              << (cli->username.empty() ? " (no-auth)" : (" user=" + cli->username)) << "\n";

    fiber::event::EventLoop loop;
    auto client_result = nacos::NacosClient::create(loop, std::move(*config_result));
    if (!client_result) {
        std::cerr << "failed to create nacos client\n";
        return 1;
    }
    std::unique_ptr<nacos::NacosClient> client = std::move(*client_result);

    int exit_code = 0;
    fiber::async::spawn(loop, [&]() { return run_demo(&loop, client.get(), *cli, &exit_code); });
    loop.run();
    return exit_code;
}
