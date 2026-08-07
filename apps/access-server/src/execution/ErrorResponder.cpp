#include "ErrorResponder.h"
#include "../observability/AccessRequestTelemetry.h"

#include <fiber/common/json/JsonEncode.h>
#include <fiber/http/HttpBodySpec.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/http/HttpExchangeIo.h>
#include <fiber/http/HttpHeaderHash.h>
#include <fiber/http/HttpHeaders.h>

#include <charconv>

namespace fiber::access_server {
namespace {

class StringSink final : public json::OutputSink {
public:
    explicit StringSink(std::string &output) noexcept : output_(output) {}

    [[nodiscard]] bool write(const char *data, std::size_t size) override {
        output_.append(data, size);
        return true;
    }

private:
    std::string &output_;
};

void append_ascii(std::string &output, std::string_view text) {
    for (const unsigned char ch: text) {
        output.push_back(ch < 0x80U ? static_cast<char>(ch) : '?');
    }
}

void append_status(std::string &output, int status) {
    char buffer[16]{};
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), status);
    if (result.ec == std::errc{}) {
        output.append(buffer, result.ptr);
    }
}

std::string render_json(const Exception &error) {
    std::string output;
    StringSink sink(output);
    json::Generator generator(sink);
    using Result = json::Generator::Result;
    if (generator.map_open() != Result::OK || generator.string("name", 4) != Result::OK ||
        generator.string(error.name.data(), error.name.size()) != Result::OK ||
        generator.string("message", 7) != Result::OK ||
        generator.string(error.message.data(), error.message.size()) != Result::OK ||
        generator.string("meta", 4) != Result::OK || generator.null_value() != Result::OK) {
        return {};
    }
    const Result close = generator.map_close();
    if (close != Result::OK && close != Result::GenerateComplete) {
        return {};
    }
    return output;
}

std::string render_html(const Exception &error, std::string_view trace_id) {
    std::string output;
    output.reserve(512 + error.name.size() + error.message.size() + trace_id.size());
    output.append("<!DOCTYPE html>\n"
                  "<html lang=\"en\">\n"
                  "<head>\n"
                  "    <meta charset=\"UTF-8\">\n"
                  "    <title>Ploto-Access-Server</title>\n"
                  "</head>\n"
                  "<body>\n"
                  "<div style=\"text-align: center\">\n"
                  "    <h3>");
    append_ascii(output, error.name);
    output.append(":&nbsp; <span style=\"color: #ff5544\">");
    append_status(output, error.status);
    output.append("</span></h3>\n"
                  "    <p>");
    output.append(error.message);
    output.append("</p>\n"
                  "    <h5>traceID: ");
    append_ascii(output, trace_id);
    output.append("</h5>\n"
                  "    <pre>"
                  "</pre>\n"
                  "</div>\n"
                  "</body>\n"
                  "</html>");
    return output;
}

async::Task<common::IoResult<void>> send_rendered(http::HttpExchange &exchange, const RenderedError &rendered,
                                                  std::chrono::milliseconds timeout,
                                                  AccessRequestTelemetry &telemetry) noexcept {
    http::HttpHeaders &headers = telemetry.response_headers();
    if (!headers.set("Content-Type", rendered.content_type) || !telemetry.finalize_response_headers()) {
        co_return std::unexpected(common::IoErr::NoMem);
    }

    const bool empty = rendered.body.empty();
    auto header_result = co_await exchange.send_header(
            {
                    .kind = http::OutgoingHeaderKind::Final,
                    .status_code = rendered.status,
                    .headers = &headers,
                    .body = http::HttpBodySpec::ContentLength(rendered.body.size()),
                    .connection_mode = http::ResponseConnectionMode::Auto,
                    .end_stream = empty,
            },
            timeout);
    if (!header_result) {
        co_return std::unexpected(header_result.error());
    }
    if (empty) {
        co_return common::IoResult<void>{};
    }

    auto body_result = co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(rendered.body.data()),
                                                   rendered.body.size(), true, timeout);
    if (!body_result) {
        co_return std::unexpected(body_result.error());
    }
    co_return common::IoResult<void>{};
}

} // namespace

bool ErrorResponder::wants_html(std::string_view accept) noexcept {
    constexpr std::string_view kHtml = "text/html";
    return accept.size() >= kHtml.size() && http::http_header_name_equals_ci(accept.substr(0, kHtml.size()), kHtml);
}

RenderedError ErrorResponder::render(const Exception &error, std::string_view accept, std::string_view trace_id) {
    if (wants_html(accept)) {
        return RenderedError{
                .status = static_cast<int>(error.status),
                .content_type = "text/html",
                .body = render_html(error, trace_id),
        };
    }
    return RenderedError{
            .status = static_cast<int>(error.status),
            .content_type = "application/json; charset=utf-8",
            .body = render_json(error),
    };
}

async::Task<common::IoResult<void>> ErrorResponder::send(http::HttpExchange &exchange,
                                                         AccessRequestTelemetry &telemetry,
                                                         const Exception &error) const noexcept {
    if (exchange.response_stats().header_sent) {
        co_return std::unexpected(common::IoErr::Already);
    }

    const std::string_view trace_id =
            telemetry.trace_id().empty() ? std::string_view("unknown-trace-id") : telemetry.trace_id();
    const RenderedError rendered = render(error, exchange.header("Accept"), trace_id);
    co_return co_await send_rendered(exchange, rendered, options_.write_timeout, telemetry);
}

} // namespace fiber::access_server
