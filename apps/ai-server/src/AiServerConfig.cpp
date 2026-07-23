#include "AiServerConfig.h"

#include <array>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

namespace fiber::ai_server {
namespace {

constexpr std::string_view kListenAddressKey = "AI_SERVER_LISTEN_ADDRESS";
constexpr std::string_view kListenPortKey = "AI_SERVER_LISTEN_PORT";
constexpr std::string_view kNacosServerAddressesKey = "NACOS_SERVER_ADDRESSES";
constexpr std::string_view kNacosHttpPortKey = "NACOS_HTTP_PORT";
constexpr std::string_view kNacosGrpcPortKey = "NACOS_GRPC_PORT";
constexpr std::string_view kNacosNamespaceKey = "NACOS_NAMESPACE_ID";
constexpr std::string_view kNacosTenantKey = "NACOS_TENANT";
constexpr std::string_view kNacosUsernameKey = "NACOS_USERNAME";
constexpr std::string_view kNacosPasswordKey = "NACOS_PASSWORD";
constexpr std::string_view kNacosContextPathKey = "NACOS_CONTEXT_PATH";
constexpr std::string_view kNacosClientVersionKey = "NACOS_CLIENT_VERSION";

constexpr std::array<std::string_view, 11> kKnownKeys = {
        kListenAddressKey, kListenPortKey,       kNacosServerAddressesKey, kNacosHttpPortKey,
        kNacosGrpcPortKey, kNacosNamespaceKey,   kNacosTenantKey,          kNacosUsernameKey,
        kNacosPasswordKey, kNacosContextPathKey, kNacosClientVersionKey,
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
};

AiServerConfigError make_error(AiServerConfigErrorCode code, std::size_t line, std::string_view key,
                               std::string detail) {
    return {.code = code, .line = line, .key = std::string(key), .detail = std::move(detail)};
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

std::expected<void, AiServerConfigError> apply_entry(const EnvEntry &entry, net::IpAddress &listen_ip,
                                                     std::uint16_t &listen_port,
                                                     nacos::NacosClientConfigParams &nacos_params,
                                                     FieldLines &field_lines) {
    if (entry.key == kListenAddressKey) {
        if (!net::IpAddress::parse(entry.value, listen_ip)) {
            return std::unexpected(make_error(AiServerConfigErrorCode::InvalidValue, entry.line, entry.key,
                                              "expected an IPv4 or IPv6 literal"));
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

} // namespace

AiServerConfig::AiServerConfig(net::SocketAddress listen_address, nacos::NacosClientConfig nacos_config) noexcept :
    listen_address_(std::move(listen_address)), nacos_config_(std::move(nacos_config)) {}

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
    return load_from_string(contents);
}

std::expected<AiServerConfig, AiServerConfigError> AiServerConfig::load_from_string(std::string_view input) {
    auto entries = parse_entries(input);
    if (!entries) {
        return std::unexpected(std::move(entries.error()));
    }

    net::IpAddress listen_ip = net::IpAddress::any_v4();
    std::uint16_t listen_port = 8080;
    nacos::NacosClientConfigParams nacos_params;
    FieldLines field_lines;
    for (const EnvEntry &entry: *entries) {
        auto result = apply_entry(entry, listen_ip, listen_port, nacos_params, field_lines);
        if (!result) {
            return std::unexpected(std::move(result.error()));
        }
    }

    auto nacos_config = nacos::NacosClientConfig::create(std::move(nacos_params));
    if (!nacos_config) {
        return std::unexpected(from_nacos_error(nacos_config.error(), field_lines));
    }

    return AiServerConfig(net::SocketAddress(listen_ip, listen_port), std::move(*nacos_config));
}

} // namespace fiber::ai_server
