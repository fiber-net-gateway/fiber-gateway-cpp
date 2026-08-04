#include "ProxyRequestPlan.h"

#include "../../../../src/http/HttpExchange.h"
#include "../../../../src/http/HttpHeaderHash.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace fiber::access_server {
namespace {

constexpr std::string_view kCallSourceHeader = "x-ploto-source-app";

bool header_name_equals(std::string_view left, std::string_view right) noexcept {
    return http::http_header_name_equals_ci(left, right);
}

void set_header(std::vector<EvaluatedHeader> &headers, std::string name, std::string value) {
    std::erase_if(headers, [&](const EvaluatedHeader &header) { return header_name_equals(header.name, name); });
    headers.push_back(EvaluatedHeader{
            .name = std::move(name),
            .value = std::move(value),
    });
}

void add_header(std::vector<EvaluatedHeader> &headers, std::string_view name, std::string_view value) {
    headers.push_back(EvaluatedHeader{
            .name = std::string(name),
            .value = std::string(value),
    });
}

std::string preserved_request_target(const http::HttpExchange &exchange) {
    if (!exchange.uri().unparsed_uri.empty()) {
        return std::string(exchange.uri().unparsed_uri);
    }
    std::string storage;
    storage.assign(exchange.uri().path);
    if (!exchange.uri().query.empty()) {
        storage.push_back('?');
        storage.append(exchange.uri().query);
    }
    return storage;
}

bool is_websocket_request(const http::HttpExchange &exchange, const CompiledProxyRoute &proxy) noexcept {
    if (!proxy.websocket_timeout_millis || *proxy.websocket_timeout_millis <= 0) {
        return false;
    }
    return header_name_equals(exchange.header("Upgrade"), "websocket") &&
           header_name_equals(exchange.header("Connection"), "upgrade");
}

std::optional<std::uint64_t> normalized_max_response_body(const CompiledProxyRoute &proxy) noexcept {
    if (!proxy.max_response_body_size || *proxy.max_response_body_size == 0) {
        return std::nullopt;
    }
    if (*proxy.max_response_body_size < 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(*proxy.max_response_body_size);
}

std::expected<std::vector<EvaluatedHeader>, AccessError>
evaluate_context(std::span<const CompiledTemplateEntry> context, TemplateEvaluator evaluator) {
    std::vector<EvaluatedHeader> result;
    result.reserve(context.size());
    for (const CompiledTemplateEntry &entry: context) {
        auto value = evaluate_template(entry.value, evaluator);
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        result.push_back(EvaluatedHeader{
                .name = entry.name,
                .value = std::move(*value),
        });
    }
    return result;
}

std::expected<std::string, AccessError> resolve_request_target(const http::HttpExchange &exchange,
                                                               const CompiledProxyRoute &proxy,
                                                               TemplateEvaluator evaluator) {
    if (!proxy.rewrite) {
        return preserved_request_target(exchange);
    }

    auto rewritten = evaluate_template(*proxy.rewrite, evaluator);
    if (!rewritten) {
        return std::unexpected(std::move(rewritten.error()));
    }
    std::string result = rewritten->empty() ? std::string("/") : java_escape_uri(*rewritten);
    if (!exchange.uri().query.empty()) {
        result.push_back('?');
        result.append(exchange.uri().query);
    }
    return result;
}

std::expected<std::vector<EvaluatedHeader>, AccessError>
build_proxy_headers(const http::HttpExchange &exchange, std::string_view project, const CompiledProxyRoute &proxy,
                    TemplateEvaluator evaluator, bool websocket) {
    std::vector<EvaluatedHeader> result;
    result.reserve(exchange.request_headers().size() + proxy.proxy_headers.size() + 3);

    if (websocket) {
        set_header(result, "Connection", "upgrade");
        set_header(result, "Upgrade", "websocket");
    } else {
        const std::string_view content_length = exchange.header("Content-Length");
        if (!content_length.empty()) {
            set_header(result, "Content-Length", std::string(content_length));
        }
    }

    for (const CompiledHeaderTemplates::EntryView header: proxy.proxy_headers) {
        auto value = evaluate_template(header.value(), evaluator);
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        if (value->empty() || is_java_filtered_response_header(header.name())) {
            continue;
        }
        if (!is_valid_http_header_name(header.name()) || !is_valid_http_header_value(*value)) {
            return std::unexpected(AccessError::unknown("invalid proxy request header"));
        }
        set_header(result, std::string(header.name()), std::move(*value));
    }

    for (const http::HttpHeaders::HeaderField &header: exchange.request_headers()) {
        if (header.name_len == 0 || is_java_filtered_proxy_request_header(header.name_view()) ||
            proxy.proxy_headers.contains(header.lowcase_view(), header.name_hash)) {
            continue;
        }
        add_header(result, header.name_view(), header.value_view());
    }

    std::string source(project);
    source.append(".unifiedAccess");
    set_header(result, std::string(kCallSourceHeader), std::move(source));
    return result;
}

http::HttpBodySpec request_body_spec(const http::HttpExchange &exchange, bool websocket) noexcept {
    if (websocket) {
        return http::HttpBodySpec::None();
    }
    const http::HttpBodySpec inbound = exchange.request_body_spec();
    if (!exchange.header("Content-Length").empty() && inbound.is_content_length()) {
        return http::HttpBodySpec::ContentLength(inbound.content_length());
    }
    // Java always installs a streaming request-body function. Its HTTP client
    // therefore adds Transfer-Encoding: chunked when Content-Length is absent,
    // including for an immediately-complete empty body.
    return http::HttpBodySpec::Chunked();
}

} // namespace

bool is_java_filtered_proxy_request_header(std::string_view name) noexcept {
    return header_name_equals(name, "host") || is_java_filtered_response_header(name);
}

std::string java_escape_uri(std::string_view value) {
    // Same byte mask as fiber-net-gateway UriCodec.escapeUri (the Nginx URI
    // escape table). UTF-8 bytes are escaped independently.
    constexpr std::array<std::uint32_t, 8> kEscape{
            0xFFFF'FFFFU, 0xD000'002DU, 0x5000'0000U, 0xB800'0001U,
            0xFFFF'FFFFU, 0xFFFF'FFFFU, 0xFFFF'FFFFU, 0xFFFF'FFFFU,
    };
    constexpr char kHex[] = "0123456789ABCDEF";

    std::size_t escaped_size = value.size();
    for (const unsigned char byte: value) {
        if ((kEscape[byte >> 5U] & (1U << (byte & 0x1FU))) != 0) {
            escaped_size += 2;
        }
    }
    std::string result;
    result.reserve(escaped_size);
    for (const unsigned char byte: value) {
        if ((kEscape[byte >> 5U] & (1U << (byte & 0x1FU))) == 0) {
            result.push_back(static_cast<char>(byte));
            continue;
        }
        result.push_back('%');
        result.push_back(kHex[byte >> 4U]);
        result.push_back(kHex[byte & 0x0FU]);
    }
    return result;
}

PreparedProxyRequestResult prepare_proxy_request(const http::HttpExchange &exchange, std::string_view project,
                                                 const CompiledProxyRoute &proxy, TemplateEvaluator evaluator,
                                                 std::size_t max_request_body_size,
                                                 std::string_view initial_context_cluster) {
    PreparedProxyRequest result(proxy.response_headers);
    result.upstream_kind = proxy.upstream_kind;
    result.service = proxy.service;
    if (proxy.cluster) {
        result.cluster = *proxy.cluster;
    }
    result.addresses = proxy.addresses;
    result.response_evaluator = evaluator;
    result.method = exchange.method();
    result.max_request_body_size = max_request_body_size;
    result.timeout_millis = proxy.timeout_millis;
    result.max_response_body_size = normalized_max_response_body(proxy);
    result.websocket_upgrade = is_websocket_request(exchange, proxy);
    result.websocket_timeout_millis =
            result.websocket_upgrade && proxy.websocket_timeout_millis ? *proxy.websocket_timeout_millis : 0;
    result.flush = proxy.flush.value_or(false);
    result.body = request_body_spec(exchange, result.websocket_upgrade);
    if (!initial_context_cluster.empty()) {
        result.context_cluster = initial_context_cluster;
    }

    auto context = evaluate_context(proxy.context, evaluator);
    if (!context) {
        return std::unexpected(std::move(context.error()));
    }
    result.context = std::move(*context);
    for (const EvaluatedHeader &entry: result.context) {
        if (entry.name == "HI-TRACE-CLUSTER") {
            if (entry.value.empty()) {
                result.context_cluster.reset();
            } else {
                result.context_cluster = entry.value;
            }
            break;
        }
    }

    auto target = resolve_request_target(exchange, proxy, evaluator);
    if (!target) {
        return std::unexpected(std::move(target.error()));
    }
    result.request_target = std::move(*target);

    auto headers = build_proxy_headers(exchange, project, proxy, evaluator, result.websocket_upgrade);
    if (!headers) {
        return std::unexpected(std::move(headers.error()));
    }
    result.headers = std::move(*headers);
    return result;
}

} // namespace fiber::access_server
