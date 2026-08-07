#include "ResponsePlan.h"

#include <fiber/http/HttpHeaderHash.h>

namespace fiber::access_server {

bool is_valid_http_header_name(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    for (const unsigned char ch: name) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
            continue;
        }
        switch (ch) {
            case '!':
            case '#':
            case '$':
            case '%':
            case '&':
            case '\'':
            case '*':
            case '+':
            case '-':
            case '.':
            case '^':
            case '_':
            case '`':
            case '|':
            case '~':
                continue;
            default:
                return false;
        }
    }
    return true;
}

bool is_valid_http_header_value(std::string_view value) noexcept {
    for (const unsigned char ch: value) {
        if (ch == '\0' || ch == '\v' || ch == '\f' || ch == '\r' || ch == '\n' || ch == 0x7fU) {
            return false;
        }
    }
    return true;
}

bool is_java_filtered_response_header(std::string_view name) noexcept {
    return http::http_header_name_equals_ci(name, "connection") ||
           http::http_header_name_equals_ci(name, "content-length") ||
           http::http_header_name_equals_ci(name, "proxy-connection") ||
           http::http_header_name_equals_ci(name, "keep-alive") ||
           http::http_header_name_equals_ci(name, "proxy-authenticate") ||
           http::http_header_name_equals_ci(name, "proxy-authorization") ||
           http::http_header_name_equals_ci(name, "te") || http::http_header_name_equals_ci(name, "trailer") ||
           http::http_header_name_equals_ci(name, "transfer-encoding") ||
           http::http_header_name_equals_ci(name, "upgrade");
}

PreparedResponseResult prepare_response(const CompiledResponseRoute &response, TemplateEvaluator evaluator) {
    PreparedResponse prepared;
    prepared.status = response.status;

    prepared.headers.reserve(response.response_headers.size());
    for (const CompiledTemplateEntry &header: response.response_headers) {
        auto value = evaluate_template(header.value, evaluator);
        if (!value) {
            return std::unexpected(value.error());
        }
        prepared.headers.push_back(EvaluatedHeader{
                .name = header.name,
                .value = std::move(*value),
        });
    }

    std::size_t committed = 0;
    for (std::size_t index = 0; index < prepared.headers.size(); ++index) {
        EvaluatedHeader &header = prepared.headers[index];
        if (is_java_filtered_response_header(header.name)) {
            continue;
        }
        if (!is_valid_http_header_name(header.name) || !is_valid_http_header_value(header.value)) {
            return std::unexpected(Err::from_exception(Exception::unknown("invalid response header")));
        }
        if (committed != index) {
            prepared.headers[committed] = std::move(header);
        }
        ++committed;
    }
    prepared.headers.resize(committed);

    if (response.body_kind == ResponseBodyKind::Template) {
        if (!response.body_template) {
            return std::unexpected(Err::from_exception(Exception{
                    .name = "TEMPLATE_SCRIPT",
                    .message = "error exec for template expression: invalid compiled template",
                    .status = 500,
            }));
        }
        auto body = evaluate_template(*response.body_template, evaluator);
        if (!body) {
            return std::unexpected(body.error());
        }
        prepared.body = std::move(*body);
    } else {
        prepared.body = response.body;
    }
    return prepared;
}

} // namespace fiber::access_server
