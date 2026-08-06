#include "ProxyResponsePlan.h"

#include <utility>

namespace fiber::access_server {
namespace {

bool java_authority_matches(std::string_view absolute_url, std::size_t authority_start, std::size_t authority_end,
                            std::string_view upstream_host) noexcept {
    const std::size_t authority_size = authority_end - authority_start;
    return authority_size <= upstream_host.size() &&
           absolute_url.substr(authority_start, authority_size) == upstream_host.substr(0, authority_size);
}

std::string normalized_downstream_scheme(std::string_view scheme) {
    return scheme.empty() ? std::string("http") : std::string(scheme);
}

} // namespace

PreparedProxyResponseHeadersResult prepare_proxy_response_headers(const CompiledHeaderTemplates &headers,
                                                                  TemplateEvaluator evaluator) {
    PreparedProxyResponseHeaders result;
    result.reserve(headers.size());
    for (const CompiledHeaderTemplates::EntryView header: headers) {
        auto value = evaluate_template(header.value(), evaluator);
        if (!value) {
            return std::unexpected(value.error());
        }
        if (value->empty() || is_java_filtered_response_header(header.name())) {
            continue;
        }
        if (!is_valid_http_header_name(header.name()) || !is_valid_http_header_value(*value)) {
            return std::unexpected(Err::from_exception(Exception::unknown("invalid proxy response header")));
        }
        result.push_back(EvaluatedHeader{
                .name = std::string(header.name()),
                .value = std::move(*value),
        });
    }
    return result;
}

std::optional<std::string> rewrite_java_proxy_location(std::string_view upstream_value, std::string_view upstream_host,
                                                       std::string_view downstream_scheme,
                                                       std::string_view downstream_host) {
    const std::size_t scheme = upstream_value.find("://");
    if (scheme == std::string_view::npos || scheme == 0) {
        return std::nullopt;
    }
    const std::size_t authority_start = scheme + 3;
    std::size_t authority_end = upstream_value.find('/', authority_start);
    if (authority_end == std::string_view::npos) {
        authority_end = upstream_value.size();
    }
    if (!java_authority_matches(upstream_value, authority_start, authority_end, upstream_host)) {
        return std::nullopt;
    }

    std::string result = normalized_downstream_scheme(downstream_scheme);
    result.append("://");
    result.append(downstream_host);
    result.append(upstream_value.substr(authority_end));
    return result;
}

std::optional<std::string> rewrite_java_proxy_refresh(std::string_view upstream_value, std::string_view upstream_host,
                                                      std::string_view downstream_scheme,
                                                      std::string_view downstream_host) {
    const std::size_t url = upstream_value.find("url=");
    if (url == std::string_view::npos || url == 0) {
        return std::nullopt;
    }
    const std::size_t scheme = upstream_value.find("://", url + 4);
    if (scheme == std::string_view::npos || scheme == 0) {
        return std::nullopt;
    }
    const std::size_t authority_start = scheme + 3;
    std::size_t authority_end = upstream_value.find('/', authority_start);
    if (authority_end == std::string_view::npos) {
        authority_end = upstream_value.size();
    }
    if (!java_authority_matches(upstream_value, authority_start, authority_end, upstream_host)) {
        return std::nullopt;
    }

    std::string result(upstream_value.substr(0, url + 4));
    result.append(normalized_downstream_scheme(downstream_scheme));
    result.append("://");
    result.append(downstream_host);
    result.append(upstream_value.substr(authority_end));
    return result;
}

} // namespace fiber::access_server
