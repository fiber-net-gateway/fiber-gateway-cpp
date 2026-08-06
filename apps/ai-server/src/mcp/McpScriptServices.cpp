#include "McpScriptServices.h"

#include "McpJsonCodec.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

#include <common/Assert.h>
#include <http/Http1ClientConnection.h>
#include <http/Http1ConnectionGroupKey.h>
#include <net/SocketAddress.h>

namespace fiber::ai_server {
namespace {

constexpr std::string_view kServicePrefix = "mcp-service:";
constexpr std::string_view kAddressPrefix = "mcp-address:";

std::string_view logical_cluster(std::string_view cluster) noexcept {
    const std::size_t separator = cluster.find('-');
    return separator == std::string_view::npos ? cluster : cluster.substr(separator + 1);
}

bool cluster_equal(std::string_view left, std::string_view right) noexcept {
    left = logical_cluster(left);
    right = logical_cluster(right);
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        char l = left[i];
        char r = right[i];
        if (l >= 'A' && l <= 'Z') {
            l = static_cast<char>(l - 'A' + 'a');
        }
        if (r >= 'A' && r <= 'Z') {
            r = static_cast<char>(r - 'A' + 'a');
        }
        if (l != r) {
            return false;
        }
    }
    return true;
}

bool local_zone(std::string_view cluster, std::string_view expected) noexcept {
    const std::size_t separator = cluster.find('-');
    return separator == std::string_view::npos || cluster.substr(0, separator) == expected;
}

std::uint32_t endpoint_weight(double weight) noexcept {
    if (!std::isfinite(weight) || weight <= 0) {
        return 0;
    }
    constexpr double kScale = 100.0;
    constexpr double kMax = static_cast<double>(std::numeric_limits<std::uint32_t>::max());
    return static_cast<std::uint32_t>(std::clamp(weight * kScale, 1.0, kMax));
}

struct ParsedAddress {
    std::string host;
    net::IpAddress ip;
    std::uint16_t port = 0;
    bool host_is_ip = false;
    bool tls = false;
};

std::optional<ParsedAddress> parse_address(std::string_view value) noexcept {
    ParsedAddress output;
    if (value.starts_with("http://")) {
        value.remove_prefix(7);
    } else if (value.starts_with("https://")) {
        value.remove_prefix(8);
        output.tls = true;
    }
    const std::size_t slash = value.find('/');
    if (slash != std::string_view::npos) {
        value = value.substr(0, slash);
    }
    std::string_view host = value;
    std::string_view port;
    if (!value.empty() && value.front() == '[') {
        const std::size_t close = value.find(']');
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        host = value.substr(1, close - 1);
        if (close + 1 < value.size()) {
            if (value[close + 1] != ':') {
                return std::nullopt;
            }
            port = value.substr(close + 2);
        }
    } else {
        const std::size_t colon = value.rfind(':');
        if (colon != std::string_view::npos && value.find(':') == colon) {
            host = value.substr(0, colon);
            port = value.substr(colon + 1);
        }
    }
    if (host.empty()) {
        return std::nullopt;
    }
    output.port = output.tls ? 443 : 80;
    if (!port.empty()) {
        unsigned int parsed_port = 0;
        const auto converted = std::from_chars(port.data(), port.data() + port.size(), parsed_port);
        if (converted.ec != std::errc{} || converted.ptr != port.data() + port.size() || parsed_port == 0 ||
            parsed_port > 65535) {
            return std::nullopt;
        }
        output.port = static_cast<std::uint16_t>(parsed_port);
    }
    output.host = std::string(host);
    output.host_is_ip = net::IpAddress::parse(host, output.ip);
    return output;
}

class ConnectedMcpUpstream final : public http_script::HttpUpstreamConnection {
public:
    ConnectedMcpUpstream(http::LocalHttp1ConnectionPoolSet::Lease lease, http::Http1ClientConnection &connection,
                         std::string authority) noexcept :
        lease_(std::move(lease)), connection_(&connection), authority_(std::move(authority)) {}

    [[nodiscard]] http::Http1ClientConnection &connection() noexcept override { return *connection_; }
    [[nodiscard]] std::string_view authority() const noexcept override { return authority_; }

private:
    http::LocalHttp1ConnectionPoolSet::Lease lease_;
    http::Http1ClientConnection *connection_ = nullptr;
    std::string authority_;
};

} // namespace

struct McpScriptServices::ServiceWatch final {
    McpScriptServices *owner = nullptr;
    std::string key;
    std::string service_name;
    std::string cluster;
    nacos::Subscription<nacos::ServiceInfo> subscription;
    std::vector<Endpoint> endpoints;
};

McpScriptServices::McpScriptServices(event::EventLoop &nacos_loop, nacos::NamingService &naming_service,
                                     event::EventLoopGroup &workers, std::string local_zone) noexcept :
    nacos_loop_(&nacos_loop), naming_service_(&naming_service), workers_(&workers), local_zone_(std::move(local_zone)),
    pool_(workers, http::Http1ConnectionPoolCore::Options{
                           .max_idle_per_group = 4,
                           .max_idle_total = 128,
                           .idle_timeout = std::chrono::seconds(30),
                           .initial_group_capacity = 16,
                   }) {
    auto empty = std::make_shared<const DirectorySnapshot>();
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    directory_.store(std::move(empty), std::memory_order_release);
#else
    std::atomic_store_explicit(&directory_, std::move(empty), std::memory_order_release);
#endif
}

McpScriptServices::~McpScriptServices() {
    FIBER_ASSERT(!nacos_started_);
    FIBER_ASSERT(!workers_initialized_);
    FIBER_ASSERT(watches_.empty());
}

void McpScriptServices::start_nacos() noexcept {
    FIBER_ASSERT(nacos_loop_->in_loop());
    nacos_started_ = true;
}

bool McpScriptServices::observe_target(const http_script::HttpTargetSpec &target) noexcept {
    FIBER_ASSERT(nacos_loop_->in_loop());
    if (!nacos_started_ || target.kind != http_script::HttpTargetSpec::Kind::Upstream ||
        !target.name.starts_with(kServicePrefix)) {
        return target.kind != http_script::HttpTargetSpec::Kind::Upstream || target.name.starts_with(kAddressPrefix);
    }
    if (watches_.contains(target.name)) {
        return true;
    }
    std::string_view value(target.name);
    value.remove_prefix(kServicePrefix.size());
    const std::size_t slash = value.rfind('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 == value.size()) {
        return false;
    }
    auto watch = std::make_unique<ServiceWatch>();
    watch->owner = this;
    watch->key = target.name;
    watch->service_name = std::string(value.substr(0, slash));
    watch->cluster = std::string(value.substr(slash + 1));
    auto subscription =
            naming_service_->subscribe(watch->service_name, kMcpAdminServiceGroup, &service_notify, watch.get());
    if (!subscription) {
        return false;
    }
    watch->subscription = std::move(*subscription);
    watches_.emplace(watch->key, std::move(watch));
    publish_directory();
    return true;
}

void McpScriptServices::service_notify(void *context,
                                       const nacos::SubscriptionResult<nacos::ServiceInfo> &result) noexcept {
    auto &watch = *static_cast<ServiceWatch *>(context);
    if (result.kind == nacos::ResultKind::Closed) {
        watch.endpoints.clear();
        watch.owner->publish_directory();
        return;
    }
    if (result.data) {
        watch.owner->apply_service(watch, *result.data);
    }
}

void McpScriptServices::apply_service(ServiceWatch &watch, const nacos::ServiceInfo &info) {
    FIBER_ASSERT(nacos_loop_->in_loop());
    std::vector<Endpoint> primary;
    std::vector<Endpoint> secondary;
    primary.reserve(info.hosts.size());
    secondary.reserve(info.hosts.size());
    for (const nacos::ServiceInstance &instance: info.hosts) {
        const std::uint32_t weight = endpoint_weight(instance.weight);
        if (!instance.enabled || !instance.healthy || instance.port == 0 || weight == 0 ||
            (!instance.cluster_name.empty() && !cluster_equal(instance.cluster_name, watch.cluster))) {
            continue;
        }
        net::IpAddress ip;
        if (!net::IpAddress::parse(instance.ip, ip) || ip.is_unspecified() || ip.is_multicast()) {
            continue;
        }
        std::string authority(instance.ip);
        authority.push_back(':');
        authority.append(std::to_string(instance.port));
        Endpoint endpoint{
                .ip = ip,
                .authority = std::move(authority),
                .port = instance.port,
                .weight = weight,
        };
        auto &target = local_zone(instance.cluster_name, local_zone_) ? primary : secondary;
        target.push_back(std::move(endpoint));
    }
    watch.endpoints = primary.empty() ? std::move(secondary) : std::move(primary);
    publish_directory();
}

void McpScriptServices::publish_directory() {
    auto snapshot = std::make_shared<DirectorySnapshot>();
    snapshot->entries.reserve(watches_.size());
    for (const auto &[key, watch]: watches_) {
        snapshot->entries.push_back(DirectoryEntry{
                .key = key,
                .endpoints = watch->endpoints,
        });
    }
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    directory_.store(std::move(snapshot), std::memory_order_release);
#else
    std::atomic_store_explicit(&directory_, std::move(snapshot), std::memory_order_release);
#endif
}

std::vector<McpScriptServices::Endpoint>
McpScriptServices::resolve_target(const http_script::HttpTargetSpec &target) const {
    if (target.kind == http_script::HttpTargetSpec::Kind::Url) {
        return {};
    }
    if (target.name.starts_with(kServicePrefix)) {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
        const auto snapshot = directory_.load(std::memory_order_acquire);
#else
        const auto snapshot = std::atomic_load_explicit(&directory_, std::memory_order_acquire);
#endif
        const auto it =
                std::lower_bound(snapshot->entries.begin(), snapshot->entries.end(), target.name,
                                 [](const DirectoryEntry &entry, std::string_view key) { return entry.key < key; });
        return it != snapshot->entries.end() && it->key == target.name ? it->endpoints : std::vector<Endpoint>{};
    }
    return {};
}

async::Task<bool> McpScriptServices::init_workers() noexcept {
    if (workers_initialized_) {
        co_return true;
    }
    if (!pool_.init()) {
        co_return false;
    }
    pool_initialized_ = true;
    if (!co_await dns_.init(*workers_)) {
        co_await dns_.shutdown();
        co_await pool_.shutdown_async();
        pool_initialized_ = false;
        co_return false;
    }
    workers_initialized_ = true;
    co_return true;
}

async::Task<common::IoResult<std::unique_ptr<http_script::HttpUpstreamConnection>>>
McpScriptServices::acquire(const http_script::HttpTargetSpec &target,
                           std::chrono::milliseconds connect_timeout) noexcept {
    using Output = std::unique_ptr<http_script::HttpUpstreamConnection>;
    if (!workers_initialized_) {
        co_return std::unexpected(common::IoErr::Canceled);
    }

    Endpoint endpoint;
    std::optional<ParsedAddress> address;
    if (target.kind == http_script::HttpTargetSpec::Kind::Url) {
        std::string url = target.tls ? "https://" : "http://";
        url.append(target.name);
        if (target.port != 0) {
            url.push_back(':');
            url.append(std::to_string(target.port));
        }
        address = parse_address(url);
    } else if (target.name.starts_with(kAddressPrefix)) {
        std::string_view addresses(target.name);
        addresses.remove_prefix(kAddressPrefix.size());
        std::vector<std::string_view> choices;
        while (!addresses.empty()) {
            const std::size_t comma = addresses.find(',');
            choices.push_back(addresses.substr(0, comma));
            if (comma == std::string_view::npos) {
                break;
            }
            addresses.remove_prefix(comma + 1);
        }
        if (choices.empty()) {
            co_return std::unexpected(common::IoErr::NotFound);
        }
        address = parse_address(choices[selection_.fetch_add(1, std::memory_order_relaxed) % choices.size()]);
    } else {
        std::vector<Endpoint> endpoints = resolve_target(target);
        if (endpoints.empty()) {
            co_return std::unexpected(common::IoErr::NotFound);
        }
        std::uint64_t total_weight = 0;
        for (const Endpoint &candidate: endpoints) {
            total_weight += candidate.weight;
        }
        std::uint64_t ticket = selection_.fetch_add(1, std::memory_order_relaxed) % total_weight;
        for (Endpoint &candidate: endpoints) {
            if (ticket < candidate.weight) {
                endpoint = std::move(candidate);
                break;
            }
            ticket -= candidate.weight;
        }
    }
    if (address) {
        endpoint.port = address->port;
        endpoint.tls = address->tls;
        endpoint.authority = address->host;
        if (address->port != (address->tls ? 443 : 80)) {
            endpoint.authority.push_back(':');
            endpoint.authority.append(std::to_string(address->port));
        }
        if (address->host_is_ip) {
            endpoint.ip = address->ip;
        } else {
            endpoint.pool_name = address->host;
            auto resolved = co_await dns_.resolve(address->host);
            if (!resolved || resolved->size == 0) {
                co_return std::unexpected(resolved ? common::IoErr::NotFound : resolved.error().io_error);
            }
            endpoint.ip = resolved->addresses[0];
        }
    }
    if (endpoint.port == 0 || endpoint.ip.is_unspecified()) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    const auto scheme =
            endpoint.tls ? http::Http1ConnectionGroupKey::Scheme::Https : http::Http1ConnectionGroupKey::Scheme::Http;
    std::optional<http::Http1ConnectionGroupKey> key;
    if (endpoint.pool_name.empty()) {
        key = http::Http1ConnectionGroupKey::from_ip(endpoint.ip, endpoint.port, scheme);
    } else {
        key = http::Http1ConnectionGroupKey::from_name(endpoint.pool_name, endpoint.port, scheme);
    }
    if (!key) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    auto lease = pool_.acquire(*key);
    if (!lease.valid()) {
        co_return std::unexpected(common::IoErr::Canceled);
    }
    http::Http1ClientConnection *connection = lease.get();
    if (!connection) {
        http::Http1ClientConnectionOptions options{
                .peer_addr = net::SocketAddress(endpoint.ip, endpoint.port),
        };
        if (endpoint.tls) {
            options.tls.enabled = true;
            options.tls.verify_peer = true;
            if (endpoint.pool_name.empty()) {
                options.tls.verify_name = endpoint.ip.to_string();
            } else {
                options.tls.server_name = endpoint.pool_name;
            }
        }
        auto created = lease.emplace_connection(std::move(options));
        if (!created) {
            co_return std::unexpected(created.error());
        }
        connection = *created;
        auto connected = co_await connection->connect(connect_timeout);
        if (!connected) {
            co_return std::unexpected(connected.error());
        }
    }
    co_return Output(new ConnectedMcpUpstream(std::move(lease), *connection, std::move(endpoint.authority)));
}

async::Task<void> McpScriptServices::shutdown_workers() noexcept {
    if (pool_initialized_) {
        co_await pool_.shutdown_async();
        pool_initialized_ = false;
    }
    co_await dns_.shutdown();
    workers_initialized_ = false;
}

async::Task<void> McpScriptServices::shutdown_nacos() noexcept {
    FIBER_ASSERT(nacos_loop_->in_loop());
    for (auto &[key, watch]: watches_) {
        (void) key;
        watch->subscription.close();
    }
    watches_.clear();
    publish_directory();
    nacos_started_ = false;
    co_return;
}

} // namespace fiber::ai_server
