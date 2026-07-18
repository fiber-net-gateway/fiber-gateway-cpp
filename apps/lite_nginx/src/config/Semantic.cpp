#include "Semantic.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <string_view>
#include <unordered_set>

#include "PathResolve.h"
#include "log/LogConfig.h"

namespace fiber::lite_nginx::config {
namespace {

struct HostPort {
    std::string host;
    std::uint16_t port = 0;
};

// Strips an optional "http://" / "https://" scheme prefix from an upstream server address. When
// present, tls is set from the scheme (https:// -> true). Absent prefix defaults to http (tls=false).
// "http://" may be omitted entirely, matching nginx's per-server scheme syntax.
struct StrippedScheme {
    std::string_view rest;
    bool tls = false;
};

std::optional<StrippedScheme> strip_server_scheme(std::string_view value) noexcept {
    constexpr std::string_view kHttp = "http://";
    constexpr std::string_view kHttps = "https://";
    auto icase_prefix = [](std::string_view text, std::string_view prefix) noexcept {
        if (text.size() < prefix.size()) {
            return false;
        }
        for (std::size_t i = 0; i < prefix.size(); ++i) {
            char c = text[i];
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
            if (c != prefix[i]) {
                return false;
            }
        }
        return true;
    };
    if (icase_prefix(value, kHttps)) {
        return StrippedScheme{value.substr(kHttps.size()), true};
    }
    if (icase_prefix(value, kHttp)) {
        return StrippedScheme{value.substr(kHttp.size()), false};
    }
    return StrippedScheme{value, false};
}

struct ProxySettingsBuilder {
    std::optional<std::chrono::milliseconds> connect_timeout;
    std::optional<std::chrono::milliseconds> read_timeout;
    std::optional<std::chrono::milliseconds> send_timeout;
    std::vector<HeaderOverride> set_headers;
    bool proxy_buffering = false;
};

ConfigError make_error(const DirectiveNode &directive, std::string message) {
    return ConfigError{
            .message = std::move(message),
            .location = directive.location,
    };
}

std::string to_lowercase(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (char ch: value) {
        if (ch >= 'A' && ch <= 'Z') {
            lowered.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else {
            lowered.push_back(ch);
        }
    }
    return lowered;
}

bool contains_variable(std::string_view value) { return value.find('$') != std::string_view::npos; }

std::expected<std::size_t, ConfigError> parse_positive_size(const DirectiveNode &directive, std::string_view value,
                                                            const char *field_name) {
    if (value.empty()) {
        return std::unexpected(make_error(directive, std::string(field_name) + " must not be empty"));
    }

    std::size_t parsed = 0;
    auto begin = value.data();
    auto end = value.data() + value.size();
    auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc() || result.ptr != end || parsed == 0) {
        return std::unexpected(make_error(directive, std::string(field_name) + " must be a positive integer"));
    }
    return parsed;
}

std::expected<std::uint16_t, ConfigError> parse_port(const DirectiveNode &directive, std::string_view value,
                                                     const char *field_name) {
    auto parsed = parse_positive_size(directive, value, field_name);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    if (*parsed > 65535) {
        return std::unexpected(make_error(directive, std::string(field_name) + " must be in range 1..65535"));
    }
    return static_cast<std::uint16_t>(*parsed);
}

std::expected<std::chrono::milliseconds, ConfigError> parse_duration(const DirectiveNode &directive,
                                                                     std::string_view value, const char *field_name) {
    if (value.empty()) {
        return std::unexpected(make_error(directive, std::string(field_name) + " must not be empty"));
    }

    std::size_t unit_offset = value.size();
    while (unit_offset > 0 && value[unit_offset - 1] >= 'A' && value[unit_offset - 1] <= 'z') {
        --unit_offset;
    }

    std::string_view number_part = value.substr(0, unit_offset);
    std::string_view unit_part = value.substr(unit_offset);
    if (number_part.empty()) {
        return std::unexpected(make_error(directive, std::string(field_name) + " has invalid duration"));
    }

    std::size_t base = 0;
    auto parsed = std::from_chars(number_part.data(), number_part.data() + number_part.size(), base);
    if (parsed.ec != std::errc() || parsed.ptr != number_part.data() + number_part.size()) {
        return std::unexpected(make_error(directive, std::string(field_name) + " has invalid duration"));
    }

    std::uint64_t multiplier = 1000;
    if (unit_part.empty() || unit_part == "s") {
        multiplier = 1000;
    } else if (unit_part == "ms") {
        multiplier = 1;
    } else if (unit_part == "m") {
        multiplier = 60 * 1000;
    } else if (unit_part == "h") {
        multiplier = 60 * 60 * 1000;
    } else {
        return std::unexpected(make_error(directive, std::string(field_name) + " has unsupported duration unit"));
    }

    return std::chrono::milliseconds(base * multiplier);
}

std::expected<std::size_t, ConfigError> parse_size(const DirectiveNode &directive, std::string_view value,
                                                   const char *field_name) {
    if (value.empty()) {
        return std::unexpected(make_error(directive, std::string(field_name) + " must not be empty"));
    }

    std::size_t multiplier = 1;
    const char suffix = value.back();
    if (suffix == 'k' || suffix == 'K') {
        multiplier = 1024;
        value.remove_suffix(1);
    } else if (suffix == 'm' || suffix == 'M') {
        multiplier = 1024 * 1024;
        value.remove_suffix(1);
    }
    if (value.empty()) {
        return std::unexpected(make_error(directive, std::string(field_name) + " has invalid size"));
    }

    std::size_t parsed = 0;
    auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc() || result.ptr != value.data() + value.size() ||
        parsed > std::numeric_limits<std::size_t>::max() / multiplier) {
        return std::unexpected(make_error(directive, std::string(field_name) + " has invalid size"));
    }
    return parsed * multiplier;
}

std::expected<unsigned, ConfigError> parse_non_negative_unsigned(const DirectiveNode &directive, std::string_view value,
                                                                 const char *field_name) {
    unsigned parsed = 0;
    auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (value.empty() || result.ec != std::errc() || result.ptr != value.data() + value.size()) {
        return std::unexpected(make_error(directive, std::string(field_name) + " must be a non-negative integer"));
    }
    return parsed;
}

std::expected<bool, ConfigError> parse_on_off(const DirectiveNode &directive, const char *field_name) {
    if (directive.has_block || directive.args.size() != 1 ||
        (directive.args[0] != "on" && directive.args[0] != "off")) {
        return std::unexpected(make_error(directive, std::string(field_name) + " expects 'on' or 'off'"));
    }
    return directive.args[0] == "on";
}

std::expected<AccessLogConfig, ConfigError> parse_access_log(const DirectiveNode &directive) {
    if (directive.has_block) {
        return std::unexpected(make_error(directive, "access_log must not be a block"));
    }
    if (directive.args.size() == 1 && directive.args[0] == "off") {
        return AccessLogConfig{
                .location = directive.location,
                .kind = AccessLogKind::Off,
        };
    }
    if (directive.args.size() != 2) {
        return std::unexpected(make_error(directive, "access_log expects '<logger-name> <message-template>' or 'off'"));
    }
    if (!fiber::log::valid_logger_name(directive.args[0])) {
        return std::unexpected(make_error(directive, "access_log logger name is invalid"));
    }
    return AccessLogConfig{
            .location = directive.location,
            .kind = AccessLogKind::Template,
            .logger_name = directive.args[0],
            .message_template = directive.args[1],
    };
}

std::expected<LoggingLevel, ConfigError> parse_logging_level(const DirectiveNode &directive, std::string_view value,
                                                             const char *field_name) {
    if (value == "trace") {
        return LoggingLevel::Trace;
    }
    if (value == "debug") {
        return LoggingLevel::Debug;
    }
    if (value == "info") {
        return LoggingLevel::Info;
    }
    if (value == "warn" || value == "warning") {
        return LoggingLevel::Warn;
    }
    if (value == "error") {
        return LoggingLevel::Error;
    }
    if (value == "fatal") {
        return LoggingLevel::Fatal;
    }
    return std::unexpected(
            make_error(directive, std::string(field_name) + " has invalid log level: " + std::string(value)));
}

bool valid_log_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 255 || name.front() == '.' || name.back() == '.') {
        return false;
    }
    bool previous_dot = false;
    for (char ch: name) {
        if (ch == '.') {
            if (previous_dot) {
                return false;
            }
            previous_dot = true;
            continue;
        }
        previous_dot = false;
        const bool alpha = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        const bool digit = ch >= '0' && ch <= '9';
        if (!alpha && !digit && ch != '_' && ch != '-') {
            return false;
        }
    }
    return true;
}

std::expected<HostPort, ConfigError> parse_host_port(const DirectiveNode &directive, std::string_view value,
                                                     const char *field_name) {
    if (value.empty()) {
        return std::unexpected(make_error(directive, std::string(field_name) + " must not be empty"));
    }

    HostPort host_port;
    if (value.front() == '[') {
        std::size_t close = value.find(']');
        if (close == std::string_view::npos || close + 1 >= value.size() || value[close + 1] != ':') {
            return std::unexpected(make_error(directive, std::string(field_name) + " must use [host]:port form"));
        }
        host_port.host = std::string(value.substr(1, close - 1));
        auto port_result = parse_port(directive, value.substr(close + 2), field_name);
        if (!port_result) {
            return std::unexpected(port_result.error());
        }
        host_port.port = *port_result;
        return host_port;
    }

    std::size_t colon = value.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= value.size()) {
        return std::unexpected(make_error(directive, std::string(field_name) + " must use host:port form"));
    }
    host_port.host = std::string(value.substr(0, colon));
    auto port_result = parse_port(directive, value.substr(colon + 1), field_name);
    if (!port_result) {
        return std::unexpected(port_result.error());
    }
    host_port.port = *port_result;
    return host_port;
}

std::expected<ListenAddress, ConfigError> parse_listen_address(const DirectiveNode &directive) {
    if (directive.args.empty() || directive.args.size() > 3) {
        return std::unexpected(make_error(directive, "listen expects '<port|ip:port> [ssl] [http3]'"));
    }

    for (const auto &arg: directive.args) {
        if (contains_variable(arg)) {
            return std::unexpected(make_error(directive, "listen does not support variables in V1"));
        }
    }

    ListenAddress listen;
    listen.location = directive.location;
    bool seen_ssl = false;
    bool seen_http3 = false;
    for (std::size_t i = 1; i < directive.args.size(); ++i) {
        const std::string &arg = directive.args[i];
        if (arg == "ssl") {
            if (seen_ssl) {
                return std::unexpected(make_error(directive, "listen ssl flag must not be repeated"));
            }
            seen_ssl = true;
            listen.tls = true;
            continue;
        }
        if (arg == "http3" || arg == "quic") {
            if (seen_http3) {
                return std::unexpected(make_error(directive, "listen http3 flag must not be repeated"));
            }
            seen_http3 = true;
            listen.http3 = true;
            continue;
        }
        return std::unexpected(make_error(directive, "listen only supports optional 'ssl' and 'http3' flags"));
    }
    if (listen.http3 && !listen.tls) {
        return std::unexpected(make_error(directive, "listen http3 requires ssl"));
    }

    std::string_view value = directive.args[0];
    if (value.find(':') == std::string_view::npos && !value.empty() && value.front() != '[') {
        auto port_result = parse_port(directive, value, "listen");
        if (!port_result) {
            return std::unexpected(port_result.error());
        }
        listen.host.clear();
        listen.port = *port_result;
        listen.has_host = false;
        return listen;
    }

    auto host_port_result = parse_host_port(directive, value, "listen");
    if (!host_port_result) {
        return std::unexpected(host_port_result.error());
    }
    listen.host = std::move(host_port_result->host);
    listen.port = host_port_result->port;
    listen.has_host = true;
    return listen;
}

void upsert_header(std::vector<HeaderOverride> &headers, HeaderOverride header) {
    for (auto &existing: headers) {
        if (existing.lowercase_name == header.lowercase_name) {
            existing = std::move(header);
            return;
        }
    }
    headers.push_back(std::move(header));
}

ProxySettings finalize_proxy_settings(const ProxySettingsBuilder &builder) {
    ProxySettings settings;
    settings.connect_timeout = builder.connect_timeout;
    settings.read_timeout = builder.read_timeout;
    settings.send_timeout = builder.send_timeout;
    settings.set_headers = builder.set_headers;
    settings.proxy_buffering = builder.proxy_buffering;
    return settings;
}

ProxySettings merge_proxy_settings(const ProxySettingsBuilder &base, const ProxySettingsBuilder &overrides) {
    ProxySettings settings;
    settings.connect_timeout = overrides.connect_timeout ? overrides.connect_timeout : base.connect_timeout;
    settings.read_timeout = overrides.read_timeout ? overrides.read_timeout : base.read_timeout;
    settings.send_timeout = overrides.send_timeout ? overrides.send_timeout : base.send_timeout;
    settings.proxy_buffering = overrides.proxy_buffering;

    settings.set_headers = base.set_headers;
    for (const auto &header: overrides.set_headers) {
        upsert_header(settings.set_headers, header);
    }
    return settings;
}

bool has_tls_identity(const ServerConfig &server) {
    return !server.certificate.empty() || !server.certificate_key.empty();
}

std::expected<HeaderOverride, ConfigError> parse_proxy_set_header(const DirectiveNode &directive) {
    if (directive.args.size() != 2) {
        return std::unexpected(make_error(directive, "proxy_set_header expects exactly two arguments"));
    }
    if (contains_variable(directive.args[0])) {
        return std::unexpected(make_error(directive, "proxy_set_header name must not contain variables"));
    }
    // The value may be a template: any ${...} is compiled at runtime-build and evaluated per
    // request (route vars like $header.host resolve via the location's RouteScriptLibrary). A
    // bare $ without ${ is treated as a literal (no interpolation).
    const std::string &value = directive.args[1];
    const bool is_template = value.find("${") != std::string::npos;

    return HeaderOverride{
            .name = directive.args[0],
            .lowercase_name = to_lowercase(directive.args[0]),
            .value = value,
            .is_template = is_template,
    };
}

std::expected<void, ConfigError> apply_proxy_timeout(ProxySettingsBuilder &builder, const DirectiveNode &directive) {
    if (directive.args.size() != 1) {
        return std::unexpected(make_error(directive, directive.name + " expects exactly one argument"));
    }
    if (contains_variable(directive.args[0])) {
        return std::unexpected(make_error(directive, directive.name + " does not support variables in V1"));
    }

    auto duration = parse_duration(directive, directive.args[0], directive.name.c_str());
    if (!duration) {
        return std::unexpected(duration.error());
    }

    if (directive.name == "proxy_connect_timeout") {
        builder.connect_timeout = *duration;
    } else if (directive.name == "proxy_read_timeout") {
        builder.read_timeout = *duration;
    } else if (directive.name == "proxy_send_timeout") {
        builder.send_timeout = *duration;
    } else {
        return std::unexpected(make_error(directive, "unsupported proxy timeout directive"));
    }
    return {};
}

std::expected<ProxyPassTarget, ConfigError> parse_proxy_pass(const DirectiveNode &directive) {
    if (!directive.has_block && directive.args.size() != 1) {
        return std::unexpected(make_error(directive, "proxy_pass expects exactly one argument"));
    }
    if (contains_variable(directive.args[0])) {
        return std::unexpected(make_error(directive, "proxy_pass does not support variables in V1"));
    }
    if (!directive.args[0].starts_with("http://")) {
        return std::unexpected(make_error(directive, "proxy_pass only supports http:// targets in V1"));
    }

    std::string_view target = directive.args[0];
    target.remove_prefix(7);
    if (target.empty()) {
        return std::unexpected(make_error(directive, "proxy_pass target must not be empty"));
    }
    if (target.find('/') != std::string_view::npos || target.find('?') != std::string_view::npos ||
        target.find('#') != std::string_view::npos) {
        return std::unexpected(make_error(directive, "proxy_pass path rewriting is not supported in V1"));
    }

    auto host_port_result = parse_host_port(directive, target, "proxy_pass");
    if (host_port_result) {
        ProxyPassTarget proxy_pass;
        proxy_pass.kind = ProxyPassKind::Direct;
        proxy_pass.raw = directive.args[0];
        proxy_pass.host = std::move(host_port_result->host);
        proxy_pass.port = host_port_result->port;
        proxy_pass.location = directive.location;
        return proxy_pass;
    }

    if (target.find(':') != std::string_view::npos) {
        return std::unexpected(host_port_result.error());
    }

    ProxyPassTarget proxy_pass;
    proxy_pass.kind = ProxyPassKind::NamedUpstream;
    proxy_pass.raw = directive.args[0];
    proxy_pass.upstream_name = std::string(target);
    proxy_pass.location = directive.location;
    return proxy_pass;
}

std::expected<UpstreamConfig, ConfigError> parse_upstream(const DirectiveNode &directive) {
    if (!directive.has_block) {
        return std::unexpected(make_error(directive, "upstream must be a block"));
    }
    if (directive.args.size() != 1) {
        return std::unexpected(make_error(directive, "upstream expects exactly one name argument"));
    }
    if (contains_variable(directive.args[0])) {
        return std::unexpected(make_error(directive, "upstream name must be static"));
    }

    UpstreamConfig upstream;
    upstream.name = directive.args[0];

    for (const auto &child: directive.children) {
        if (child.has_block) {
            return std::unexpected(make_error(child, "nested blocks are not allowed inside upstream"));
        }

        if (child.name == "server") {
            if (child.args.empty()) {
                return std::unexpected(make_error(child, "upstream server expects a host:port argument"));
            }
            if (contains_variable(child.args[0])) {
                return std::unexpected(make_error(child, "upstream server does not support variables in V1"));
            }
            auto stripped = strip_server_scheme(child.args[0]);
            if (!stripped) {
                return std::unexpected(make_error(child, "upstream server has an invalid scheme prefix"));
            }
            auto host_port = parse_host_port(child, stripped->rest, "upstream server");
            if (!host_port) {
                return std::unexpected(host_port.error());
            }
            UpstreamServerConfig server;
            server.host = std::move(host_port->host);
            server.port = host_port->port;
            server.tls = stripped->tls;
            // server parameters are name=value triplets; the lexer splits '=' into its own token,
            // so args look like [host:port, "weight", "=", "3", ...].
            for (std::size_t i = 1; i < child.args.size(); ++i) {
                const std::string &param = child.args[i];
                if (param == "=") {
                    return std::unexpected(make_error(child, "server parameter missing a name"));
                }
                if (i + 2 >= child.args.size() || child.args[i + 1] != "=") {
                    return std::unexpected(make_error(child, "server parameter must be name=value"));
                }
                const std::string &value = child.args[i + 2];
                i += 2; // consume '=' and value (loop's ++ advances past the triplet)
                if (param == "weight") {
                    std::uint32_t w = 0;
                    auto r = std::from_chars(value.data(), value.data() + value.size(), w);
                    if (r.ec != std::errc() || r.ptr != value.data() + value.size() || w == 0) {
                        return std::unexpected(make_error(child, "server weight must be a positive integer"));
                    }
                    server.weight = w;
                } else {
                    return std::unexpected(make_error(child, "unsupported server parameter: " + param));
                }
            }
            upstream.servers.push_back(std::move(server));
            continue;
        }

        if (child.name == "connect_timeout") {
            if (child.args.size() != 1) {
                return std::unexpected(make_error(child, "connect_timeout expects exactly one argument"));
            }
            auto duration = parse_duration(child, child.args[0], "connect_timeout");
            if (!duration) {
                return std::unexpected(duration.error());
            }
            upstream.connect_timeout = *duration;
            continue;
        }
        if (child.name == "read_timeout") {
            if (child.args.size() != 1) {
                return std::unexpected(make_error(child, "read_timeout expects exactly one argument"));
            }
            auto duration = parse_duration(child, child.args[0], "read_timeout");
            if (!duration) {
                return std::unexpected(duration.error());
            }
            upstream.read_timeout = *duration;
            continue;
        }
        if (child.name == "send_timeout") {
            if (child.args.size() != 1) {
                return std::unexpected(make_error(child, "send_timeout expects exactly one argument"));
            }
            auto duration = parse_duration(child, child.args[0], "send_timeout");
            if (!duration) {
                return std::unexpected(duration.error());
            }
            upstream.send_timeout = *duration;
            continue;
        }

        return std::unexpected(make_error(child, "unsupported directive in upstream block: " + child.name));
    }

    if (upstream.servers.empty()) {
        return std::unexpected(make_error(directive, "upstream must define at least one server"));
    }
    return upstream;
}

std::expected<LogAppenderConfig, ConfigError> parse_log_appender(const DirectiveNode &directive) {
    if (!directive.has_block || directive.args.size() != 1) {
        return std::unexpected(make_error(directive, "appender expects one name and a block"));
    }
    if (!valid_log_name(directive.args[0])) {
        return std::unexpected(make_error(directive, "appender has an invalid name"));
    }

    LogAppenderConfig appender;
    appender.location = directive.location;
    appender.name = directive.args[0];
    bool seen_type = false;
    bool seen_path = false;
    bool seen_mode = false;
    bool seen_buffer_size = false;
    bool seen_flush_interval = false;
    bool seen_min_level = false;
    bool seen_max_level = false;
    bool seen_stream = false;

    for (const auto &child: directive.children) {
        if (child.has_block || child.args.size() != 1) {
            return std::unexpected(make_error(child, child.name + " expects exactly one argument"));
        }
        const std::string &value = child.args[0];
        if (child.name == "type") {
            if (seen_type || (value != "file" && value != "console")) {
                return std::unexpected(make_error(child, "appender type must be specified once as file or console"));
            }
            appender.kind = value == "file" ? LogAppenderKind::File : LogAppenderKind::Console;
            seen_type = true;
            continue;
        }
        if (child.name == "path") {
            if (seen_path || value.empty() || contains_variable(value)) {
                return std::unexpected(make_error(child, "appender path must be a single non-empty static path"));
            }
            appender.path = resolve_config_path(child.location.source_name, value);
            seen_path = true;
            continue;
        }
        if (child.name == "mode") {
            if (seen_mode || value.empty()) {
                return std::unexpected(make_error(child, "appender mode must not be repeated"));
            }
            std::uint32_t mode = 0;
            auto parsed = std::from_chars(value.data(), value.data() + value.size(), mode, 8);
            if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() || mode > 0777) {
                return std::unexpected(make_error(child, "appender mode must be an octal value in range 0000..0777"));
            }
            appender.file_mode = mode;
            seen_mode = true;
            continue;
        }
        if (child.name == "buffer_size") {
            if (seen_buffer_size) {
                return std::unexpected(make_error(child, "buffer_size must not be repeated"));
            }
            auto size = parse_size(child, value, "buffer_size");
            if (!size) {
                return std::unexpected(size.error());
            }
            appender.buffer_size = *size;
            seen_buffer_size = true;
            continue;
        }
        if (child.name == "flush_interval") {
            if (seen_flush_interval) {
                return std::unexpected(make_error(child, "flush_interval must not be repeated"));
            }
            auto duration = parse_duration(child, value, "flush_interval");
            if (!duration) {
                return std::unexpected(duration.error());
            }
            appender.flush_interval = *duration;
            seen_flush_interval = true;
            continue;
        }
        if (child.name == "min_level" || child.name == "max_level") {
            bool &seen = child.name == "min_level" ? seen_min_level : seen_max_level;
            if (seen) {
                return std::unexpected(make_error(child, child.name + " must not be repeated"));
            }
            auto level = parse_logging_level(child, value, child.name.c_str());
            if (!level) {
                return std::unexpected(level.error());
            }
            if (child.name == "min_level") {
                appender.min_level = *level;
            } else {
                appender.max_level = *level;
            }
            seen = true;
            continue;
        }
        if (child.name == "stream") {
            if (seen_stream || (value != "stdout" && value != "stderr")) {
                return std::unexpected(make_error(child, "console stream must be specified once as stdout or stderr"));
            }
            appender.stream = value == "stdout" ? LogConsoleStream::Stdout : LogConsoleStream::Stderr;
            seen_stream = true;
            continue;
        }
        return std::unexpected(make_error(child, "unsupported directive in appender block: " + child.name));
    }

    if (!seen_type) {
        return std::unexpected(make_error(directive, "appender must define type"));
    }
    if (static_cast<unsigned char>(appender.min_level) > static_cast<unsigned char>(appender.max_level)) {
        return std::unexpected(make_error(directive, "appender min_level must not exceed max_level"));
    }
    if ((appender.buffer_size == 0) != (appender.flush_interval.count() == 0)) {
        return std::unexpected(
                make_error(directive, "buffer_size and flush_interval must both be zero or both be non-zero"));
    }
    if (appender.kind == LogAppenderKind::File) {
        if (!seen_path) {
            return std::unexpected(make_error(directive, "file appender must define path"));
        }
        if (seen_stream) {
            return std::unexpected(make_error(directive, "file appender does not support stream"));
        }
    } else if (seen_path || seen_mode || seen_buffer_size || seen_flush_interval) {
        return std::unexpected(make_error(directive, "console appender does not support file options"));
    }
    return appender;
}

std::expected<LoggerRuleConfig, ConfigError> parse_logger_rule(const DirectiveNode &directive) {
    if (!directive.has_block || directive.args.size() != 1 || !valid_log_name(directive.args[0])) {
        return std::unexpected(make_error(directive, "logger expects one valid name and a block"));
    }

    LoggerRuleConfig logger;
    logger.location = directive.location;
    logger.name = directive.args[0];
    bool seen_level = false;
    bool seen_verbosity = false;
    bool seen_additive = false;
    std::unordered_set<std::string> appender_names;
    for (const auto &child: directive.children) {
        if (child.has_block || child.args.size() != 1) {
            return std::unexpected(make_error(child, child.name + " expects exactly one argument"));
        }
        if (child.name == "level") {
            if (seen_level) {
                return std::unexpected(make_error(child, "logger level must not be repeated"));
            }
            auto level = parse_logging_level(child, child.args[0], "logger level");
            if (!level) {
                return std::unexpected(level.error());
            }
            logger.level = *level;
            seen_level = true;
            continue;
        }
        if (child.name == "verbosity") {
            if (seen_verbosity) {
                return std::unexpected(make_error(child, "logger verbosity must not be repeated"));
            }
            auto verbosity = parse_non_negative_unsigned(child, child.args[0], "logger verbosity");
            if (!verbosity) {
                return std::unexpected(verbosity.error());
            }
            logger.verbosity = *verbosity;
            seen_verbosity = true;
            continue;
        }
        if (child.name == "additive") {
            if (seen_additive) {
                return std::unexpected(make_error(child, "logger additive must not be repeated"));
            }
            auto additive = parse_on_off(child, "logger additive");
            if (!additive) {
                return std::unexpected(additive.error());
            }
            logger.additive = *additive;
            seen_additive = true;
            continue;
        }
        if (child.name == "appender") {
            if (!appender_names.insert(child.args[0]).second) {
                return std::unexpected(make_error(child, "logger appender must not be repeated"));
            }
            logger.appenders.push_back(child.args[0]);
            continue;
        }
        return std::unexpected(make_error(child, "unsupported directive in logger block: " + child.name));
    }
    return logger;
}

std::expected<RootLoggerConfig, ConfigError> parse_root_logger(const DirectiveNode &directive) {
    if (!directive.has_block || !directive.args.empty()) {
        return std::unexpected(make_error(directive, "root_logger expects a block without arguments"));
    }

    RootLoggerConfig root;
    root.location = directive.location;
    bool seen_level = false;
    bool seen_verbosity = false;
    std::unordered_set<std::string> appender_names;
    for (const auto &child: directive.children) {
        if (child.has_block || child.args.size() != 1) {
            return std::unexpected(make_error(child, child.name + " expects exactly one argument"));
        }
        if (child.name == "level") {
            if (seen_level) {
                return std::unexpected(make_error(child, "root_logger level must not be repeated"));
            }
            auto level = parse_logging_level(child, child.args[0], "root_logger level");
            if (!level) {
                return std::unexpected(level.error());
            }
            root.level = *level;
            seen_level = true;
            continue;
        }
        if (child.name == "verbosity") {
            if (seen_verbosity) {
                return std::unexpected(make_error(child, "root_logger verbosity must not be repeated"));
            }
            auto verbosity = parse_non_negative_unsigned(child, child.args[0], "root_logger verbosity");
            if (!verbosity) {
                return std::unexpected(verbosity.error());
            }
            root.verbosity = *verbosity;
            seen_verbosity = true;
            continue;
        }
        if (child.name == "appender") {
            if (!appender_names.insert(child.args[0]).second) {
                return std::unexpected(make_error(child, "root_logger appender must not be repeated"));
            }
            root.appenders.push_back(child.args[0]);
            continue;
        }
        return std::unexpected(make_error(child, "unsupported directive in root_logger block: " + child.name));
    }
    return root;
}

std::expected<LoggingConfig, ConfigError> parse_logging(const DirectiveNode &directive) {
    if (!directive.has_block || !directive.args.empty()) {
        return std::unexpected(make_error(directive, "logging expects a block without arguments"));
    }

    LoggingConfig logging;
    logging.location = directive.location;
    logging.configured = true;
    std::unordered_set<std::string> appender_names;
    std::unordered_set<std::string> logger_names;
    bool seen_root = false;
    for (const auto &child: directive.children) {
        if (child.name == "appender") {
            auto appender = parse_log_appender(child);
            if (!appender) {
                return std::unexpected(appender.error());
            }
            if (!appender_names.insert(appender->name).second) {
                return std::unexpected(make_error(child, "duplicate appender name: " + appender->name));
            }
            logging.appenders.push_back(std::move(*appender));
            continue;
        }
        if (child.name == "logger") {
            auto logger = parse_logger_rule(child);
            if (!logger) {
                return std::unexpected(logger.error());
            }
            if (!logger_names.insert(logger->name).second) {
                return std::unexpected(make_error(child, "duplicate logger name: " + logger->name));
            }
            logging.loggers.push_back(std::move(*logger));
            continue;
        }
        if (child.name == "root_logger") {
            if (seen_root) {
                return std::unexpected(make_error(child, "root_logger must not be repeated"));
            }
            auto root = parse_root_logger(child);
            if (!root) {
                return std::unexpected(root.error());
            }
            logging.root = std::move(*root);
            seen_root = true;
            continue;
        }
        return std::unexpected(make_error(child, "unsupported directive in logging block: " + child.name));
    }
    if (!seen_root) {
        return std::unexpected(make_error(directive, "logging must define root_logger"));
    }
    for (const auto &logger: logging.loggers) {
        for (const auto &name: logger.appenders) {
            if (!appender_names.contains(name)) {
                return std::unexpected(ConfigError{
                        .message = "logger references unknown appender: " + name,
                        .location = logger.location,
                });
            }
        }
    }
    for (const auto &name: logging.root.appenders) {
        if (!appender_names.contains(name)) {
            return std::unexpected(ConfigError{
                    .message = "root_logger references unknown appender: " + name,
                    .location = logging.root.location,
            });
        }
    }
    return logging;
}

std::expected<LocationConfig, ConfigError> parse_location(const DirectiveNode &directive,
                                                          const ProxySettingsBuilder &server_proxy_defaults) {
    if (!directive.has_block) {
        return std::unexpected(make_error(directive, "location must be a block"));
    }

    LocationConfig location;
    location.location = directive.location;
    if (directive.args.size() != 1) {
        return std::unexpected(make_error(directive, "location expects exactly one pattern argument"));
    }
    location.pattern = directive.args[0];
    if (contains_variable(location.pattern)) {
        return std::unexpected(make_error(directive, "location does not support variables in V1"));
    }

    ProxySettingsBuilder location_proxy_defaults;
    bool has_proxy_pass = false;
    bool has_script_file = false;
    bool seen_access_log = false;

    for (const auto &child: directive.children) {
        if (child.has_block) {
            return std::unexpected(make_error(child, "nested blocks are not allowed inside location"));
        }

        if (child.name == "proxy_pass") {
            auto target = parse_proxy_pass(child);
            if (!target) {
                return std::unexpected(target.error());
            }
            location.proxy_pass = std::move(*target);
            has_proxy_pass = true;
            continue;
        }
        if (child.name == "script_file") {
            if (child.args.size() != 1) {
                return std::unexpected(make_error(child, "script_file expects exactly one argument"));
            }
            location.kind = LocationKind::Script;
            // Resolve against the file that contains this directive (absolute paths pass
            // through); the stored path is absolute/canonical so RuntimeBuilder opens it
            // regardless of the process working directory.
            location.script_file = resolve_config_path(child.location.source_name, child.args[0]);
            has_script_file = true;
            continue;
        }
        if (child.name == "proxy_connect_timeout" || child.name == "proxy_read_timeout" ||
            child.name == "proxy_send_timeout") {
            auto apply_result = apply_proxy_timeout(location_proxy_defaults, child);
            if (!apply_result) {
                return std::unexpected(apply_result.error());
            }
            continue;
        }
        if (child.name == "proxy_set_header") {
            auto header = parse_proxy_set_header(child);
            if (!header) {
                return std::unexpected(header.error());
            }
            upsert_header(location_proxy_defaults.set_headers, std::move(*header));
            continue;
        }
        if (child.name == "proxy_buffering") {
            if (child.args.size() != 1 || child.args[0] != "off") {
                return std::unexpected(make_error(child, "proxy_buffering only supports 'off' in V1"));
            }
            location_proxy_defaults.proxy_buffering = false;
            continue;
        }
        if (child.name == "access_log") {
            if (seen_access_log) {
                return std::unexpected(make_error(child, "access_log must not be repeated in location"));
            }
            auto access_log = parse_access_log(child);
            if (!access_log) {
                return std::unexpected(access_log.error());
            }
            location.access_log = std::move(*access_log);
            seen_access_log = true;
            continue;
        }

        return std::unexpected(make_error(child, "unsupported directive in location block: " + child.name));
    }

    if (has_proxy_pass && has_script_file) {
        return std::unexpected(make_error(directive, "location must define only one of proxy_pass or script_file"));
    }
    if (!has_proxy_pass && !has_script_file) {
        return std::unexpected(make_error(directive, "location must define proxy_pass or script_file"));
    }
    location.proxy = merge_proxy_settings(server_proxy_defaults, location_proxy_defaults);
    return location;
}

std::expected<ServerConfig, ConfigError> parse_server(const DirectiveNode &directive) {
    if (!directive.has_block) {
        return std::unexpected(make_error(directive, "server must be a block"));
    }
    if (!directive.args.empty()) {
        return std::unexpected(make_error(directive, "server does not accept inline arguments"));
    }

    ServerConfig server;
    server.location = directive.location;
    ProxySettingsBuilder proxy_defaults;
    bool seen_certificate = false;
    bool seen_certificate_key = false;
    bool seen_access_log = false;

    for (const auto &child: directive.children) {
        if (child.name == "server_name") {
            if (child.has_block || child.args.empty()) {
                return std::unexpected(make_error(child, "server_name expects one or more names"));
            }
            for (const auto &name: child.args) {
                if (contains_variable(name)) {
                    return std::unexpected(make_error(child, "server_name does not support variables in V1"));
                }
                server.server_names.push_back(name);
            }
            continue;
        }

        if (child.name == "certificate") {
            if (child.has_block || child.args.size() != 1) {
                return std::unexpected(make_error(child, "certificate expects exactly one path"));
            }
            if (contains_variable(child.args[0])) {
                return std::unexpected(make_error(child, "certificate does not support variables in V1"));
            }
            if (seen_certificate) {
                return std::unexpected(make_error(child, "certificate must not be repeated"));
            }
            // Resolve against the file that contains this directive (absolute paths pass
            // through); stored absolute so TLS loads it regardless of process pwd.
            server.certificate = resolve_config_path(child.location.source_name, child.args[0]);
            seen_certificate = true;
            continue;
        }

        if (child.name == "certificate_key") {
            if (child.has_block || child.args.size() != 1) {
                return std::unexpected(make_error(child, "certificate_key expects exactly one path"));
            }
            if (contains_variable(child.args[0])) {
                return std::unexpected(make_error(child, "certificate_key does not support variables in V1"));
            }
            if (seen_certificate_key) {
                return std::unexpected(make_error(child, "certificate_key must not be repeated"));
            }
            server.certificate_key = resolve_config_path(child.location.source_name, child.args[0]);
            seen_certificate_key = true;
            continue;
        }

        if (child.name == "proxy_connect_timeout" || child.name == "proxy_read_timeout" ||
            child.name == "proxy_send_timeout") {
            auto apply_result = apply_proxy_timeout(proxy_defaults, child);
            if (!apply_result) {
                return std::unexpected(apply_result.error());
            }
            continue;
        }

        if (child.name == "proxy_set_header") {
            auto header = parse_proxy_set_header(child);
            if (!header) {
                return std::unexpected(header.error());
            }
            upsert_header(proxy_defaults.set_headers, std::move(*header));
            continue;
        }

        if (child.name == "access_log") {
            if (seen_access_log) {
                return std::unexpected(make_error(child, "access_log must not be repeated in server"));
            }
            auto access_log = parse_access_log(child);
            if (!access_log) {
                return std::unexpected(access_log.error());
            }
            server.access_log = std::move(*access_log);
            seen_access_log = true;
            continue;
        }

        if (child.name == "location") {
            auto location = parse_location(child, proxy_defaults);
            if (!location) {
                return std::unexpected(location.error());
            }
            server.locations.push_back(std::move(*location));
            continue;
        }

        return std::unexpected(make_error(child, "unsupported directive in server block: " + child.name));
    }

    if (server.server_names.empty()) {
        return std::unexpected(make_error(directive, "server must define at least one server_name"));
    }
    if (has_tls_identity(server) && (server.certificate.empty() || server.certificate_key.empty())) {
        return std::unexpected(make_error(directive, "server must define both certificate and certificate_key"));
    }
    if (server.locations.empty()) {
        return std::unexpected(make_error(directive, "server must define at least one location"));
    }

    server.proxy_defaults = finalize_proxy_settings(proxy_defaults);
    return server;
}

std::expected<ConnectionPoolConfig, ConfigError> parse_connection_pool(const DirectiveNode &directive) {
    if (!directive.has_block) {
        return std::unexpected(make_error(directive, "connection_pool must be a block"));
    }
    ConnectionPoolConfig pool;
    for (const auto &child: directive.children) {
        if (child.has_block) {
            return std::unexpected(make_error(child, "nested blocks are not allowed inside connection_pool"));
        }
        if (child.name == "keepalive_size") {
            if (child.args.size() != 1) {
                return std::unexpected(make_error(child, "keepalive_size expects exactly one argument"));
            }
            if (contains_variable(child.args[0])) {
                return std::unexpected(make_error(child, "keepalive_size does not support variables in V1"));
            }
            std::size_t sz = 0;
            auto r = std::from_chars(child.args[0].data(), child.args[0].data() + child.args[0].size(), sz);
            if (r.ec != std::errc() || r.ptr != child.args[0].data() + child.args[0].size()) {
                return std::unexpected(make_error(child, "keepalive_size must be a non-negative integer"));
            }
            pool.keepalive_size = sz;
            continue;
        }
        if (child.name == "keepalive_timeout") {
            if (child.args.size() != 1) {
                return std::unexpected(make_error(child, "keepalive_timeout expects exactly one argument"));
            }
            auto d = parse_duration(child, child.args[0], "keepalive_timeout");
            if (!d) {
                return std::unexpected(d.error());
            }
            pool.keepalive_timeout = *d;
            continue;
        }
        if (child.name == "steal") {
            if (child.args.size() != 1) {
                return std::unexpected(make_error(child, "steal expects exactly one argument: on|off|auto"));
            }
            if (contains_variable(child.args[0])) {
                return std::unexpected(make_error(child, "steal does not support variables in V1"));
            }
            const std::string_view val = child.args[0];
            if (val == "auto") {
                pool.steal = PoolSteal::Auto;
            } else if (val == "on") {
                pool.steal = PoolSteal::On;
            } else if (val == "off") {
                pool.steal = PoolSteal::Off;
            } else {
                return std::unexpected(make_error(child, "steal must be one of: on|off|auto"));
            }
            continue;
        }
        if (child.name == "max_idle_total") {
            if (child.args.size() != 1) {
                return std::unexpected(make_error(child, "max_idle_total expects exactly one argument"));
            }
            if (contains_variable(child.args[0])) {
                return std::unexpected(make_error(child, "max_idle_total does not support variables in V1"));
            }
            std::size_t sz = 0;
            auto r = std::from_chars(child.args[0].data(), child.args[0].data() + child.args[0].size(), sz);
            if (r.ec != std::errc() || r.ptr != child.args[0].data() + child.args[0].size()) {
                return std::unexpected(make_error(child, "max_idle_total must be a non-negative integer"));
            }
            pool.max_idle_total = sz;
            continue;
        }
        if (child.name == "initial_group_capacity") {
            if (child.args.size() != 1) {
                return std::unexpected(make_error(child, "initial_group_capacity expects exactly one argument"));
            }
            if (contains_variable(child.args[0])) {
                return std::unexpected(make_error(child, "initial_group_capacity does not support variables in V1"));
            }
            std::size_t sz = 0;
            auto r = std::from_chars(child.args[0].data(), child.args[0].data() + child.args[0].size(), sz);
            if (r.ec != std::errc() || r.ptr != child.args[0].data() + child.args[0].size()) {
                return std::unexpected(make_error(child, "initial_group_capacity must be a non-negative integer"));
            }
            pool.initial_group_capacity = sz;
            continue;
        }
        return std::unexpected(make_error(child, "unsupported directive in connection_pool block: " + child.name));
    }
    return pool;
}

std::expected<HttpConfig, ConfigError> parse_http(const DirectiveNode &directive) {
    if (!directive.has_block) {
        return std::unexpected(make_error(directive, "http must be a block"));
    }
    if (!directive.args.empty()) {
        return std::unexpected(make_error(directive, "http does not accept inline arguments"));
    }

    HttpConfig http;
    std::unordered_set<std::string> upstream_names;
    bool has_tls_listen = false;
    bool seen_access_log = false;

    for (const auto &child: directive.children) {
        if (child.name == "listen") {
            auto listen = parse_listen_address(child);
            if (!listen) {
                return std::unexpected(listen.error());
            }
            has_tls_listen = has_tls_listen || listen->tls;
            http.listens.push_back(std::move(*listen));
            continue;
        }
        if (child.name == "upstream") {
            auto upstream = parse_upstream(child);
            if (!upstream) {
                return std::unexpected(upstream.error());
            }
            if (!upstream_names.insert(upstream->name).second) {
                return std::unexpected(make_error(child, "duplicate upstream name: " + upstream->name));
            }
            http.upstreams.push_back(std::move(*upstream));
            continue;
        }
        if (child.name == "connection_pool") {
            auto pool = parse_connection_pool(child);
            if (!pool) {
                return std::unexpected(pool.error());
            }
            http.connection_pool = std::move(*pool);
            continue;
        }
        if (child.name == "access_log") {
            if (seen_access_log) {
                return std::unexpected(make_error(child, "access_log must not be repeated in http"));
            }
            auto access_log = parse_access_log(child);
            if (!access_log) {
                return std::unexpected(access_log.error());
            }
            http.access_log = std::move(*access_log);
            seen_access_log = true;
            continue;
        }
        if (child.name == "server") {
            auto server = parse_server(child);
            if (!server) {
                return std::unexpected(server.error());
            }
            http.servers.push_back(std::move(*server));
            continue;
        }
        return std::unexpected(make_error(child, "unsupported directive in http block: " + child.name));
    }

    if (http.listens.empty()) {
        return std::unexpected(make_error(directive, "http must define at least one listen"));
    }
    if (http.servers.empty()) {
        return std::unexpected(make_error(directive, "http must define at least one server"));
    }

    for (const auto &server: http.servers) {
        if (has_tls_listen && !has_tls_identity(server)) {
            return std::unexpected(ConfigError{
                    .message = "server must define certificate and certificate_key when any ssl listen is configured",
                    .location = server.location,
            });
        }
        if (!has_tls_listen && has_tls_identity(server)) {
            return std::unexpected(ConfigError{
                    .message = "certificate directives require at least one ssl listen in http",
                    .location = server.location,
            });
        }
        for (const auto &location: server.locations) {
            if (location.kind == LocationKind::Script) {
                continue; // script locations do not proxy and have no upstream reference
            }
            if (location.proxy_pass.kind == ProxyPassKind::NamedUpstream &&
                !upstream_names.contains(location.proxy_pass.upstream_name)) {
                return std::unexpected(ConfigError{
                        .message = "proxy_pass references unknown upstream: " + location.proxy_pass.upstream_name,
                        .location = location.proxy_pass.location,
                });
            }
        }
    }

    return http;
}

} // namespace

std::expected<MainConfig, ConfigError> SemanticAnalyzer::analyze(const Document &document) const {
    MainConfig config;
    bool seen_http = false;
    bool seen_worker_processes = false;
    bool seen_logging = false;

    for (const auto &directive: document.directives) {
        if (directive.name == "worker_processes") {
            if (directive.has_block || directive.args.size() != 1) {
                return std::unexpected(make_error(directive, "worker_processes expects exactly one integer argument"));
            }
            if (seen_worker_processes) {
                return std::unexpected(make_error(directive, "worker_processes must not be repeated"));
            }
            auto workers = parse_positive_size(directive, directive.args[0], "worker_processes");
            if (!workers) {
                return std::unexpected(workers.error());
            }
            config.worker_processes = *workers;
            seen_worker_processes = true;
            continue;
        }

        if (directive.name == "http") {
            if (seen_http) {
                return std::unexpected(make_error(directive, "http block must not be repeated"));
            }
            auto http_result = parse_http(directive);
            if (!http_result) {
                return std::unexpected(http_result.error());
            }
            config.http = std::move(*http_result);
            seen_http = true;
            continue;
        }

        if (directive.name == "logging") {
            if (seen_logging) {
                return std::unexpected(make_error(directive, "logging block must not be repeated"));
            }
            auto logging = parse_logging(directive);
            if (!logging) {
                return std::unexpected(logging.error());
            }
            config.logging = std::move(*logging);
            seen_logging = true;
            continue;
        }

        return std::unexpected(make_error(directive, "unsupported top-level directive: " + directive.name));
    }

    if (!seen_http) {
        return std::unexpected(ConfigError{
                .message = "missing required http block",
                .location = SourceLocation{},
        });
    }

    return config;
}

} // namespace fiber::lite_nginx::config
