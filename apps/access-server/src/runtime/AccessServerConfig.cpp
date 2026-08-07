#include "AccessServerConfig.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <utility>
#include <vector>

namespace fiber::access_server {
namespace {

constexpr std::string_view kListenAddress = "ACCESS_SERVER_LISTEN_ADDRESS";
constexpr std::string_view kListenPort = "ACCESS_SERVER_LISTEN_PORT";
constexpr std::string_view kMetricsListenAddress = "ACCESS_SERVER_METRICS_LISTEN_ADDRESS";
constexpr std::string_view kMetricsListenPort = "ACCESS_SERVER_METRICS_LISTEN_PORT";
constexpr std::string_view kInitialConfigTimeout = "ACCESS_SERVER_INITIAL_CONFIG_TIMEOUT_MILLIS";
constexpr std::string_view kMaxRequestBody = "ACCESS_SERVER_MAX_REQUEST_BODY_SIZE";
constexpr std::string_view kTestMode = "ACCESS_SERVER_TEST_MODE";
constexpr std::string_view kProjectsDataId = "ACCESS_SERVER_PROJECTS_DATA_ID";
constexpr std::string_view kRouteDataIdPrefix = "ACCESS_SERVER_ROUTE_DATA_ID_PREFIX";
constexpr std::string_view kRouteGroup = "ACCESS_SERVER_ROUTE_GROUP";
constexpr std::string_view kGrayDataId = "ACCESS_SERVER_GRAY_DATA_ID";
constexpr std::string_view kNamingGroup = "ACCESS_SERVER_NAMING_GROUP";
constexpr std::string_view kZone = "ACCESS_SERVER_ZONE";
constexpr std::string_view kNacosServers = "NACOS_SERVER_ADDRESSES";
constexpr std::string_view kNacosHttpPort = "NACOS_HTTP_PORT";
constexpr std::string_view kNacosGrpcPort = "NACOS_GRPC_PORT";
constexpr std::string_view kNacosNamespace = "NACOS_NAMESPACE";
constexpr std::string_view kNacosTenant = "NACOS_TENANT";
constexpr std::string_view kNacosUsername = "NACOS_USERNAME";
constexpr std::string_view kNacosPassword = "NACOS_PASSWORD";
constexpr std::string_view kNacosClientVersion = "NACOS_CLIENT_VERSION";
constexpr std::string_view kCatAppKey = "CAT_APP_KEY";
constexpr std::string_view kCatHostname = "CAT_HOSTNAME";
constexpr std::string_view kCatIp = "CAT_IP";
constexpr std::string_view kCatRouters = "CAT_ROUTER_ADDRESSES";
constexpr std::string_view kCatCollectors = "CAT_COLLECTOR_ADDRESSES";

struct Entry {
    std::string key;
    std::string value;
    std::size_t line = 0;
};

AccessServerConfigError error(AccessServerConfigErrorCode code, std::size_t line, std::string_view key,
                              std::string detail) {
    return AccessServerConfigError{
            .code = code,
            .line = line,
            .key = std::string(key),
            .detail = std::move(detail),
    };
}

std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
        value.remove_suffix(1);
    }
    return value;
}

std::expected<std::vector<Entry>, AccessServerConfigError> parse_entries(std::string_view input) {
    std::vector<Entry> entries;
    std::set<std::string, std::less<>> keys;
    std::size_t line_number = 0;
    while (!input.empty()) {
        ++line_number;
        const std::size_t newline = input.find('\n');
        std::string_view line = newline == std::string_view::npos ? input : input.substr(0, newline);
        input = newline == std::string_view::npos ? std::string_view{} : input.substr(newline + 1);
        line = trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            return std::unexpected(
                    error(AccessServerConfigErrorCode::InvalidSyntax, line_number, {}, "expected KEY=VALUE"));
        }
        const std::string_view key = trim(line.substr(0, equals));
        const std::string_view value = trim(line.substr(equals + 1));
        if (key.empty()) {
            return std::unexpected(
                    error(AccessServerConfigErrorCode::InvalidSyntax, line_number, {}, "setting name is empty"));
        }
        if (!keys.emplace(key).second) {
            return std::unexpected(error(AccessServerConfigErrorCode::DuplicateKey, line_number, key,
                                         "setting is defined more than once"));
        }
        entries.push_back(Entry{
                .key = std::string(key),
                .value = std::string(value),
                .line = line_number,
        });
    }
    return entries;
}

template<typename T>
bool parse_unsigned(std::string_view input, T &output) noexcept {
    if (input.empty()) {
        return false;
    }
    T value = 0;
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size()) {
        return false;
    }
    output = value;
    return true;
}

bool parse_boolean(std::string_view input, bool &output) noexcept {
    if (input == "true") {
        output = true;
        return true;
    }
    if (input == "false") {
        output = false;
        return true;
    }
    return false;
}

std::expected<std::pair<net::IpAddress, std::uint16_t>, AccessServerConfigError>
parse_cat_endpoint(std::string_view text, std::size_t line, std::string_view key) {
    text = trim(text);
    std::string_view host;
    std::string_view port_text;
    if (text.starts_with('[')) {
        const std::size_t bracket = text.find(']');
        if (bracket == std::string_view::npos || bracket + 1 >= text.size() || text[bracket + 1] != ':') {
            return std::unexpected(
                    error(AccessServerConfigErrorCode::InvalidValue, line, key, "expected IP:port or [IPv6]:port"));
        }
        host = text.substr(1, bracket - 1);
        port_text = text.substr(bracket + 2);
    } else {
        const std::size_t colon = text.rfind(':');
        if (colon == std::string_view::npos || text.substr(0, colon).find(':') != std::string_view::npos) {
            return std::unexpected(
                    error(AccessServerConfigErrorCode::InvalidValue, line, key, "expected IP:port or [IPv6]:port"));
        }
        host = text.substr(0, colon);
        port_text = text.substr(colon + 1);
    }

    net::IpAddress address;
    std::uint16_t port = 0;
    if (!net::IpAddress::parse(host, address) || address.is_unspecified() || address.is_multicast() ||
        !parse_unsigned(port_text, port) || port == 0) {
        return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, line, key,
                                     "expected a specified unicast IP and non-zero port"));
    }
    return std::pair(address, port);
}

std::expected<void, AccessServerConfigError> parse_cat_endpoints(std::string_view value, std::size_t line,
                                                                 std::string_view key, bool routers,
                                                                 cat::CatClientConfigParams &params) {
    if (value.empty()) {
        return {};
    }
    while (true) {
        const std::size_t separator = value.find(',');
        const std::string_view item = separator == std::string_view::npos ? value : value.substr(0, separator);
        auto endpoint = parse_cat_endpoint(item, line, key);
        if (!endpoint) {
            return std::unexpected(std::move(endpoint.error()));
        }
        if (routers) {
            params.routers.push_back(cat::CatRouterEndpoint{
                    .host = endpoint->first.to_string(),
                    .port = endpoint->second,
            });
        } else {
            params.bootstrap_collectors.emplace_back(endpoint->first, endpoint->second);
        }
        if (separator == std::string_view::npos) {
            break;
        }
        value.remove_prefix(separator + 1);
        if (value.empty()) {
            return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, line, key,
                                         "empty endpoint in comma-separated list"));
        }
    }
    return {};
}

std::expected<std::vector<net::IpAddress>, AccessServerConfigError> parse_nacos_servers(std::string_view input,
                                                                                        std::size_t line) {
    std::vector<net::IpAddress> servers;
    while (!input.empty()) {
        const std::size_t comma = input.find(',');
        const std::string_view token = trim(comma == std::string_view::npos ? input : input.substr(0, comma));
        net::IpAddress address;
        if (token.empty() || !net::IpAddress::parse(token, address)) {
            return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, line, kNacosServers,
                                         "expected a comma-separated list of IP literals"));
        }
        servers.push_back(address);
        input = comma == std::string_view::npos ? std::string_view{} : input.substr(comma + 1);
    }
    return servers;
}

AccessServerConfigError nacos_error(const nacos::NacosConfigError &source) {
    std::string detail = "invalid Nacos client configuration";
    if (source.code == nacos::NacosConfigErrorCode::EmptyServerList) {
        return error(AccessServerConfigErrorCode::MissingRequiredKey, 0, kNacosServers, "required setting is missing");
    }
    if (source.code == nacos::NacosConfigErrorCode::EmptyUsername ||
        source.code == nacos::NacosConfigErrorCode::EmptyPassword) {
        detail = "NACOS_USERNAME and NACOS_PASSWORD must both be empty or both be set";
    }
    return error(AccessServerConfigErrorCode::InvalidNacosConfig, 0, kNacosServers, std::move(detail));
}

} // namespace

AccessServerConfig::AccessServerConfig(net::SocketAddress listen_address, net::SocketAddress metrics_listen_address,
                                       std::chrono::milliseconds initial_config_timeout,
                                       std::size_t default_max_request_body_size, bool test_mode,
                                       std::optional<cat::CatClientConfig> cat_config,
                                       nacos::NacosClientConfig nacos_config,
                                       AccessConfigWatcherOptions watcher_options,
                                       GrayConfigWatcherOptions gray_watcher_options,
                                       AccessServiceDiscoveryOptions service_discovery_options) noexcept :
    listen_address_(std::move(listen_address)), metrics_listen_address_(std::move(metrics_listen_address)),
    initial_config_timeout_(initial_config_timeout), default_max_request_body_size_(default_max_request_body_size),
    test_mode_(test_mode), cat_config_(std::move(cat_config)), nacos_config_(std::move(nacos_config)),
    watcher_options_(std::move(watcher_options)), gray_watcher_options_(std::move(gray_watcher_options)),
    service_discovery_options_(std::move(service_discovery_options)) {}

std::expected<AccessServerConfig, AccessServerConfigError> AccessServerConfig::load_from_file(std::string_view path) {
    std::ifstream input(std::string(path), std::ios::binary);
    if (!input) {
        return std::unexpected(
                error(AccessServerConfigErrorCode::OpenFailed, 0, {}, "failed to open configuration file"));
    }
    std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) {
        return std::unexpected(
                error(AccessServerConfigErrorCode::ReadFailed, 0, {}, "failed to read configuration file"));
    }
    return load_from_string(contents);
}

std::expected<AccessServerConfig, AccessServerConfigError>
AccessServerConfig::load_from_string(std::string_view input) {
    auto entries = parse_entries(input);
    if (!entries) {
        return std::unexpected(std::move(entries.error()));
    }

    net::IpAddress listen_ip = net::IpAddress::any_v4();
    std::uint16_t listen_port = 16688;
    std::optional<net::IpAddress> metrics_ip;
    std::optional<std::uint16_t> metrics_port;
    std::uint64_t timeout_millis = 60000;
    std::size_t max_request_body = 400U << 20U;
    bool test_mode = false;
    cat::CatClientConfigParams cat_params{
            .thread_group_name = "access-server-cat",
            .thread_id = "0",
            .thread_name = "cat-sender",
    };
    bool cat_setting_present = false;
    nacos::NacosClientConfigParams nacos_params;
    nacos_params.namespace_id = "public";
    AccessConfigWatcherOptions watcher_options;
    GrayConfigWatcherOptions gray_options;
    AccessServiceDiscoveryOptions service_discovery_options;

    for (const Entry &entry: *entries) {
        const std::string_view value = entry.value;
        if (entry.key == kListenAddress) {
            if (!net::IpAddress::parse(value, listen_ip) || listen_ip.is_multicast()) {
                return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                             "expected a non-multicast IP literal"));
            }
        } else if (entry.key == kListenPort) {
            if (!parse_unsigned(value, listen_port) || listen_port == 0) {
                return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                             "expected a port in range 1..65535"));
            }
        } else if (entry.key == kMetricsListenAddress) {
            net::IpAddress address;
            if (!net::IpAddress::parse(value, address) || address.is_multicast()) {
                return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                             "expected a non-multicast IP literal"));
            }
            metrics_ip = address;
        } else if (entry.key == kMetricsListenPort) {
            std::uint16_t port = 0;
            if (!parse_unsigned(value, port) || port == 0) {
                return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                             "expected a port in range 1..65535"));
            }
            metrics_port = port;
        } else if (entry.key == kInitialConfigTimeout) {
            if (!parse_unsigned(value, timeout_millis) ||
                timeout_millis > static_cast<std::uint64_t>(std::chrono::milliseconds::max().count())) {
                return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                             "expected a non-negative millisecond duration"));
            }
        } else if (entry.key == kMaxRequestBody) {
            if (!parse_unsigned(value, max_request_body)) {
                return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                             "expected a non-negative byte count"));
            }
        } else if (entry.key == kTestMode) {
            if (!parse_boolean(value, test_mode)) {
                return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                             "expected true or false"));
            }
        } else if (entry.key == kProjectsDataId) {
            watcher_options.project_list_data_id = entry.value;
        } else if (entry.key == kRouteDataIdPrefix) {
            watcher_options.project_route_data_id_prefix = entry.value;
        } else if (entry.key == kRouteGroup) {
            watcher_options.project_route_group = entry.value;
        } else if (entry.key == kGrayDataId) {
            gray_options.data_id = entry.value;
        } else if (entry.key == kNamingGroup) {
            service_discovery_options.group = entry.value;
            gray_options.group = entry.value;
        } else if (entry.key == kZone) {
            service_discovery_options.zone = entry.value;
        } else if (entry.key == kNacosServers) {
            auto parsed = parse_nacos_servers(value, entry.line);
            if (!parsed) {
                return std::unexpected(std::move(parsed.error()));
            }
            nacos_params.server_ips = std::move(*parsed);
        } else if (entry.key == kNacosHttpPort) {
            if (!parse_unsigned(value, nacos_params.http_port) || nacos_params.http_port == 0) {
                return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                             "expected a port in range 1..65535"));
            }
        } else if (entry.key == kNacosGrpcPort) {
            if (!parse_unsigned(value, nacos_params.grpc_port) || nacos_params.grpc_port == 0) {
                return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                             "expected a port in range 1..65535"));
            }
        } else if (entry.key == kNacosNamespace) {
            nacos_params.namespace_id = entry.value;
        } else if (entry.key == kNacosTenant) {
            nacos_params.tenant = entry.value;
        } else if (entry.key == kNacosUsername) {
            nacos_params.username = entry.value;
        } else if (entry.key == kNacosPassword) {
            nacos_params.password = entry.value;
        } else if (entry.key == kNacosClientVersion) {
            nacos_params.client_version = entry.value;
        } else if (entry.key == kCatAppKey) {
            cat_params.app_key = entry.value;
            cat_setting_present = cat_setting_present || !entry.value.empty();
        } else if (entry.key == kCatHostname) {
            cat_params.hostname = entry.value;
            cat_setting_present = cat_setting_present || !entry.value.empty();
        } else if (entry.key == kCatIp) {
            cat_params.ip = entry.value;
            cat_setting_present = cat_setting_present || !entry.value.empty();
        } else if (entry.key == kCatRouters || entry.key == kCatCollectors) {
            auto parsed = parse_cat_endpoints(value, entry.line, entry.key, entry.key == kCatRouters, cat_params);
            if (!parsed) {
                return std::unexpected(std::move(parsed.error()));
            }
            cat_setting_present = cat_setting_present || !entry.value.empty();
        } else {
            return std::unexpected(error(AccessServerConfigErrorCode::UnknownKey, entry.line, entry.key,
                                         "unknown access-server setting"));
        }
    }

    if (watcher_options.project_list_data_id.empty() || watcher_options.project_route_data_id_prefix.empty() ||
        watcher_options.project_route_group.empty() || gray_options.data_id.empty() || gray_options.group.empty() ||
        service_discovery_options.group.empty()) {
        return std::unexpected(
                error(AccessServerConfigErrorCode::InvalidValue, 0, {}, "Nacos data IDs and groups must be non-empty"));
    }
    auto nacos_config = nacos::NacosClientConfig::create(std::move(nacos_params));
    if (!nacos_config) {
        return std::unexpected(nacos_error(nacos_config.error()));
    }
    std::optional<cat::CatClientConfig> cat_config;
    if (cat_setting_present) {
        auto created = cat::CatClientConfig::create(std::move(cat_params));
        if (!created) {
            return std::unexpected(
                    error(AccessServerConfigErrorCode::InvalidValue, 0, {},
                          "CAT_APP_KEY, CAT_HOSTNAME, CAT_IP and at least one CAT endpoint are required"));
        }
        cat_config = std::move(*created);
    }
    if (!metrics_port) {
        if (listen_port == std::numeric_limits<std::uint16_t>::max()) {
            return std::unexpected(error(AccessServerConfigErrorCode::InvalidValue, 0, kMetricsListenPort,
                                         "metrics port must be explicit when the HTTP port is 65535"));
        }
        metrics_port = static_cast<std::uint16_t>(listen_port + 1);
    }
    return AccessServerConfig(net::SocketAddress(listen_ip, listen_port),
                              net::SocketAddress(metrics_ip.value_or(listen_ip), *metrics_port),
                              std::chrono::milliseconds(timeout_millis), max_request_body, test_mode,
                              std::move(cat_config), std::move(*nacos_config), std::move(watcher_options),
                              std::move(gray_options), std::move(service_discovery_options));
}

} // namespace fiber::access_server
