#include "AiServerConfig.h"

#include <array>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

namespace fiber::ai_server {
namespace {

constexpr std::string_view kListenAddressKey = "AI_SERVER_LISTEN_ADDRESS";
constexpr std::string_view kListenPortKey = "AI_SERVER_LISTEN_PORT";
constexpr std::string_view kInitialConfigTimeoutKey = "AI_SERVER_INITIAL_CONFIG_TIMEOUT_MS";
constexpr std::string_view kAdvertiseAddressKey = "AI_SERVER_ADVERTISE_ADDRESS";
constexpr std::string_view kServiceNameKey = "AI_SERVER_SERVICE_NAME";
constexpr std::string_view kServiceGroupKey = "AI_SERVER_SERVICE_GROUP";
constexpr std::string_view kNacosServerAddressesKey = "NACOS_SERVER_ADDRESSES";
constexpr std::string_view kNacosHttpPortKey = "NACOS_HTTP_PORT";
constexpr std::string_view kNacosGrpcPortKey = "NACOS_GRPC_PORT";
constexpr std::string_view kNacosNamespaceKey = "NACOS_NAMESPACE_ID";
constexpr std::string_view kNacosTenantKey = "NACOS_TENANT";
constexpr std::string_view kNacosUsernameKey = "NACOS_USERNAME";
constexpr std::string_view kNacosPasswordKey = "NACOS_PASSWORD";
constexpr std::string_view kNacosContextPathKey = "NACOS_CONTEXT_PATH";
constexpr std::string_view kNacosClientVersionKey = "NACOS_CLIENT_VERSION";
constexpr std::string_view kCatAppKey = "CAT_APP_KEY";
constexpr std::string_view kCatHostnameKey = "CAT_HOSTNAME";
constexpr std::string_view kCatIpKey = "CAT_IP";
constexpr std::string_view kCatRouterAddressesKey = "CAT_ROUTER_ADDRESSES";
constexpr std::string_view kCatCollectorAddressesKey = "CAT_COLLECTOR_ADDRESSES";
constexpr std::string_view kLogConfigPathKey = "AI_SERVER_LOG_CONFIG_PATH";

constexpr std::array<std::string_view, 21> kKnownKeys = {
        kListenAddressKey,        kListenPortKey,
        kInitialConfigTimeoutKey, kAdvertiseAddressKey,
        kServiceNameKey,          kServiceGroupKey,
        kNacosServerAddressesKey, kNacosHttpPortKey,
        kNacosGrpcPortKey,        kNacosNamespaceKey,
        kNacosTenantKey,          kNacosUsernameKey,
        kNacosPasswordKey,        kNacosContextPathKey,
        kNacosClientVersionKey,   kCatAppKey,
        kCatHostnameKey,          kCatIpKey,
        kCatRouterAddressesKey,   kCatCollectorAddressesKey,
        kLogConfigPathKey,
};

struct EnvEntry {
    std::string key;
    std::string value;
    std::size_t line = 0;
};

struct FieldLines {
    std::size_t server_addresses = 0;
    std::size_t http_port = 0;
    std::size_t grpc_port = 0;
    std::size_t username = 0;
    std::size_t password = 0;
    std::size_t context_path = 0;
    std::size_t cat_app_key = 0;
    std::size_t cat_hostname = 0;
    std::size_t cat_ip = 0;
    std::size_t cat_routers = 0;
    std::size_t cat_collectors = 0;
};

AiServerConfigError make_error(AiServerConfigErrorCode code, std::size_t line, std::string_view key,
                               std::string detail) {
    return {.code = code, .line = line, .key = std::string(key), .detail = std::move(detail)};
}

std::expected<std::string, AiServerConfigError> resolve_config_reference(std::string_view source_path,
                                                                         std::string_view reference) {
    namespace fs = std::filesystem;
    fs::path target(reference);
    if (!target.is_absolute() && !source_path.empty()) {
        target = fs::path(source_path).parent_path() / target;
    }
    std::error_code error;
    fs::path absolute = fs::absolute(target, error);
    if (error) {
        return std::unexpected(make_error(AiServerConfigErrorCode::InvalidValue, 0, kLogConfigPathKey,
                                          "failed to resolve logging configuration path: " + error.message()));
    }
    return absolute.lexically_normal().string();
}

std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return value;
}

bool valid_key(std::string_view key) noexcept {
    if (key.empty() || (std::isalpha(static_cast<unsigned char>(key.front())) == 0 && key.front() != '_')) {
        return false;
    }
    for (const char ch: key.substr(1)) {
        if (std::isalnum(static_cast<unsigned char>(ch)) == 0 && ch != '_') {
            return false;
        }
    }
    return true;
}

bool known_key(std::string_view key) noexcept {
    for (const std::string_view known: kKnownKeys) {
        if (known == key) {
            return true;
        }
    }
    return false;
}

std::expected<std::string, AiServerConfigError> parse_quoted_value(std::string_view value, std::size_t line,
                                                                   std::string_view key) {
    const char quote = value.front();
    std::string parsed;
    parsed.reserve(value.size());

    std::size_t i = 1;
    for (; i < value.size(); ++i) {
        const char ch = value[i];
        if (ch == quote) {
            break;
        }
        if (quote == '"' && ch == '\\') {
            if (++i >= value.size()) {
                return std::unexpected(make_error(AiServerConfigErrorCode::InvalidSyntax, line, key,
                                                  "unterminated escape in quoted value"));
            }
            switch (value[i]) {
                case 'n':
                    parsed.push_back('\n');
                    break;
                case 'r':
                    parsed.push_back('\r');
                    break;
                case 't':
                    parsed.push_back('\t');
                    break;
                case '\\':
                case '"':
                    parsed.push_back(value[i]);
                    break;
                default:
                    return std::unexpected(make_error(AiServerConfigErrorCode::InvalidSyntax, line, key,
                                                      "unsupported escape in quoted value"));
            }
            continue;
        }
        parsed.push_back(ch);
    }

    if (i == value.size()) {
        return std::unexpected(
                make_error(AiServerConfigErrorCode::InvalidSyntax, line, key, "unterminated quoted value"));
    }

    const std::string_view trailing = trim(value.substr(i + 1));
    if (!trailing.empty() && trailing.front() != '#') {
        return std::unexpected(
                make_error(AiServerConfigErrorCode::InvalidSyntax, line, key, "unexpected text after quoted value"));
    }
    return parsed;
}

std::expected<std::optional<EnvEntry>, AiServerConfigError> parse_line(std::string_view line_text,
                                                                       std::size_t line_number) {
    if (line_number == 1 && line_text.starts_with("\xEF\xBB\xBF")) {
        line_text.remove_prefix(3);
    }
    line_text = trim(line_text);
    if (line_text.empty() || line_text.front() == '#') {
        return std::optional<EnvEntry>{};
    }

    if (line_text.starts_with("export") && line_text.size() > 6 &&
        std::isspace(static_cast<unsigned char>(line_text[6])) != 0) {
        line_text = trim(line_text.substr(7));
    }

    const std::size_t separator = line_text.find('=');
    if (separator == std::string_view::npos) {
        return std::unexpected(
                make_error(AiServerConfigErrorCode::InvalidSyntax, line_number, {}, "expected KEY=VALUE"));
    }

    const std::string_view key = trim(line_text.substr(0, separator));
    if (!valid_key(key)) {
        return std::unexpected(
                make_error(AiServerConfigErrorCode::InvalidSyntax, line_number, key, "invalid environment key"));
    }
    if (!known_key(key)) {
        return std::unexpected(
                make_error(AiServerConfigErrorCode::UnknownKey, line_number, key, "unknown ai-server setting"));
    }

    std::string_view value = trim(line_text.substr(separator + 1));
    std::string parsed_value;
    if (!value.empty() && (value.front() == '\'' || value.front() == '"')) {
        auto quoted = parse_quoted_value(value, line_number, key);
        if (!quoted) {
            return std::unexpected(std::move(quoted.error()));
        }
        parsed_value = std::move(*quoted);
    } else {
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '#' && (i == 0 || std::isspace(static_cast<unsigned char>(value[i - 1])) != 0)) {
                value = value.substr(0, i);
                break;
            }
        }
        parsed_value = std::string(trim(value));
    }

    return std::optional<EnvEntry>(
            EnvEntry{.key = std::string(key), .value = std::move(parsed_value), .line = line_number});
}

std::expected<std::vector<EnvEntry>, AiServerConfigError> parse_entries(std::string_view input) {
    std::vector<EnvEntry> entries;
    std::size_t line_number = 1;
    while (true) {
        const std::size_t end = input.find('\n');
        std::string_view line = end == std::string_view::npos ? input : input.substr(0, end);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        auto entry = parse_line(line, line_number);
        if (!entry) {
            return std::unexpected(std::move(entry.error()));
        }
        if (*entry) {
            for (const EnvEntry &existing: entries) {
                if (existing.key == (*entry)->key) {
                    return std::unexpected(make_error(AiServerConfigErrorCode::DuplicateKey, line_number, (*entry)->key,
                                                      "duplicate environment key"));
                }
            }
            entries.push_back(std::move(**entry));
        }

        if (end == std::string_view::npos) {
            break;
        }
        input.remove_prefix(end + 1);
        ++line_number;
    }
    return entries;
}

bool parse_port(std::string_view text, bool allow_zero, std::uint16_t &out) noexcept {
    unsigned int value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size() || value > 65535 ||
        (!allow_zero && value == 0)) {
        return false;
    }
    out = static_cast<std::uint16_t>(value);
    return true;
}

std::expected<std::pair<net::IpAddress, std::uint16_t>, AiServerConfigError>
parse_cat_endpoint(std::string_view text, std::size_t line, std::string_view key) {
    text = trim(text);
    std::string_view host;
    std::string_view port_text;
    if (text.starts_with('[')) {
        const std::size_t bracket = text.find(']');
        if (bracket == std::string_view::npos || bracket + 1 >= text.size() || text[bracket + 1] != ':') {
            return std::unexpected(
                    make_error(AiServerConfigErrorCode::InvalidValue, line, key, "expected IP:port or [IPv6]:port"));
        }
        host = text.substr(1, bracket - 1);
        port_text = text.substr(bracket + 2);
    } else {
        const std::size_t colon = text.rfind(':');
        if (colon == std::string_view::npos || text.substr(0, colon).find(':') != std::string_view::npos) {
            return std::unexpected(
                    make_error(AiServerConfigErrorCode::InvalidValue, line, key, "expected IP:port or [IPv6]:port"));
        }
        host = text.substr(0, colon);
        port_text = text.substr(colon + 1);
    }

    net::IpAddress address;
    std::uint16_t port = 0;
    if (!net::IpAddress::parse(host, address) || address.is_unspecified() || address.is_multicast() ||
        !parse_port(port_text, false, port)) {
        return std::unexpected(make_error(AiServerConfigErrorCode::InvalidValue, line, key,
                                          "expected a specified unicast IP and non-zero port"));
    }
    return std::pair(address, port);
}

std::expected<void, AiServerConfigError> parse_cat_endpoints(std::string_view value, std::size_t line,
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
            return std::unexpected(make_error(AiServerConfigErrorCode::InvalidValue, line, key,
                                              "empty endpoint in comma-separated list"));
        }
    }
    return {};
}

bool parse_milliseconds(std::string_view text, std::chrono::milliseconds &out) noexcept {
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::chrono::milliseconds::rep>::max());
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size() || value > kMax) {
        return false;
    }
    out = std::chrono::milliseconds(value);
    return true;
}

std::expected<std::vector<net::IpAddress>, AiServerConfigError> parse_server_addresses(std::string_view value,
                                                                                       std::size_t line) {
    std::vector<net::IpAddress> addresses;
    while (true) {
        const std::size_t separator = value.find(',');
        const std::string_view item = trim(separator == std::string_view::npos ? value : value.substr(0, separator));
        net::IpAddress address;
        if (!net::IpAddress::parse(item, address)) {
            return std::unexpected(make_error(AiServerConfigErrorCode::InvalidValue, line, kNacosServerAddressesKey,
                                              "expected comma-separated IP literals"));
        }
        addresses.push_back(address);

        if (separator == std::string_view::npos) {
            break;
        }
        value.remove_prefix(separator + 1);
    }
    return addresses;
}

std::expected<void, AiServerConfigError>
apply_entry(const EnvEntry &entry, net::IpAddress &listen_ip, std::uint16_t &listen_port,
            std::chrono::milliseconds &initial_config_timeout, std::optional<net::IpAddress> &advertise_address,
            std::string &service_name, std::string &service_group, cat::CatClientConfigParams &cat_params,
            bool &cat_setting_present, nacos::NacosClientConfigParams &nacos_params, FieldLines &field_lines,
            std::string &logging_config_path) {
    if (entry.key == kListenAddressKey) {
        if (!net::IpAddress::parse(entry.value, listen_ip)) {
            return std::unexpected(make_error(AiServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                              "expected an IPv4 or IPv6 literal"));
        }
        return {};
    }
    if (entry.key == kInitialConfigTimeoutKey) {
        if (!parse_milliseconds(entry.value, initial_config_timeout)) {
            return std::unexpected(make_error(AiServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                              "expected a non-negative millisecond duration"));
        }
        return {};
    }
    if (entry.key == kAdvertiseAddressKey) {
        net::IpAddress address;
        if (!net::IpAddress::parse(entry.value, address) || !address.is_v4() || address.is_unspecified() ||
            address.is_multicast()) {
            return std::unexpected(make_error(AiServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                              "expected a specified unicast IPv4 literal"));
        }
        advertise_address = address;
        return {};
    }
    if (entry.key == kServiceNameKey || entry.key == kServiceGroupKey) {
        if (entry.value.empty() || entry.value.size() > 255) {
            return std::unexpected(make_error(AiServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                              "expected a non-empty value of at most 255 bytes"));
        }
        if (entry.key == kServiceNameKey) {
            service_name = entry.value;
        } else {
            service_group = entry.value;
        }
        return {};
    }
    if (entry.key == kListenPortKey) {
        if (!parse_port(entry.value, true, listen_port)) {
            return std::unexpected(make_error(AiServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                              "expected a port from 0 through 65535"));
        }
        return {};
    }
    if (entry.key == kNacosServerAddressesKey) {
        auto addresses = parse_server_addresses(entry.value, entry.line);
        if (!addresses) {
            return std::unexpected(std::move(addresses.error()));
        }
        nacos_params.server_ips = std::move(*addresses);
        field_lines.server_addresses = entry.line;
        return {};
    }
    if (entry.key == kNacosHttpPortKey) {
        if (!parse_port(entry.value, false, nacos_params.http_port)) {
            return std::unexpected(make_error(AiServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                              "expected a port from 1 through 65535"));
        }
        field_lines.http_port = entry.line;
        return {};
    }
    if (entry.key == kNacosGrpcPortKey) {
        if (!parse_port(entry.value, false, nacos_params.grpc_port)) {
            return std::unexpected(make_error(AiServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                              "expected a port from 1 through 65535"));
        }
        field_lines.grpc_port = entry.line;
        return {};
    }
    if (entry.key == kNacosNamespaceKey) {
        nacos_params.namespace_id = entry.value;
        return {};
    }
    if (entry.key == kNacosTenantKey) {
        nacos_params.tenant = entry.value;
        return {};
    }
    if (entry.key == kNacosUsernameKey) {
        nacos_params.username = entry.value;
        field_lines.username = entry.line;
        return {};
    }
    if (entry.key == kNacosPasswordKey) {
        nacos_params.password = entry.value;
        field_lines.password = entry.line;
        return {};
    }
    if (entry.key == kNacosContextPathKey) {
        nacos_params.context_path = entry.value;
        field_lines.context_path = entry.line;
        return {};
    }
    if (entry.key == kNacosClientVersionKey) {
        nacos_params.client_version = entry.value;
        return {};
    }
    if (entry.key == kCatAppKey) {
        cat_params.app_key = entry.value;
        field_lines.cat_app_key = entry.line;
        cat_setting_present = cat_setting_present || !entry.value.empty();
        return {};
    }
    if (entry.key == kCatHostnameKey) {
        cat_params.hostname = entry.value;
        field_lines.cat_hostname = entry.line;
        cat_setting_present = cat_setting_present || !entry.value.empty();
        return {};
    }
    if (entry.key == kCatIpKey) {
        net::IpAddress address;
        if (!entry.value.empty() &&
            (!net::IpAddress::parse(entry.value, address) || address.is_unspecified() || address.is_multicast())) {
            return std::unexpected(make_error(AiServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                              "expected a specified unicast IP literal"));
        }
        cat_params.ip = entry.value;
        field_lines.cat_ip = entry.line;
        cat_setting_present = cat_setting_present || !entry.value.empty();
        return {};
    }
    if (entry.key == kCatRouterAddressesKey || entry.key == kCatCollectorAddressesKey) {
        auto parsed = parse_cat_endpoints(entry.value, entry.line, entry.key, entry.key == kCatRouterAddressesKey,
                                          cat_params);
        if (!parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
        if (entry.key == kCatRouterAddressesKey) {
            field_lines.cat_routers = entry.line;
        } else {
            field_lines.cat_collectors = entry.line;
        }
        cat_setting_present = cat_setting_present || !entry.value.empty();
        return {};
    }
    if (entry.key == kLogConfigPathKey) {
        if (entry.value.empty() || entry.value.size() > 4096 || entry.value.find('\0') != std::string::npos) {
            return std::unexpected(make_error(AiServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                              "expected a non-empty NUL-free path of at most 4096 bytes"));
        }
        logging_config_path = entry.value;
        return {};
    }

    return std::unexpected(
            make_error(AiServerConfigErrorCode::UnknownKey, entry.line, entry.key, "unknown ai-server setting"));
}

AiServerConfigError from_nacos_error(const nacos::NacosConfigError &error, const FieldLines &lines) {
    switch (error.code) {
        case nacos::NacosConfigErrorCode::EmptyServerList:
            return make_error(AiServerConfigErrorCode::MissingRequiredKey, 0, kNacosServerAddressesKey,
                              "required setting is missing");
        case nacos::NacosConfigErrorCode::InvalidServerAddress:
            return make_error(AiServerConfigErrorCode::InvalidNacosConfig, lines.server_addresses,
                              kNacosServerAddressesKey, "Nacos server IP must be unicast and specified");
        case nacos::NacosConfigErrorCode::InvalidHttpPort:
            return make_error(AiServerConfigErrorCode::InvalidNacosConfig, lines.http_port, kNacosHttpPortKey,
                              "invalid Nacos HTTP port");
        case nacos::NacosConfigErrorCode::InvalidGrpcPort:
            return make_error(AiServerConfigErrorCode::InvalidNacosConfig, lines.grpc_port, kNacosGrpcPortKey,
                              "invalid Nacos gRPC port");
        case nacos::NacosConfigErrorCode::EmptyUsername:
            return make_error(AiServerConfigErrorCode::InvalidNacosConfig, lines.password, kNacosUsernameKey,
                              "username and password must both be empty or both be set");
        case nacos::NacosConfigErrorCode::EmptyPassword:
            return make_error(AiServerConfigErrorCode::InvalidNacosConfig, lines.username, kNacosPasswordKey,
                              "username and password must both be empty or both be set");
        case nacos::NacosConfigErrorCode::InvalidContextPath:
            return make_error(AiServerConfigErrorCode::InvalidNacosConfig, lines.context_path, kNacosContextPathKey,
                              "context path must be absolute and contain no query or fragment");
    }
    return make_error(AiServerConfigErrorCode::InvalidNacosConfig, 0, {}, "invalid Nacos configuration");
}

AiServerConfigError from_cat_error(cat::CatConfigError error, const FieldLines &lines) {
    switch (error) {
        case cat::CatConfigError::EmptyAppKey:
            return make_error(AiServerConfigErrorCode::MissingRequiredKey, lines.cat_app_key, kCatAppKey,
                              "CAT_APP_KEY is required when CAT is configured");
        case cat::CatConfigError::EmptyHostname:
            return make_error(AiServerConfigErrorCode::MissingRequiredKey, lines.cat_hostname, kCatHostnameKey,
                              "CAT_HOSTNAME is required when CAT is configured");
        case cat::CatConfigError::EmptyIp:
            return make_error(AiServerConfigErrorCode::MissingRequiredKey, lines.cat_ip, kCatIpKey,
                              "CAT_IP is required when CAT is configured");
        case cat::CatConfigError::EmptyServerList:
            return make_error(AiServerConfigErrorCode::MissingRequiredKey,
                              lines.cat_routers != 0 ? lines.cat_routers : lines.cat_collectors, kCatRouterAddressesKey,
                              "at least one CAT router or collector is required");
        case cat::CatConfigError::InvalidRouter:
            return make_error(AiServerConfigErrorCode::InvalidCatConfig, lines.cat_routers, kCatRouterAddressesKey,
                              "invalid CAT router endpoint");
        case cat::CatConfigError::InvalidCollector:
            return make_error(AiServerConfigErrorCode::InvalidCatConfig, lines.cat_collectors,
                              kCatCollectorAddressesKey, "invalid CAT collector endpoint");
    }
    return make_error(AiServerConfigErrorCode::InvalidCatConfig, 0, {}, "invalid CAT configuration");
}

} // namespace

AiServerConfig::AiServerConfig(net::SocketAddress listen_address, nacos::NacosClientConfig nacos_config,
                               std::chrono::milliseconds initial_config_timeout,
                               std::optional<net::IpAddress> advertise_address, std::string service_name,
                               std::string service_group, std::optional<cat::CatClientConfig> cat_config,
                               std::string logging_config_path) noexcept :
    listen_address_(std::move(listen_address)), nacos_config_(std::move(nacos_config)),
    initial_config_timeout_(initial_config_timeout), advertise_address_(std::move(advertise_address)),
    service_name_(std::move(service_name)), service_group_(std::move(service_group)),
    cat_config_(std::move(cat_config)), logging_config_path_(std::move(logging_config_path)) {}

std::expected<AiServerConfig, AiServerConfigError> AiServerConfig::load_from_file(std::string_view path) {
    std::ifstream input(std::string(path), std::ios::binary);
    if (!input) {
        return std::unexpected(
                make_error(AiServerConfigErrorCode::OpenFailed, 0, {}, "failed to open configuration file"));
    }

    std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) {
        return std::unexpected(
                make_error(AiServerConfigErrorCode::ReadFailed, 0, {}, "failed to read configuration file"));
    }
    auto config = load_from_string(contents);
    if (!config) {
        return config;
    }
    auto logging_config_path = resolve_config_reference(path, config->logging_config_path_);
    if (!logging_config_path) {
        return std::unexpected(std::move(logging_config_path.error()));
    }
    if (logging_config_path->size() > 4096) {
        return std::unexpected(make_error(AiServerConfigErrorCode::InvalidValue, 0, kLogConfigPathKey,
                                          "resolved logging configuration path exceeds 4096 bytes"));
    }
    config->logging_config_path_ = std::move(*logging_config_path);
    return config;
}

std::expected<AiServerConfig, AiServerConfigError> AiServerConfig::load_from_string(std::string_view input) {
    auto entries = parse_entries(input);
    if (!entries) {
        return std::unexpected(std::move(entries.error()));
    }

    net::IpAddress listen_ip = net::IpAddress::any_v4();
    std::uint16_t listen_port = 8080;
    std::chrono::milliseconds initial_config_timeout{60000};
    std::optional<net::IpAddress> advertise_address;
    std::string service_name = "ploto-ai-server";
    std::string service_group = "DEFAULT_GROUP";
    cat::CatClientConfigParams cat_params{
            .thread_group_name = "ai-server-cat",
            .thread_id = "0",
            .thread_name = "cat-sender",
    };
    bool cat_setting_present = false;
    nacos::NacosClientConfigParams nacos_params;
    FieldLines field_lines;
    std::string logging_config_path;
    for (const EnvEntry &entry: *entries) {
        auto result = apply_entry(entry, listen_ip, listen_port, initial_config_timeout, advertise_address,
                                  service_name, service_group, cat_params, cat_setting_present, nacos_params,
                                  field_lines, logging_config_path);
        if (!result) {
            return std::unexpected(std::move(result.error()));
        }
    }
    if (logging_config_path.empty()) {
        return std::unexpected(make_error(AiServerConfigErrorCode::MissingRequiredKey, 0, kLogConfigPathKey,
                                          "required setting is missing"));
    }

    auto nacos_config = nacos::NacosClientConfig::create(std::move(nacos_params));
    if (!nacos_config) {
        return std::unexpected(from_nacos_error(nacos_config.error(), field_lines));
    }

    if (!advertise_address && listen_ip.is_v4() && !listen_ip.is_unspecified() && !listen_ip.is_multicast()) {
        advertise_address = listen_ip;
    }
    std::optional<cat::CatClientConfig> cat_config;
    if (cat_setting_present) {
        auto created = cat::CatClientConfig::create(std::move(cat_params));
        if (!created) {
            return std::unexpected(from_cat_error(created.error(), field_lines));
        }
        cat_config = std::move(*created);
    }
    return AiServerConfig(net::SocketAddress(listen_ip, listen_port), std::move(*nacos_config), initial_config_timeout,
                          std::move(advertise_address), std::move(service_name), std::move(service_group),
                          std::move(cat_config), std::move(logging_config_path));
}

} // namespace fiber::ai_server
