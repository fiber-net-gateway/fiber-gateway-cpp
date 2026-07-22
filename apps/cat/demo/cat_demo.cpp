// cat-demo: exercise the native CAT client against a real CAT server.
//
// Defaults are read from /data/appdatas/cat/client.xml (the standard Dianping
// CAT client config location). The demo creates a CatClient, records a mix of
// transaction/event trees (success + error), demonstrates explicit cross-service
// propagation, records Count/Duration metrics, lets the startup Reboot and
// periodic Heartbeat fire, waits for the sender to flush, prints client stats,
// and shuts down cleanly.
//
// Usage:
//   cat-demo [options]
//     --app <key>            CAT domain / app key (default: cat-demo)
//     --router <ip:port>     router HTTP endpoint (default: from client.xml)
//     --collector <ip:port>  bootstrap collector (default: from client.xml)
//     --encoder nt1|pt1      wire encoder (default: nt1)
//     --count <n>            trees per scenario (default: 5)
//     --wait <ms>            extra flush time after send (default: 3000)
//     --no-router            skip router discovery
//     --no-collector         skip bootstrap collector
//     --config <path>        client.xml path (default: /data/appdatas/cat/client.xml)
//     --hostname <name>      client hostname (default: system hostname)
//     --ip <ip>              client ip (default: detected)
//     -h, --help             show this help

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <ifaddrs.h>
#include <iostream>
#include <iterator>
#include <netdb.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include <async/Sleep.h>
#include <async/Spawn.h>
#include <event/EventLoop.h>
#include <fiber/cat/Cat.h>
#include <net/IpAddress.h>
#include <net/SocketAddress.h>

namespace {

using namespace std::chrono_literals;

struct ServerEndpoint {
    std::string ip;
    std::uint16_t port = 2280; // collector (raw TCP NT1/PT1)
    std::uint16_t http_port = 8080; // router HTTP
};

struct DemoSettings {
    std::string app_key = "cat-demo";
    int count = 5;
    long wait_ms = 3000;
    std::string encoder = "nt1";
    bool use_router = true;
    bool use_collector = true;
};

struct DemoConfig {
    DemoSettings settings;
    std::string hostname;
    std::string ip;
    std::string app_key;
    std::vector<fiber::cat::CatRouterEndpoint> routers;
    std::vector<fiber::net::SocketAddress> collectors;
    fiber::cat::CatEncoderType encoder = fiber::cat::CatEncoderType::Nt1;
};

std::optional<std::string> xml_attr(std::string_view tag, std::string_view name) {
    std::string needle = std::string(name) + "=\"";
    auto pos = tag.find(needle);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    pos += needle.size();
    auto end = tag.find('"', pos);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    return std::string(tag.substr(pos, end - pos));
}

// Locate "<server" but not "<servers": skip matches where the char after the
// tag name is itself a letter (i.e. a longer tag name).
std::size_t find_server_tag(std::string_view text, std::size_t from = 0) {
    while (true) {
        auto pos = text.find("<server", from);
        if (pos == std::string_view::npos) {
            return std::string_view::npos;
        }
        const std::size_t after = pos + 7; // length of "<server"
        if (after >= text.size() || !std::isalpha(static_cast<unsigned char>(text[after]))) {
            return pos;
        }
        from = after;
    }
}

std::optional<std::uint16_t> parse_u16(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    char *end = nullptr;
    const unsigned long value = std::strtoul(std::string(text).c_str(), &end, 10);
    if (!end || *end != '\0' || value > 65535) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

std::optional<ServerEndpoint> read_client_xml(const std::string &path) {
    std::ifstream file(path);
    if (!file) {
        return std::nullopt;
    }
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    auto pos = find_server_tag(content);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    auto close = content.find('>', pos);
    if (close == std::string::npos) {
        return std::nullopt;
    }
    std::string_view tag(content.data() + pos, close - pos);

    ServerEndpoint ep;
    if (auto v = xml_attr(tag, "ip")) {
        ep.ip = *v;
    }
    if (auto v = xml_attr(tag, "port")) {
        ep.port = parse_u16(*v).value_or(2280);
    }
    if (auto v = xml_attr(tag, "http-port")) {
        ep.http_port = parse_u16(*v).value_or(8080);
    }
    if (ep.ip.empty()) {
        return std::nullopt;
    }
    return ep;
}

std::string get_hostname() {
    std::array<char, 256> buf{};
    if (::gethostname(buf.data(), buf.size()) == 0) {
        return std::string(buf.data());
    }
    return "localhost";
}

std::string get_local_ip() {
    struct ifaddrs *head = nullptr;
    if (::getifaddrs(&head) != 0) {
        return "127.0.0.1";
    }
    std::string best = "127.0.0.1";
    for (struct ifaddrs *p = head; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        const auto *sin = reinterpret_cast<const struct sockaddr_in *>(p->ifa_addr);
        char buf[INET_ADDRSTRLEN] = {};
        if (::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)) == nullptr) {
            continue;
        }
        std::string_view ip{buf};
        if (ip.starts_with("127.") || ip.starts_with("169.254.")) {
            continue; // loopback / link-local
        }
        best = std::string{ip};
        break;
    }
    ::freeifaddrs(head);
    return best;
}

std::optional<std::pair<std::string, std::uint16_t>> split_host_port(std::string_view text) {
    auto colon = text.rfind(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    auto host = text.substr(0, colon);
    auto port = parse_u16(text.substr(colon + 1));
    if (!port || host.empty()) {
        return std::nullopt;
    }
    return std::make_pair(std::string{host}, *port);
}

void print_usage() {
    std::cerr << "usage: cat-demo [options]\n"
              << "  --app <key>            CAT domain (default: cat-demo)\n"
              << "  --router <ip:port>     router HTTP endpoint\n"
              << "  --collector <ip:port>  bootstrap collector\n"
              << "  --encoder nt1|pt1      wire encoder (default: nt1)\n"
              << "  --count <n>            trees per scenario (default: 5)\n"
              << "  --wait <ms>            extra flush ms (default: 3000)\n"
              << "  --no-router            skip router discovery\n"
              << "  --no-collector         skip bootstrap collector\n"
              << "  --config <path>        client.xml (default: /data/appdatas/cat/client.xml)\n"
              << "  --hostname <name>      client hostname\n"
              << "  --ip <ip>              client ip\n"
              << "  -h, --help             show this help\n";
}

std::optional<DemoConfig> parse_args(int argc, char **argv) {
    DemoConfig cfg;
    std::string config_path = "/data/appdatas/cat/client.xml";
    std::optional<ServerEndpoint> server = read_client_xml(config_path);

    auto need_arg = [&](int &i, const char *flag) -> std::optional<std::string> {
        if (i + 1 >= argc) {
            std::cerr << "error: " << flag << " requires an argument\n";
            return std::nullopt;
        }
        return std::string(argv[++i]);
    };

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage();
            return std::nullopt;
        } else if (arg == "--app") {
            auto v = need_arg(i, "--app");
            if (!v)
                return std::nullopt;
            cfg.settings.app_key = *v;
        } else if (arg == "--config") {
            auto v = need_arg(i, "--config");
            if (!v)
                return std::nullopt;
            config_path = *v;
            server = read_client_xml(config_path);
        } else if (arg == "--router") {
            auto v = need_arg(i, "--router");
            if (!v)
                return std::nullopt;
            auto hp = split_host_port(*v);
            if (!hp) {
                std::cerr << "error: --router expects <ip:port>\n";
                return std::nullopt;
            }
            if (!server) {
                server = ServerEndpoint{};
            }
            server->ip = hp->first;
            server->http_port = hp->second;
        } else if (arg == "--collector") {
            auto v = need_arg(i, "--collector");
            if (!v)
                return std::nullopt;
            auto hp = split_host_port(*v);
            if (!hp) {
                std::cerr << "error: --collector expects <ip:port>\n";
                return std::nullopt;
            }
            fiber::net::IpAddress ip;
            if (!fiber::net::IpAddress::parse(hp->first, ip)) {
                std::cerr << "error: --collector ip '" << hp->first << "' is not a literal IP\n";
                return std::nullopt;
            }
            if (!server) {
                server = ServerEndpoint{};
            }
            server->ip = hp->first;
            server->port = hp->second;
        } else if (arg == "--encoder") {
            auto v = need_arg(i, "--encoder");
            if (!v)
                return std::nullopt;
            if (*v != "nt1" && *v != "pt1") {
                std::cerr << "error: --encoder must be nt1 or pt1\n";
                return std::nullopt;
            }
            cfg.settings.encoder = *v;
        } else if (arg == "--count") {
            auto v = need_arg(i, "--count");
            if (!v)
                return std::nullopt;
            char *end = nullptr;
            long n = std::strtol(v->c_str(), &end, 10);
            if (!end || *end != '\0' || n < 0) {
                std::cerr << "error: --count must be a non-negative integer\n";
                return std::nullopt;
            }
            cfg.settings.count = static_cast<int>(n);
        } else if (arg == "--wait") {
            auto v = need_arg(i, "--wait");
            if (!v)
                return std::nullopt;
            char *end = nullptr;
            long n = std::strtol(v->c_str(), &end, 10);
            if (!end || *end != '\0' || n < 0) {
                std::cerr << "error: --wait must be a non-negative integer\n";
                return std::nullopt;
            }
            cfg.settings.wait_ms = n;
        } else if (arg == "--no-router") {
            cfg.settings.use_router = false;
        } else if (arg == "--no-collector") {
            cfg.settings.use_collector = false;
        } else if (arg == "--hostname") {
            auto v = need_arg(i, "--hostname");
            if (!v)
                return std::nullopt;
            cfg.hostname = *v;
        } else if (arg == "--ip") {
            auto v = need_arg(i, "--ip");
            if (!v)
                return std::nullopt;
            cfg.ip = *v;
        } else {
            std::cerr << "error: unknown option '" << arg << "'\n";
            print_usage();
            return std::nullopt;
        }
    }

    if (!server) {
        std::cerr << "warning: could not read '" << config_path
                  << "'; falling back to 10.164.200.155 (router :8080, collector :2280)\n";
        server = ServerEndpoint{.ip = "10.164.200.155", .port = 2280, .http_port = 8080};
    }

    if (cfg.hostname.empty()) {
        cfg.hostname = get_hostname();
    }
    if (cfg.ip.empty()) {
        cfg.ip = get_local_ip();
    }
    cfg.app_key = cfg.settings.app_key;
    cfg.encoder = (cfg.settings.encoder == "pt1") ? fiber::cat::CatEncoderType::Pt1 : fiber::cat::CatEncoderType::Nt1;

    if (cfg.settings.use_router) {
        cfg.routers.push_back({.host = server->ip, .port = server->http_port});
    }
    if (cfg.settings.use_collector) {
        fiber::net::IpAddress ip;
        if (!fiber::net::IpAddress::parse(server->ip, ip)) {
            std::cerr << "error: collector ip '" << server->ip << "' is not a literal IP\n";
            return std::nullopt;
        }
        cfg.collectors.emplace_back(ip, server->port);
    }
    if (cfg.routers.empty() && cfg.collectors.empty()) {
        std::cerr << "error: no router and no collector configured\n";
        return std::nullopt;
    }
    return cfg;
}

void print_stats(const fiber::cat::CatClientStats &s) {
    std::cout << "  submitted=" << s.submitted_messages << " sent=" << s.sent_messages << " sent_bytes=" << s.sent_bytes
              << "\n";
    std::cout << "  router: ok=" << s.router_successes << " fail=" << s.router_failures << " blocks=" << s.router_blocks
              << " sample_changes=" << s.router_sample_changes << " collector_set_changes=" << s.collector_set_changes
              << "\n";
    std::cout << "  connect: ok=" << s.connect_successes << " fail=" << s.connect_failures
              << " would_block=" << s.write_would_block << " write_failures=" << s.write_failures << "\n";
    std::cout << "  trees: sampled=" << s.sampled_trees << " forced_problem=" << s.forced_problem_trees
              << " aggregated=" << s.aggregated_trees << " truncated=" << s.truncated_trees << "\n";
    std::cout << "  metric: observations=" << s.metric_observations << " submitted=" << s.metric_submitted
              << " dropped=" << s.metric_dropped << "\n";
    std::cout << "  heartbeat: submitted=" << s.heartbeat_submitted << " sent=" << s.heartbeat_sent
              << " skipped=" << s.heartbeat_skipped << " dropped=" << s.heartbeat_dropped << "\n";
    std::cout << "  drops: queue_full=" << s.dropped_queue_full << " unavailable=" << s.dropped_unavailable
              << " sampled=" << s.dropped_sampled << " partial_frame=" << s.dropped_partial_frame
              << " encode_failures=" << s.encode_failures << "\n";
}

// A successful transaction tree: root URL with a child SQL transaction and a Cache event.
std::string record_success_tree(fiber::cat::CatClient &client, int idx) {
    auto tr = fiber::cat::MessageTrace::create(client);
    if (!tr) {
        return {};
    }
    fiber::cat::MessageTrace trace = std::move(*tr);
    std::string id;
    if (auto pc = trace.propagation_context()) {
        id = std::string(pc->message_id());
    }
    auto rr = trace.create_transaction("URL", "/api/orders");
    if (!rr) {
        return id;
    }
    fiber::cat::Transaction root = std::move(*rr);
    (void) root.add_data("method", "GET");
    (void) root.add_data("idx", std::to_string(idx));
    if (auto cr = root.start_transaction("SQL", "select-order")) {
        fiber::cat::Transaction sql = std::move(*cr);
        (void) sql.add_data("SELECT * FROM orders WHERE id=" + std::to_string(idx));
        (void) sql.complete();
    }
    (void) root.log_event("Cache", "miss");
    (void) root.complete();
    return id;
}

// An error transaction tree: log_error + fail status => problem tree, bypasses sampling.
std::string record_error_tree(fiber::cat::CatClient &client, int idx) {
    auto tr = fiber::cat::MessageTrace::create(client);
    if (!tr) {
        return {};
    }
    fiber::cat::MessageTrace trace = std::move(*tr);
    std::string id;
    if (auto pc = trace.propagation_context()) {
        id = std::string(pc->message_id());
    }
    auto rr = trace.create_transaction("URL", "/api/pay");
    if (!rr) {
        return id;
    }
    fiber::cat::Transaction root = std::move(*rr);
    (void) root.add_data("order_id", std::to_string(idx));
    (void) root.log_error("NullPointerException", "pay service npe at idx=" + std::to_string(idx));
    (void) root.complete(fiber::cat::status::Fail);
    return id;
}

// Explicit cross-service propagation: checkout service builds an outbound child
// context for the "inventory" domain, then a second trace is created from that
// inbound context to simulate the downstream service.
std::string record_propagation_chain(fiber::cat::CatClient &client) {
    auto t1r = fiber::cat::MessageTrace::create(client);
    if (!t1r) {
        return {};
    }
    fiber::cat::MessageTrace t1 = std::move(*t1r);
    auto current = t1.propagation_context();
    if (!current) {
        return {};
    }
    const std::string root_id = std::string(current->message_id());

    auto outbound = client.create_remote_context(*current, "inventory");
    if (outbound) {
        std::cout << "  propagation: root=" << current->message_id() << " child=" << outbound->message_id()
                  << " (root_id=" << outbound->root_message_id() << " parent_id=" << outbound->parent_message_id()
                  << ")\n";
    }

    if (auto rr = t1.create_transaction("URL", "/checkout")) {
        fiber::cat::Transaction root = std::move(*rr);
        (void) root.add_data("service", "checkout");
        (void) root.complete();
    }

    if (outbound) {
        auto t2r = fiber::cat::MessageTrace::create(client, *outbound);
        if (t2r) {
            fiber::cat::MessageTrace t2 = std::move(*t2r);
            if (auto rr = t2.create_transaction("URL", "/inventory/stock")) {
                fiber::cat::Transaction root2 = std::move(*rr);
                (void) root2.add_data("service", "inventory");
                (void) root2.complete();
            }
        }
    }
    return root_id;
}

void record_metrics(fiber::cat::CatClient &client) {
    if (auto cr = fiber::cat::Metric::create_count(client, "demo.order.count")) {
        fiber::cat::Metric m = std::move(*cr);
        for (int i = 0; i < 3; ++i) {
            (void) m.record_count(1);
        }
    }
    if (auto dr = fiber::cat::Metric::create_duration(client, "demo.order.latency")) {
        fiber::cat::Metric m = std::move(*dr);
        (void) m.record_duration(std::chrono::milliseconds(12));
        (void) m.record_duration(std::chrono::milliseconds(34));
        (void) m.record_duration(std::chrono::milliseconds(56));
    }
}

fiber::async::DetachedTask run_demo(fiber::event::EventLoop *loop, fiber::cat::CatClientConfig config,
                                    fiber::cat::CatClientOptions options, const DemoSettings &settings,
                                    int *exit_code) {
    auto created = fiber::cat::CatClient::create(*loop, std::move(config), options);
    if (!created) {
        std::cerr << "error: failed to create cat client\n";
        *exit_code = 1;
        loop->stop();
        co_return;
    }
    std::unique_ptr<fiber::cat::CatClient> client = std::move(*created);
    if (!client->start()) {
        std::cerr << "error: failed to start cat client\n";
        *exit_code = 1;
        loop->stop();
        co_return;
    }
    std::cout << "cat client started (encoder=" << (options.encoder == fiber::cat::CatEncoderType::Pt1 ? "pt1" : "nt1")
              << ")\n";

    std::cout << "recording " << settings.count << " success trees...\n";
    for (int i = 0; i < settings.count; ++i) {
        std::string id = record_success_tree(*client, i);
        if (!id.empty() && i < 2) {
            std::cout << "  success tree[" << i << "] root message id: " << id << "\n";
        }
    }

    std::cout << "recording " << settings.count << " error trees...\n";
    for (int i = 0; i < settings.count; ++i) {
        std::string id = record_error_tree(*client, i);
        if (!id.empty() && i < 2) {
            std::cout << "  error tree[" << i << "] root message id: " << id << "\n";
        }
    }

    std::cout << "recording cross-service propagation chain...\n";
    (void) record_propagation_chain(*client);

    std::cout << "recording metrics...\n";
    record_metrics(*client);

    const std::size_t expected_trees = static_cast<std::size_t>(settings.count) * 2 + 2;
    const auto send_deadline = loop->now() + 5s;
    while (client->stats().sent_messages < expected_trees && loop->now() < send_deadline) {
        co_await fiber::async::sleep(10ms);
    }

    std::cout << "waiting " << settings.wait_ms << "ms for heartbeat + metric flush...\n";
    co_await fiber::async::sleep(std::chrono::milliseconds(settings.wait_ms));

    std::cout << "\nstats before shutdown:\n";
    print_stats(client->stats());

    const bool started = client->state() == fiber::cat::CatClientState::Running;
    const auto before = client->stats();
    co_await client->shutdown();
    const auto after = client->stats();
    const bool stopped = client->state() == fiber::cat::CatClientState::Stopped;

    std::cout << "\nstats after shutdown:\n";
    print_stats(after);

    std::cout << "\n=== summary ===\n";
    std::cout << "  client started:  " << (started ? "YES" : "NO") << "\n";
    std::cout << "  client stopped:  " << (stopped ? "YES" : "NO") << "\n";
    std::cout << "  router ok:       " << (before.router_successes > 0 ? "YES" : "NO") << "\n";
    std::cout << "  collector conn:  " << (before.connect_successes > 0 ? "YES" : "NO") << "\n";
    std::cout << "  messages sent:   " << before.sent_messages << "\n";
    std::cout << "  bytes sent:      " << before.sent_bytes << "\n";
    std::cout << "  heartbeat sent:  " << before.heartbeat_sent << "\n";
    const bool clean = started && stopped && before.connect_successes > 0 && before.sent_messages > 0 &&
                       before.dropped_partial_frame == 0 && before.encode_failures == 0;
    std::cout << "  result:          " << (clean ? "PASS" : "CHECK") << "\n";

    *exit_code = clean ? 0 : 1;
    loop->stop();
    co_return;
}

} // namespace

int main(int argc, char **argv) {
    auto cfg = parse_args(argc, argv);
    if (!cfg) {
        return 1;
    }

    std::cout << "=== cat-demo ===\n";
    std::cout << "app_key (domain): " << cfg->app_key << "\n";
    std::cout << "hostname:          " << cfg->hostname << "\n";
    std::cout << "ip:                " << cfg->ip << "\n";
    std::cout << "encoder:           " << cfg->settings.encoder << "\n";
    for (const auto &r: cfg->routers) {
        std::cout << "router:            " << r.host << ":" << r.port << "\n";
    }
    for (const auto &c: cfg->collectors) {
        std::cout << "collector:         " << c.to_string() << "\n";
    }
    if (cfg->routers.empty()) {
        std::cout << "router:            (disabled)\n";
    }
    if (cfg->collectors.empty()) {
        std::cout << "collector:         (disabled)\n";
    }
    std::cout << "\n";

    fiber::cat::CatClientConfigParams params{
            .app_key = cfg->app_key,
            .hostname = cfg->hostname,
            .ip = cfg->ip,
            .thread_group_name = "cat-demo",
            .thread_id = "0",
            .thread_name = "main",
            .routers = cfg->routers,
            .bootstrap_collectors = cfg->collectors,
    };
    auto config = fiber::cat::CatClientConfig::create(std::move(params));
    if (!config) {
        std::cerr << "error: invalid cat client config\n";
        return 1;
    }

    fiber::cat::CatClientOptions options;
    options.encoder = cfg->encoder;
    options.initial_sample_rate = 1.0;
    options.enable_heartbeat = true;
    options.heartbeat_initial_delay = 200ms;
    options.heartbeat_interval = 2000ms;
    options.aggregation_flush_interval = 500ms;
    options.router_connect_timeout = 1000ms;
    options.router_request_timeout = 2000ms;
    options.collector_connect_timeout = 1000ms;
    options.collector_write_timeout = 1000ms;
    options.shutdown_drain_timeout = 1000ms;

    fiber::event::EventLoop loop;
    int exit_code = 0;
    fiber::async::spawn(loop,
                        [&]() { return run_demo(&loop, std::move(*config), options, cfg->settings, &exit_code); });
    loop.run();
    return exit_code;
}
