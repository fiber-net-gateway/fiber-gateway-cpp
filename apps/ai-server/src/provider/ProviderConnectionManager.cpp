#include "ProviderConnectionManager.h"

#include <array>
#include <charconv>
#include <optional>
#include <utility>

#include <common/Assert.h>
#include <event/EventLoop.h>
#include <event/EventLoopGroup.h>
#include <http/Http1ConnectionGroupKey.h>
#include <net/SocketAddress.h>

namespace fiber::ai_server {
namespace {

using ConnectionKey = http::Http1ConnectionGroupKey;

struct DialTarget {
    ParsedProviderEndpoint endpoint;
    std::optional<ConnectionKey> key;
    ProviderResolvedAddresses addresses;
    std::string host_header;
    std::string target;
    std::string tls_server_name;
    ProviderLoadBalanceLease load_balance;
};

ProviderConnectionError error(ProviderConnectionErrorCode code, const char *message,
                              common::IoErr io_error = common::IoErr::None, std::uint64_t failed_service_peer_id = 0,
                              bool dns_backoff_hit = false) noexcept {
    return ProviderConnectionError{
            .code = code,
            .io_error = io_error,
            .message = message,
            .failed_service_peer_id = failed_service_peer_id,
            .dns_backoff_hit = dns_backoff_hit,
    };
}

std::string host_header(std::string_view host, bool ipv6, std::uint16_t port, std::uint16_t default_port,
                        bool always_port) {
    std::string value;
    value.reserve(host.size() + 10);
    if (ipv6) {
        value.push_back('[');
        value.append(host);
        value.push_back(']');
    } else {
        value.append(host);
    }
    if (always_port || port != default_port) {
        std::array<char, 5> port_text{};
        const auto converted = std::to_chars(port_text.data(), port_text.data() + port_text.size(), port);
        value.push_back(':');
        value.append(port_text.data(), converted.ptr);
    }
    return value;
}

std::string request_target(std::string_view base_path, std::string_view protocol_path) {
    if (base_path.empty() || base_path == "/") {
        return std::string(protocol_path);
    }
    std::string target;
    target.reserve(base_path.size() + protocol_path.size());
    target.append(base_path);
    if (target.back() == '/' && protocol_path.starts_with('/')) {
        target.pop_back();
    } else if (target.back() != '/' && !protocol_path.starts_with('/')) {
        target.push_back('/');
    }
    target.append(protocol_path);
    return target;
}

std::optional<ConnectionKey> connection_key(const ParsedProviderEndpoint &endpoint) noexcept {
    const ConnectionKey::Scheme scheme = endpoint.tls() ? ConnectionKey::Scheme::Https : ConnectionKey::Scheme::Http;
    if (endpoint.host_is_ip) {
        return ConnectionKey::from_ip(endpoint.ip, endpoint.port, scheme);
    }
    return ConnectionKey::from_name(endpoint.host, endpoint.port, scheme);
}

http::Http1ClientConnectionOptions connection_options(const net::IpAddress &ip, const ParsedProviderEndpoint &endpoint,
                                                      std::string_view tls_server_name) {
    http::Http1ClientConnectionOptions options;
    options.peer_addr = net::SocketAddress(ip, endpoint.port);
    if (endpoint.tls()) {
        options.tls.enabled = true;
        options.tls.verify_peer = true;
        if (endpoint.host_is_ip) {
            options.tls.verify_name = std::string(endpoint.host);
        } else {
            options.tls.server_name = std::string(tls_server_name);
        }
    }
    return options;
}

} // namespace

ProviderConnectionManager::ProviderConnectionManager(event::EventLoopGroup &workers) noexcept :
    ProviderConnectionManager(workers, {}) {}

ProviderConnectionManager::ProviderConnectionManager(event::EventLoopGroup &workers,
                                                     WorkerDnsService::Options dns_options) noexcept :
    workers_(&workers), dns_(std::move(dns_options)), pool_(workers, http::Http1ConnectionPoolCore::Options{
                                                                             .max_idle_per_group = 4,
                                                                             .max_idle_total = 256,
                                                                             .idle_timeout = std::chrono::seconds(30),
                                                                             .initial_group_capacity = 16,
                                                                     }) {}

ProviderConnectionManager::~ProviderConnectionManager() { FIBER_ASSERT(!initialized_); }

async::Task<bool> ProviderConnectionManager::init() noexcept {
    if (initialized_) {
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
    initialized_ = true;
    co_return true;
}

async::Task<void> ProviderConnectionManager::shutdown() noexcept {
    if (pool_initialized_) {
        co_await pool_.shutdown_async();
        pool_initialized_ = false;
    }
    co_await dns_.shutdown();
    initialized_ = false;
}

async::Task<std::expected<ProviderConnectionLease, ProviderConnectionError>>
ProviderConnectionManager::acquire(const ResolvedProviderAttempt &attempt, std::chrono::milliseconds connect_timeout,
                                   ProviderServiceSelection service_selection) noexcept {
    if (!initialized_ || !attempt.provider || !attempt.provider->config || !attempt.protocol) {
        co_return std::unexpected(
                error(ProviderConnectionErrorCode::InvalidEndpoint, "provider attempt is incomplete"));
    }

    auto parsed = parse_provider_endpoint(attempt.provider->config->base_url);
    if (!parsed) {
        co_return std::unexpected(error(ProviderConnectionErrorCode::InvalidEndpoint, parsed.error().message));
    }
    DialTarget dial;
    dial.endpoint = *parsed;
    dial.target = request_target(dial.endpoint.base_path, attempt.protocol->path);

    if (dial.endpoint.is_service()) {
        if (!attempt.provider->service) {
            co_return std::unexpected(error(ProviderConnectionErrorCode::NoServiceEndpoint,
                                            "provider service discovery is unavailable", common::IoErr::NotFound));
        }
        auto selected = attempt.provider->service->select(service_selection.rendezvous_key,
                                                          service_selection.excluded_peer_ids);
        if (!selected) {
            co_return std::unexpected(error(ProviderConnectionErrorCode::NoServiceEndpoint,
                                            "provider service has no usable endpoint", common::IoErr::NotFound));
        }
        const net::IpAddress &selected_ip = selected->ip_address();
        dial.endpoint.scheme = ProviderEndpointScheme::Http;
        dial.endpoint.ip = selected_ip;
        dial.endpoint.host_is_ip = true;
        dial.endpoint.port = selected->port();
        dial.addresses.addresses[0] = selected_ip;
        dial.addresses.size = 1;
        dial.host_header = std::string(selected->authority());
        dial.key = http::Http1ConnectionGroupKey::from_ip(selected_ip, selected->port(),
                                                          http::Http1ConnectionGroupKey::Scheme::Http);
        dial.load_balance = ProviderLoadBalanceLease{
                .instance = std::move(*selected),
        };
    } else {
        dial.host_header = host_header(dial.endpoint.host, dial.endpoint.host_is_ip && dial.endpoint.ip.is_v6(),
                                       dial.endpoint.port, dial.endpoint.tls() ? 443 : 80, false);
        if (dial.endpoint.host_is_ip) {
            dial.addresses.addresses[0] = dial.endpoint.ip;
            dial.addresses.size = 1;
        } else {
            dial.tls_server_name = std::string(dial.endpoint.host);
        }
    }

    if (!dial.key) {
        auto key = connection_key(dial.endpoint);
        if (!key) {
            co_return std::unexpected(error(ProviderConnectionErrorCode::InvalidEndpoint,
                                            "provider host cannot be used as a connection pool key"));
        }
        dial.key = *key;
    }

    ProviderConnectionLease output;
    output.load_balance = std::move(dial.load_balance);
    output.lease = pool_.acquire(*dial.key);
    if (!output.lease.valid()) {
        co_return std::unexpected(error(ProviderConnectionErrorCode::PoolShutdown,
                                        "provider connection pool is shutting down", common::IoErr::Canceled));
    }
    if (output.lease.has_connection()) {
        output.connection = output.lease.get();
        output.host_header = std::move(dial.host_header);
        output.target = std::move(dial.target);
        co_return std::move(output);
    }

    if (dial.addresses.size == 0) {
        auto resolved = co_await dns_.resolve(dial.endpoint.host);
        if (!resolved) {
            co_return std::unexpected(error(ProviderConnectionErrorCode::Dns, "provider DNS resolution failed",
                                            resolved.error().io_error, 0, resolved.error().backoff_hit));
        }
        dial.addresses = *resolved;
    }

    common::IoErr last_error = common::IoErr::NotFound;
    for (std::uint16_t i = 0; i < dial.addresses.size; ++i) {
        if (i > 0) {
            output.lease = pool_.acquire(*dial.key);
            if (!output.lease.valid()) {
                co_return std::unexpected(error(ProviderConnectionErrorCode::PoolShutdown,
                                                "provider connection pool is shutting down", common::IoErr::Canceled));
            }
            if (output.lease.has_connection()) {
                output.connection = output.lease.get();
                output.host_header = std::move(dial.host_header);
                output.target = std::move(dial.target);
                co_return std::move(output);
            }
        }
        auto connection = output.lease.emplace_connection(
                connection_options(dial.addresses.addresses[i], dial.endpoint, dial.tls_server_name));
        if (!connection) {
            last_error = connection.error();
            output.lease.reset();
            continue;
        }
        auto connected = co_await (*connection)->connect(connect_timeout);
        if (connected) {
            output.connection = *connection;
            output.host_header = std::move(dial.host_header);
            output.target = std::move(dial.target);
            co_return std::move(output);
        }
        last_error = connected.error();
        output.lease.reset();
    }
    const InstanceReportOutcome outcome =
            last_error == common::IoErr::NoMem ? InstanceReportOutcome::Neutral : InstanceReportOutcome::Failure;
    const std::uint64_t failed_service_peer_id =
            outcome == InstanceReportOutcome::Failure ? output.load_balance.peer_id() : 0;
    output.load_balance.report(outcome);
    co_return std::unexpected(error(ProviderConnectionErrorCode::Connect, "provider connection failed", last_error,
                                    failed_service_peer_id));
}

} // namespace fiber::ai_server
