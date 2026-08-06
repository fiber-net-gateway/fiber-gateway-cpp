#include "McpJsonCodec.h"

#include <algorithm>
#include <limits>
#include <unordered_set>
#include <utility>

#include <common/json/JsonEncode.h>
#include <common/json/JsonParse.h>
#include <common/json/JsonParser.h>
#include <common/mem/BufPool.h>

namespace fiber::ai_server {
namespace {

constexpr std::size_t kMaxConfigBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxNameBytes = 128;
constexpr std::size_t kMaxScriptIdBytes = 256;
constexpr std::string_view kCachePrefix = "```\n";
constexpr std::string_view kCacheSeparator = "\n```\n";

class BoundedStringSink final : public json::OutputSink {
public:
    BoundedStringSink(std::string &output, std::size_t max_bytes) noexcept : output_(&output), max_bytes_(max_bytes) {}

    bool write(const char *data, std::size_t len) noexcept override {
        if (len > max_bytes_ - std::min(max_bytes_, output_->size())) {
            failed_ = true;
            return false;
        }
        output_->append(data, len);
        return true;
    }

    [[nodiscard]] bool failed() const noexcept { return failed_; }

private:
    std::string *output_ = nullptr;
    std::size_t max_bytes_ = 0;
    bool failed_ = false;
};

McpJsonError error(McpJsonErrorCode code, std::string field, std::string message, std::size_t offset = 0) {
    return McpJsonError{
            .code = code,
            .offset = offset,
            .field = std::move(field),
            .message = std::move(message),
    };
}

std::expected<json::JsonAny, McpJsonError> parse_document(std::string_view content, mem::BufPool &pool) {
    if (content.size() > kMaxConfigBytes) {
        return std::unexpected(error(McpJsonErrorCode::TooLarge, {}, "JSON document is too large"));
    }
    json::JsonParser parser;
    if (!parser.feed(content.data(), content.size())) {
        const json::ParseError &parse_error = parser.error();
        return std::unexpected(error(McpJsonErrorCode::InvalidJson, {},
                                     parse_error.message ? parse_error.message : "invalid JSON", parse_error.offset));
    }
    parser.finish();
    json::JsonAny root;
    if (json::parse_document(
                parser, pool, root,
                [](json::JsonParser &value_parser, mem::BufPool &value_pool, json::JsonAny &value) noexcept {
                    return json::parse_any(value_parser, value_pool, value);
                }) != json::ParseStatus::Done) {
        const json::ParseError &parse_error = parser.error();
        return std::unexpected(error(McpJsonErrorCode::InvalidJson, {},
                                     parse_error.message ? parse_error.message : "invalid JSON", parse_error.offset));
    }
    return root;
}

const json::JsonAny *field(const json::JsonObject<json::JsonAny> &object, std::string_view name) noexcept {
    const auto *entry = object.find(name);
    return entry ? &entry->value : nullptr;
}

json::Generator::Result generate_any(json::Generator &generator, const json::JsonAny &value) noexcept {
    using Result = json::Generator::Result;
    switch (value.kind()) {
        case json::JsonAnyKind::Null:
            return generator.null_value();
        case json::JsonAnyKind::Bool:
            return generator.bool_value(value.as_bool());
        case json::JsonAnyKind::Integer:
            return generator.integer(value.as_integer());
        case json::JsonAnyKind::Double:
            return generator.double_value(value.as_double());
        case json::JsonAnyKind::Text:
            return generator.string(value.as_text().data(), value.as_text().size());
        case json::JsonAnyKind::Array: {
            Result result = generator.array_open();
            if (result != Result::OK) {
                return result;
            }
            for (const json::JsonAny &item: value.as_array()) {
                result = generate_any(generator, item);
                if (result != Result::OK) {
                    return result;
                }
            }
            return generator.array_close();
        }
        case json::JsonAnyKind::Object: {
            Result result = generator.map_open();
            if (result != Result::OK) {
                return result;
            }
            for (const auto &entry: value.as_object()) {
                result = generator.string(entry.key.data(), entry.key.size());
                if (result != Result::OK) {
                    return result;
                }
                result = generate_any(generator, entry.value);
                if (result != Result::OK) {
                    return result;
                }
            }
            return generator.map_close();
        }
    }
    return Result::ErrorState;
}

bool valid_token(std::string_view name, std::size_t max_length, bool allow_dot) noexcept {
    if (name.empty() || name.size() > max_length) {
        return false;
    }
    for (const unsigned char ch: name) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' ||
            ch == '-' || (allow_dot && ch == '.')) {
            continue;
        }
        return false;
    }
    return true;
}

std::expected<McpLoadedTool, McpJsonError>
parse_tool_json(std::string_view content, std::string_view expected_script_id, std::string_view cached_script) {
    mem::BufPool pool;
    auto parsed = parse_document(content, pool);
    if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    if (!parsed->is_object()) {
        return std::unexpected(error(McpJsonErrorCode::InvalidField, {}, "tool document must be an object"));
    }
    const auto &root = parsed->as_object();
    const json::JsonAny *id = field(root, "id");
    if (!id) {
        id = field(root, "scriptId");
    }
    if (!id || !id->is_text() || id->as_text() != expected_script_id || !valid_mcp_script_id(id->as_text())) {
        return std::unexpected(error(McpJsonErrorCode::InvalidField, "id", "tool id does not match request"));
    }
    const json::JsonAny *script = field(root, "script");
    std::string_view script_source = cached_script;
    if (script_source.empty()) {
        if (!script || !script->is_text() || script->as_text().empty()) {
            return std::unexpected(error(McpJsonErrorCode::MissingField, "script", "tool script is required"));
        }
        script_source = script->as_text();
    }
    const json::JsonAny *tool = field(root, "tool");
    if (!tool || !tool->is_object()) {
        return std::unexpected(error(McpJsonErrorCode::MissingField, "tool", "tool description is required"));
    }
    const auto &tool_object = tool->as_object();
    const json::JsonAny *name = field(tool_object, "name");
    if (!name || !name->is_text() || !valid_token(name->as_text(), kMaxNameBytes, true)) {
        return std::unexpected(error(McpJsonErrorCode::InvalidField, "tool.name", "invalid tool name"));
    }
    const json::JsonAny *description = field(tool_object, "description");
    if (description && !description->is_null() && !description->is_text()) {
        return std::unexpected(error(McpJsonErrorCode::InvalidField, "tool.description", "expected string or null"));
    }
    const json::JsonAny *input_schema = field(tool_object, "inputSchema");
    if (!input_schema || !input_schema->is_object()) {
        return std::unexpected(
                error(McpJsonErrorCode::InvalidField, "tool.inputSchema", "input schema must be an object"));
    }

    McpLoadedTool loaded;
    loaded.descriptor.script_id = std::string(expected_script_id);
    loaded.descriptor.name = std::string(name->as_text());
    if (description && description->is_text()) {
        loaded.descriptor.description = std::string(description->as_text());
    }
    if (!encode_json_any(*input_schema, loaded.descriptor.input_schema_json)) {
        return std::unexpected(error(McpJsonErrorCode::TooLarge, "tool.inputSchema", "input schema is too large"));
    }
    if (!encode_json_any(*tool, loaded.descriptor.tool_json)) {
        return std::unexpected(error(McpJsonErrorCode::TooLarge, "tool", "tool description is too large"));
    }
    loaded.script = std::string(script_source);
    return loaded;
}

} // namespace

std::expected<McpNameSetConfig, McpJsonError> parse_mcp_name_set_config(std::string_view content, bool project_names) {
    mem::BufPool pool;
    auto parsed = parse_document(content, pool);
    if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    if (!parsed->is_object()) {
        return std::unexpected(error(McpJsonErrorCode::InvalidEnvelope, {}, "configuration must be an object"));
    }
    const auto &root = parsed->as_object();
    const json::JsonAny *version = field(root, "version");
    const json::JsonAny *data = field(root, "data");
    if (!version || !version->is_integer() || version->as_integer() < std::numeric_limits<std::int32_t>::min() ||
        version->as_integer() > std::numeric_limits<std::int32_t>::max()) {
        return std::unexpected(error(McpJsonErrorCode::InvalidField, "version", "expected 32-bit integer"));
    }
    if (!data || !data->is_array()) {
        return std::unexpected(error(McpJsonErrorCode::InvalidField, "data", "expected string array"));
    }

    McpNameSetConfig result;
    result.version = static_cast<std::int32_t>(version->as_integer());
    result.names.reserve(data->as_array().size());
    std::unordered_set<std::string> unique;
    for (const json::JsonAny &item: data->as_array()) {
        if (!item.is_text() ||
            !(project_names ? valid_mcp_project_name(item.as_text()) : valid_mcp_script_id(item.as_text()))) {
            return std::unexpected(error(McpJsonErrorCode::InvalidField, "data", "contains invalid name"));
        }
        std::string name(item.as_text());
        if (!unique.insert(name).second) {
            return std::unexpected(error(McpJsonErrorCode::DuplicateValue, "data", "contains duplicate name"));
        }
        result.names.push_back(std::move(name));
    }
    std::sort(result.names.begin(), result.names.end());
    return result;
}

std::expected<McpLoadedTool, McpJsonError> parse_mcp_admin_tool(std::string_view content,
                                                                std::string_view expected_script_id) {
    return parse_tool_json(content, expected_script_id, {});
}

std::expected<McpLoadedTool, McpJsonError> parse_mcp_tool_cache(std::string_view content,
                                                                std::string_view expected_script_id) {
    if (!content.starts_with(kCachePrefix)) {
        return std::unexpected(error(McpJsonErrorCode::InvalidField, {}, "cache does not start with script fence"));
    }
    const std::size_t separator = content.find(kCacheSeparator, kCachePrefix.size());
    if (separator == std::string_view::npos) {
        return std::unexpected(error(McpJsonErrorCode::InvalidField, {}, "cache script fence is incomplete"));
    }
    const std::string_view script = content.substr(kCachePrefix.size(), separator - kCachePrefix.size());
    const std::string_view metadata = content.substr(separator + kCacheSeparator.size());
    return parse_tool_json(metadata, expected_script_id, script);
}

std::string encode_mcp_tool_cache(const McpLoadedTool &tool) {
    std::string output;
    output.reserve(tool.script.size() + tool.descriptor.tool_json.size() + tool.descriptor.script_id.size() + 64);
    output.append(kCachePrefix);
    output.append(tool.script);
    output.append(kCacheSeparator);
    output.append("{\"id\":");
    (void) append_json_string(output, tool.descriptor.script_id);
    output.append(",\"tool\":");
    output.append(tool.descriptor.tool_json);
    output.push_back('}');
    return output;
}

bool encode_json_any(const json::JsonAny &value, std::string &output, std::size_t max_bytes) noexcept {
    BoundedStringSink sink(output, max_bytes);
    json::Generator generator(sink);
    const auto result = generate_any(generator, value);
    return !sink.failed() &&
           (result == json::Generator::Result::OK || result == json::Generator::Result::GenerateComplete);
}

bool append_json_string(std::string &output, std::string_view value, std::size_t max_bytes) noexcept {
    if (output.size() >= max_bytes) {
        return false;
    }
    BoundedStringSink sink(output, max_bytes);
    json::Generator generator(sink);
    const auto result = generator.string(value.data(), value.size());
    return !sink.failed() &&
           (result == json::Generator::Result::OK || result == json::Generator::Result::GenerateComplete);
}

bool valid_mcp_project_name(std::string_view name) noexcept { return valid_token(name, kMaxNameBytes, false); }

bool valid_mcp_script_id(std::string_view name) noexcept { return valid_token(name, kMaxScriptIdBytes, true); }

} // namespace fiber::ai_server
