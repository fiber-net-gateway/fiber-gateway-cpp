#include "McpProtocol.h"

#include "McpJsonCodec.h"

#include <cstddef>
#include <utility>

#include <common/json/JsonParse.h>
#include <common/json/JsonParser.h>
#include <common/mem/BufPool.h>
#include <event/EventLoop.h>
#include <http/HttpExchange.h>

namespace fiber::ai_server {
namespace {

constexpr std::size_t kMaxMcpBodyBytes = 4 * 1024 * 1024;

enum class MessageKind : std::uint8_t {
    Request,
    Notification,
    Response,
    Invalid,
};

struct ParsedMessage {
    MessageKind kind = MessageKind::Invalid;
    std::string id_json = "null";
    std::string method;
    std::string params_json = "{}";
    const json::JsonAny *params = nullptr;
};

const json::JsonAny *field(const json::JsonObject<json::JsonAny> &object, std::string_view name) noexcept {
    const auto *entry = object.find(name);
    return entry ? &entry->value : nullptr;
}

McpProtocolError protocol_error(McpProtocolErrorCode code, std::string message, int json_rpc_code = -32600) {
    return McpProtocolError{
            .code = code,
            .json_rpc_code = json_rpc_code,
            .message = std::move(message),
    };
}

std::expected<json::JsonAny, McpProtocolError> parse_document(std::string_view body, mem::BufPool &pool) {
    if (body.empty() || body.size() > kMaxMcpBodyBytes) {
        return std::unexpected(protocol_error(McpProtocolErrorCode::InvalidJson,
                                              body.empty() ? "request body is empty" : "request body is too large",
                                              -32700));
    }
    json::JsonParser parser;
    if (!parser.feed(body.data(), body.size())) {
        return std::unexpected(protocol_error(McpProtocolErrorCode::InvalidJson, "invalid JSON", -32700));
    }
    parser.finish();
    json::JsonAny root;
    if (json::parse_document(
                parser, pool, root,
                [](json::JsonParser &value_parser, mem::BufPool &value_pool, json::JsonAny &value) noexcept {
                    return json::parse_any(value_parser, value_pool, value);
                }) != json::ParseStatus::Done) {
        return std::unexpected(protocol_error(McpProtocolErrorCode::InvalidJson, "invalid JSON", -32700));
    }
    return root;
}

ParsedMessage parse_message(const json::JsonAny &value) {
    ParsedMessage parsed;
    if (!value.is_object()) {
        return parsed;
    }
    const auto &object = value.as_object();
    const json::JsonAny *jsonrpc = field(object, "jsonrpc");
    if (jsonrpc && (!jsonrpc->is_text() || jsonrpc->as_text() != "2.0")) {
        return parsed;
    }
    const json::JsonAny *method = field(object, "method");
    const json::JsonAny *id = field(object, "id");
    const json::JsonAny *result = field(object, "result");
    const json::JsonAny *error = field(object, "error");
    parsed.params = field(object, "params");
    if (parsed.params) {
        parsed.params_json.clear();
        if (!encode_json_any(*parsed.params, parsed.params_json)) {
            return {};
        }
    }
    if (method) {
        if (!method->is_text() || method->as_text().empty()) {
            return {};
        }
        parsed.method = std::string(method->as_text());
        if (id) {
            parsed.id_json.clear();
            if (!encode_json_any(*id, parsed.id_json)) {
                return {};
            }
            parsed.kind = MessageKind::Request;
        } else {
            parsed.kind = MessageKind::Notification;
        }
        return parsed;
    }
    if (result || error) {
        parsed.kind = MessageKind::Response;
    }
    return parsed;
}

void append_error(std::vector<std::string> &responses, std::string_view id_json, int code, std::string_view message) {
    std::string response;
    response.reserve(id_json.size() + message.size() + 80);
    response.append("{\"jsonrpc\":\"2.0\",\"id\":");
    response.append(id_json);
    response.append(",\"error\":{\"code\":");
    response.append(std::to_string(code));
    response.append(",\"message\":");
    (void) append_json_string(response, message);
    response.append("}}");
    responses.push_back(std::move(response));
}

void append_result(std::vector<std::string> &responses, std::string_view id_json, std::string_view result_json) {
    std::string response;
    response.reserve(id_json.size() + result_json.size() + 40);
    response.append("{\"jsonrpc\":\"2.0\",\"id\":");
    response.append(id_json);
    response.append(",\"result\":");
    response.append(result_json);
    response.push_back('}');
    responses.push_back(std::move(response));
}

std::string initialize_result(std::string_view version) {
    std::string result;
    result.reserve(192);
    result.append("{\"protocolVersion\":");
    (void) append_json_string(result, version);
    result.append(",\"capabilities\":{\"tools\":{\"listChanged\":true}},"
                  "\"serverInfo\":{\"name\":\"ploto-ai-server\",\"version\":\"1.0\"}}");
    return result;
}

std::string tools_result(const McpProjectRuntime &project) {
    std::size_t capacity = 32;
    for (const auto &tool: project.tools) {
        capacity += tool->descriptor.tool_json.size() + 1;
    }
    std::string result;
    result.reserve(capacity);
    result.append("{\"tools\":[");
    bool first = true;
    for (const auto &tool: project.tools) {
        if (!first) {
            result.push_back(',');
        }
        first = false;
        result.append(tool->descriptor.tool_json);
    }
    result.append("]}");
    return result;
}

async::Task<void> handle_request(http::HttpExchange &exchange, const std::shared_ptr<McpSession> &session,
                                 const ParsedMessage &message, McpProtocolOutput &output) noexcept {
    if (message.method == "initialize") {
        std::string_view requested_version;
        if (message.params && message.params->is_object()) {
            const json::JsonAny *version = field(message.params->as_object(), "protocolVersion");
            if (version && version->is_text()) {
                requested_version = version->as_text();
            }
        }
        const std::string_view selected =
                McpProtocol::supported_version(requested_version) ? requested_version : kMcpLatestProtocolVersion;
        if (!session->begin_initialize(std::string(selected))) {
            append_error(output.responses, message.id_json, -32600, "Invalid Request: Server already initialized");
            co_return;
        }
        append_result(output.responses, message.id_json, initialize_result(selected));
        co_return;
    }
    if (!session->validate_initialized()) {
        append_error(output.responses, message.id_json, -32000, "Bad Request: Server not initialized");
        co_return;
    }
    if (message.method == "ping") {
        append_result(output.responses, message.id_json, "{}");
        co_return;
    }
    if (message.method == "tools/list") {
        const auto project = session->project();
        append_result(output.responses, message.id_json, tools_result(*project));
        co_return;
    }
    if (message.method != "tools/call") {
        std::string error = "unknown method: ";
        error.append(message.method);
        append_error(output.responses, message.id_json, -32601, error);
        co_return;
    }
    if (!message.params || !message.params->is_object()) {
        append_error(output.responses, message.id_json, -32602, "tools/call params must be an object");
        co_return;
    }
    const auto &params = message.params->as_object();
    const json::JsonAny *name = field(params, "name");
    if (!name || !name->is_text() || name->as_text().empty()) {
        append_error(output.responses, message.id_json, -32602, "tool name is required");
        co_return;
    }
    const auto project = session->project();
    const McpTool *tool = project->find_tool(name->as_text());
    if (!tool || !tool->handler) {
        std::string error = "tool not found: ";
        error.append(name->as_text());
        append_error(output.responses, message.id_json, -32600, error);
        co_return;
    }
    const json::JsonAny *arguments = field(params, "arguments");
    std::string arguments_json = "null";
    if (arguments) {
        arguments_json.clear();
        if (!encode_json_any(*arguments, arguments_json)) {
            append_error(output.responses, message.id_json, -32602, "tool arguments are too large");
            co_return;
        }
    }
    McpToolCallResult called = co_await tool->handler->invoke(exchange, arguments_json);
    std::string result;
    result.reserve(called.text.size() + 80);
    result.append("{\"content\":[");
    if (called.has_content) {
        result.append("{\"type\":\"text\",\"text\":");
        (void) append_json_string(result, called.text);
        result.push_back('}');
    }
    result.append("],\"isError\":");
    result.append(called.is_error ? "true}" : "false}");
    append_result(output.responses, message.id_json, result);
}

} // namespace

async::Task<std::expected<McpProtocolOutput, McpProtocolError>>
McpProtocol::process(http::HttpExchange &exchange, const std::shared_ptr<McpSession> &session, std::string_view body,
                     std::string_view protocol_header) noexcept {
    if (!session || session->state() == McpSessionState::Closed) {
        co_return std::unexpected(protocol_error(McpProtocolErrorCode::InvalidRequest, "session is closed"));
    }
    mem::BufPool pool;
    auto root = parse_document(body, pool);
    if (!root) {
        co_return std::unexpected(std::move(root.error()));
    }
    std::vector<const json::JsonAny *> values;
    if (root->is_object()) {
        values.push_back(&*root);
    } else if (root->is_array() && !root->as_array().empty()) {
        values.reserve(root->as_array().size());
        for (const json::JsonAny &value: root->as_array()) {
            values.push_back(&value);
        }
    } else {
        co_return std::unexpected(
                protocol_error(McpProtocolErrorCode::InvalidRequest, "request must be an object or non-empty array"));
    }

    std::vector<ParsedMessage> messages;
    messages.reserve(values.size());
    std::size_t initialize_count = 0;
    for (const json::JsonAny *value: values) {
        ParsedMessage message = parse_message(*value);
        if (message.kind == MessageKind::Request && message.method == "initialize") {
            ++initialize_count;
        }
        messages.push_back(std::move(message));
    }
    if (initialize_count != 0 && (initialize_count != 1 || messages.size() != 1)) {
        co_return std::unexpected(
                protocol_error(McpProtocolErrorCode::InvalidRequest, "only one initialization request is allowed"));
    }
    if (initialize_count == 0 && !protocol_header.empty() && !supported_version(protocol_header)) {
        co_return std::unexpected(
                protocol_error(McpProtocolErrorCode::InvalidRequest, "Unsupported MCP protocol version", -32000));
    }

    McpProtocolOutput output;
    for (const ParsedMessage &message: messages) {
        switch (message.kind) {
            case MessageKind::Request:
                output.has_request = true;
                co_await handle_request(exchange, session, message, output);
                break;
            case MessageKind::Notification:
                if (message.method == "notifications/initialized") {
                    session->mark_initialized();
                }
                break;
            case MessageKind::Response:
                break;
            case MessageKind::Invalid:
                output.has_request = true;
                append_error(output.responses, "null", -32600, "Invalid Request");
                break;
        }
    }
    session->touch(event::EventLoop::current().now());
    co_return output;
}

bool McpProtocol::supported_version(std::string_view version) noexcept {
    return version == kMcpLatestProtocolVersion || version == kMcpOldProtocolVersion;
}

std::string McpProtocol::tools_list_changed_notification() {
    return "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/list_changed\"}";
}

std::string McpProtocol::ping_request(std::uint64_t id) {
    std::string output = "{\"jsonrpc\":\"2.0\",\"id\":";
    output.append(std::to_string(id));
    output.append(",\"method\":\"ping\",\"params\":{}}");
    return output;
}

} // namespace fiber::ai_server
