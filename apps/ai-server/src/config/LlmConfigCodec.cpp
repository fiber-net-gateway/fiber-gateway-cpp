#include "LlmConfigCodec.h"
#include "../provider/ProviderEndpoint.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

#include <common/json/JsonParse.h>
#include <common/json/JsonParser.h>
#include <common/json/JsonValue.h>
#include <common/mem/BufPool.h>
#include <common/util/Base64.h>

namespace fiber::ai_server {
namespace {

using json::JsonAny;
using json::JsonAnyKind;
using json::JsonArray;
using json::JsonObject;

struct Envelope {
    std::int32_t version = 0;
    const JsonAny *data = nullptr;
};

LlmConfigError make_error(LlmConfigErrorCode code, std::string field, std::string message, std::size_t offset = 0) {
    return LlmConfigError{
            .code = code,
            .offset = offset,
            .field = std::move(field),
            .message = std::move(message),
    };
}

LlmConfigError invalid_field(std::string_view field, std::string message) {
    return make_error(LlmConfigErrorCode::InvalidField, std::string(field), std::move(message));
}

LlmConfigError missing_field(std::string_view field) {
    return make_error(LlmConfigErrorCode::MissingField, std::string(field), "required field is missing");
}

std::expected<JsonAny, LlmConfigError> parse_json_document(std::string_view content, mem::BufPool &pool,
                                                           json::JsonParser &parser) {
    if (!parser.feed(content.data(), content.size())) {
        const json::ParseError &error = parser.error();
        return std::unexpected(
                make_error(LlmConfigErrorCode::InvalidJson, {}, error.message ? error.message : "", error.offset));
    }
    parser.finish();
    JsonAny value;
    const auto status = json::parse_document(
            parser, pool, value, [](json::JsonParser &value_parser, mem::BufPool &value_pool, JsonAny &out) noexcept {
                return json::parse_any(value_parser, value_pool, out);
            });
    if (status != json::ParseStatus::Done) {
        const json::ParseError &error = parser.error();
        return std::unexpected(
                make_error(LlmConfigErrorCode::InvalidJson, {}, error.message ? error.message : "", error.offset));
    }
    return value;
}

const JsonAny *find_alias(const JsonObject<JsonAny> &object, std::initializer_list<std::string_view> names) noexcept {
    const JsonAny *found = nullptr;
    for (const auto &entry: object) {
        for (const std::string_view name: names) {
            if (entry.key == name) {
                found = &entry.value;
                break;
            }
        }
    }
    return found;
}

std::expected<const JsonObject<JsonAny> *, LlmConfigError> require_object(const JsonAny *value,
                                                                          std::string_view field) {
    if (!value) {
        return std::unexpected(missing_field(field));
    }
    if (!value->is_object()) {
        return std::unexpected(invalid_field(field, "expected object"));
    }
    return &value->as_object();
}

std::expected<const JsonArray<JsonAny> *, LlmConfigError> require_array(const JsonAny *value, std::string_view field) {
    if (!value) {
        return std::unexpected(missing_field(field));
    }
    if (!value->is_array()) {
        return std::unexpected(invalid_field(field, "expected array"));
    }
    return &value->as_array();
}

std::expected<std::string_view, LlmConfigError>
require_text(const JsonObject<JsonAny> &object, std::initializer_list<std::string_view> names, std::string_view field) {
    const JsonAny *value = find_alias(object, names);
    if (!value) {
        return std::unexpected(missing_field(field));
    }
    if (!value->is_text()) {
        return std::unexpected(invalid_field(field, "expected string"));
    }
    return value->as_text();
}

std::expected<std::optional<std::string_view>, LlmConfigError>
optional_text(const JsonObject<JsonAny> &object, std::initializer_list<std::string_view> names,
              std::string_view field) {
    const JsonAny *value = find_alias(object, names);
    if (!value || value->is_null()) {
        return std::optional<std::string_view>{};
    }
    if (!value->is_text()) {
        return std::unexpected(invalid_field(field, "expected string or null"));
    }
    return std::optional<std::string_view>(value->as_text());
}

std::expected<std::optional<std::int64_t>, LlmConfigError>
optional_integer(const JsonObject<JsonAny> &object, std::initializer_list<std::string_view> names,
                 std::string_view field) {
    const JsonAny *value = find_alias(object, names);
    if (!value || value->is_null()) {
        return std::optional<std::int64_t>{};
    }
    if (!value->is_integer()) {
        return std::unexpected(invalid_field(field, "expected integer or null"));
    }
    return std::optional<std::int64_t>(value->as_integer());
}

std::expected<std::optional<bool>, LlmConfigError> optional_bool(const JsonObject<JsonAny> &object,
                                                                 std::initializer_list<std::string_view> names,
                                                                 std::string_view field) {
    const JsonAny *value = find_alias(object, names);
    if (!value || value->is_null()) {
        return std::optional<bool>{};
    }
    if (!value->is_bool()) {
        return std::unexpected(invalid_field(field, "expected boolean or null"));
    }
    return std::optional<bool>(value->as_bool());
}

std::expected<Envelope, LlmConfigError> parse_envelope(const JsonAny &root) {
    if (!root.is_object()) {
        return std::unexpected(
                make_error(LlmConfigErrorCode::InvalidEnvelope, {}, "configuration envelope must be an object"));
    }
    const JsonObject<JsonAny> &object = root.as_object();
    Envelope envelope;
    const JsonAny *version = find_alias(object, {"version"});
    if (version && !version->is_null()) {
        if (!version->is_integer() || version->as_integer() < std::numeric_limits<std::int32_t>::min() ||
            version->as_integer() > std::numeric_limits<std::int32_t>::max()) {
            return std::unexpected(invalid_field("version", "expected 32-bit integer"));
        }
        envelope.version = static_cast<std::int32_t>(version->as_integer());
    }
    envelope.data = find_alias(object, {"data"});
    return envelope;
}

ConfigMetadata metadata(std::string_view data_id, std::string_view md5, std::int32_t version) {
    return ConfigMetadata{
            .data_id = std::string(data_id),
            .group = std::string(kLlmConfigGroup),
            .md5 = std::string(md5),
            .version = version,
    };
}

bool valid_name(std::string_view name, std::size_t max_length, bool allow_dot) noexcept {
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

std::string normalize_option(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    std::string normalized(value);
    for (char &ch: normalized) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return normalized;
}

std::expected<ProviderProtocolType, LlmConfigError> parse_protocol_type(std::string_view value,
                                                                        std::string_view field) {
    if (value == "openai-chat-completions") {
        return ProviderProtocolType::OpenAiChatCompletions;
    }
    if (value == "openai-embedding") {
        return ProviderProtocolType::OpenAiEmbedding;
    }
    if (value == "anthropic-messages") {
        return ProviderProtocolType::AnthropicMessages;
    }
    return std::unexpected(invalid_field(field, "unsupported provider protocol type"));
}

std::expected<LoadBalanceConfig, LlmConfigError> parse_load_balance(const JsonAny *value, std::string_view base_path) {
    LoadBalanceConfig config;
    if (!value || value->is_null()) {
        return config;
    }
    auto object_result = require_object(value, base_path);
    if (!object_result) {
        return std::unexpected(std::move(object_result.error()));
    }
    const JsonObject<JsonAny> &object = **object_result;

    auto policy = optional_text(object, {"policy"}, std::string(base_path) + ".policy");
    if (!policy) {
        return std::unexpected(std::move(policy.error()));
    }
    if (*policy && normalize_option(**policy) != "rendezvous-hash") {
        // Java normalizes every unsupported policy to the only implemented one.
    }

    auto hash_source = optional_text(object, {"hash-source", "hashSource"}, std::string(base_path) + ".hash-source");
    if (!hash_source) {
        return std::unexpected(std::move(hash_source.error()));
    }
    if (*hash_source && normalize_option(**hash_source) != "prompt-prefix") {
        // Java normalizes every unsupported source to the only implemented one.
    }

    auto service_policy = optional_text(object, {"service-instance-policy", "serviceInstancePolicy"},
                                        std::string(base_path) + ".service-instance-policy");
    if (!service_policy) {
        return std::unexpected(std::move(service_policy.error()));
    }
    if (*service_policy) {
        const std::string normalized = normalize_option(**service_policy);
        if (normalized == "smooth-weighted-round-robin" || normalized == "smooth-round-robin") {
            return std::unexpected(invalid_field(std::string(base_path) + ".service-instance-policy",
                                                 "ai-server only supports weighted rendezvous service selection"));
        }
        if (normalized != "weighted-rendezvous" && normalized != "weighted-rendezvous-hash") {
            return std::unexpected(invalid_field(std::string(base_path) + ".service-instance-policy",
                                                 "unsupported service instance policy"));
        }
    }

    auto prefix = optional_integer(object, {"prefix-max-bytes", "prefixMaxBytes"},
                                   std::string(base_path) + ".prefix-max-bytes");
    if (!prefix) {
        return std::unexpected(std::move(prefix.error()));
    }
    if (*prefix && **prefix > 0) {
        if (**prefix > std::numeric_limits<std::int32_t>::max()) {
            return std::unexpected(
                    invalid_field(std::string(base_path) + ".prefix-max-bytes", "integer exceeds 32-bit range"));
        }
        config.prefix_max_bytes = static_cast<std::int32_t>(**prefix);
    }

    auto max_attempts = optional_integer(object, {"max-primary-attempts", "maxPrimaryAttempts"},
                                         std::string(base_path) + ".max-primary-attempts");
    if (!max_attempts) {
        return std::unexpected(std::move(max_attempts.error()));
    }
    if (*max_attempts && **max_attempts > 0) {
        if (**max_attempts > std::numeric_limits<std::int32_t>::max()) {
            return std::unexpected(
                    invalid_field(std::string(base_path) + ".max-primary-attempts", "integer exceeds 32-bit range"));
        }
        config.max_primary_attempts = static_cast<std::int32_t>(**max_attempts);
    }

    auto fallback = optional_bool(object, {"fallback-enabled", "fallbackEnabled"},
                                  std::string(base_path) + ".fallback-enabled");
    if (!fallback) {
        return std::unexpected(std::move(fallback.error()));
    }
    if (*fallback) {
        config.fallback_enabled = **fallback;
    }

    const JsonAny *statuses = find_alias(object, {"retryable-status", "retryableStatus"});
    if (statuses && !statuses->is_null()) {
        auto array_result = require_array(statuses, std::string(base_path) + ".retryable-status");
        if (!array_result) {
            return std::unexpected(std::move(array_result.error()));
        }
        if (!(**array_result).empty()) {
            config.retryable_statuses.clear();
            for (std::size_t i = 0; i < (**array_result).size(); ++i) {
                const JsonAny &item = (**array_result)[i];
                if (!item.is_integer() || item.as_integer() < std::numeric_limits<std::int32_t>::min() ||
                    item.as_integer() > std::numeric_limits<std::int32_t>::max()) {
                    return std::unexpected(
                            invalid_field(std::string(base_path) + ".retryable-status[" + std::to_string(i) + "]",
                                          "expected 32-bit integer"));
                }
                config.retryable_statuses.push_back(static_cast<std::int32_t>(item.as_integer()));
            }
            std::sort(config.retryable_statuses.begin(), config.retryable_statuses.end());
            config.retryable_statuses.erase(
                    std::unique(config.retryable_statuses.begin(), config.retryable_statuses.end()),
                    config.retryable_statuses.end());
        }
    }
    return config;
}

std::expected<std::optional<ModelRateLimitConfig>, LlmConfigError> parse_rate_limit(const JsonAny *value,
                                                                                    std::string_view base_path) {
    if (!value || value->is_null()) {
        return std::optional<ModelRateLimitConfig>{};
    }
    auto object_result = require_object(value, base_path);
    if (!object_result) {
        return std::unexpected(std::move(object_result.error()));
    }
    const JsonObject<JsonAny> &object = **object_result;
    auto window = optional_integer(object, {"window-duration-millis", "windowDurationMillis"},
                                   std::string(base_path) + ".window-duration-millis");
    if (!window) {
        return std::unexpected(std::move(window.error()));
    }
    if (!*window) {
        return std::unexpected(missing_field(std::string(base_path) + ".window-duration-millis"));
    }
    if (**window <= 0) {
        return std::unexpected(
                invalid_field(std::string(base_path) + ".window-duration-millis", "must be greater than zero"));
    }

    auto max_tokens = optional_integer(object, {"max-tokens-per-window", "maxTokensPerWindow"},
                                       std::string(base_path) + ".max-tokens-per-window");
    if (!max_tokens) {
        return std::unexpected(std::move(max_tokens.error()));
    }
    if (!*max_tokens) {
        return std::unexpected(missing_field(std::string(base_path) + ".max-tokens-per-window"));
    }
    if (**max_tokens < 0) {
        return std::unexpected(
                invalid_field(std::string(base_path) + ".max-tokens-per-window", "must be non-negative"));
    }
    return std::optional<ModelRateLimitConfig>(
            ModelRateLimitConfig{.window_duration_millis = **window, .max_tokens_per_window = **max_tokens});
}

} // namespace

bool valid_provider_name(std::string_view name) noexcept { return valid_name(name, 128, false); }

bool valid_user_group_name(std::string_view name) noexcept { return valid_name(name, 64, false); }

bool valid_model_name(std::string_view name) noexcept { return valid_name(name, 128, true); }

std::expected<Bt1KeySnapshot, LlmConfigError> parse_bt1_key_config(std::string_view content, std::string_view md5) {
    mem::BufPool pool;
    json::JsonParser parser;
    auto root = parse_json_document(content, pool, parser);
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    auto envelope = parse_envelope(*root);
    if (!envelope) {
        return std::unexpected(std::move(envelope.error()));
    }
    auto data_result = require_object(envelope->data, "data");
    if (!data_result) {
        return std::unexpected(std::move(data_result.error()));
    }
    const JsonObject<JsonAny> &data = **data_result;

    Bt1KeySnapshot snapshot;
    snapshot.metadata = metadata(kBt1KeysDataId, md5, envelope->version);
    auto skew = optional_integer(data, {"clockSkewSec"}, "data.clockSkewSec");
    if (!skew) {
        return std::unexpected(std::move(skew.error()));
    }
    if (*skew) {
        if (**skew < 0 || **skew > 300) {
            return std::unexpected(invalid_field("data.clockSkewSec", "must be in range 0..300"));
        }
        snapshot.clock_skew_seconds = static_cast<std::int32_t>(**skew);
    }

    auto keys_result = require_array(find_alias(data, {"keys"}), "data.keys");
    if (!keys_result) {
        return std::unexpected(std::move(keys_result.error()));
    }
    if ((**keys_result).empty()) {
        return std::unexpected(invalid_field("data.keys", "must contain at least one key"));
    }
    std::unordered_set<std::string> seen;
    snapshot.keys.reserve((**keys_result).size());
    for (std::size_t i = 0; i < (**keys_result).size(); ++i) {
        const std::string path = "data.keys[" + std::to_string(i) + "]";
        auto item_result = require_object(&(**keys_result)[i], path);
        if (!item_result) {
            return std::unexpected(std::move(item_result.error()));
        }
        auto kid = require_text(**item_result, {"kid"}, path + ".kid");
        if (!kid) {
            return std::unexpected(std::move(kid.error()));
        }
        if (!valid_name(*kid, 16, false)) {
            return std::unexpected(invalid_field(path + ".kid", "invalid BT1 key id"));
        }
        if (!seen.emplace(*kid).second) {
            return std::unexpected(
                    make_error(LlmConfigErrorCode::DuplicateValue, path + ".kid", "duplicate BT1 key id"));
        }
        auto secret = require_text(**item_result, {"secret"}, path + ".secret");
        if (!secret) {
            return std::unexpected(std::move(secret.error()));
        }
        if (secret->empty()) {
            return std::unexpected(invalid_field(path + ".secret", "BT1 secret must not be empty"));
        }
        std::string decoded;
        if (secret->starts_with("base64:")) {
            if (!util::base64_decode(secret->substr(7), decoded) || decoded.empty()) {
                return std::unexpected(invalid_field(path + ".secret", "invalid or empty standard Base64 secret"));
            }
        } else {
            decoded.assign(*secret);
        }
        snapshot.keys.push_back(Bt1Key{.kid = std::string(*kid), .secret = std::move(decoded)});
    }
    std::sort(snapshot.keys.begin(), snapshot.keys.end(),
              [](const Bt1Key &left, const Bt1Key &right) { return left.kid < right.kid; });
    return snapshot;
}

std::expected<UserGroupSnapshot, LlmConfigError> parse_user_group_config(std::string_view content, std::string_view md5,
                                                                         std::string_view expected_name) {
    if (!valid_user_group_name(expected_name)) {
        return std::unexpected(invalid_field("dataId", "invalid user group name"));
    }
    mem::BufPool pool;
    json::JsonParser parser;
    auto root = parse_json_document(content, pool, parser);
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    auto envelope = parse_envelope(*root);
    if (!envelope) {
        return std::unexpected(std::move(envelope.error()));
    }
    auto data_result = require_object(envelope->data, "data");
    if (!data_result) {
        return std::unexpected(std::move(data_result.error()));
    }
    auto name = require_text(**data_result, {"name"}, "data.name");
    if (!name) {
        return std::unexpected(std::move(name.error()));
    }
    if (*name != expected_name) {
        return std::unexpected(invalid_field("data.name", "must match the user group Data ID suffix"));
    }

    UserGroupSnapshot snapshot;
    snapshot.metadata =
            metadata(std::string(kUserGroupDataIdPrefix) + std::string(expected_name), md5, envelope->version);
    snapshot.name = std::string(expected_name);

    const JsonAny *users = find_alias(**data_result, {"users"});
    if (users && !users->is_null()) {
        auto users_result = require_array(users, "data.users");
        if (!users_result) {
            return std::unexpected(std::move(users_result.error()));
        }
        snapshot.users.reserve((**users_result).size());
        for (std::size_t i = 0; i < (**users_result).size(); ++i) {
            const JsonAny &item = (**users_result)[i];
            if (!item.is_text()) {
                return std::unexpected(invalid_field("data.users[" + std::to_string(i) + "]", "expected string"));
            }
            if (!item.as_text().empty()) {
                snapshot.users.emplace_back(item.as_text());
            }
        }
        std::sort(snapshot.users.begin(), snapshot.users.end());
        snapshot.users.erase(std::unique(snapshot.users.begin(), snapshot.users.end()), snapshot.users.end());
    }
    return snapshot;
}

std::expected<ProviderConfigSnapshot, LlmConfigError>
parse_provider_config(std::string_view content, std::string_view md5, std::string_view expected_name) {
    if (!valid_provider_name(expected_name)) {
        return std::unexpected(invalid_field("dataId", "invalid provider name"));
    }
    mem::BufPool pool;
    json::JsonParser parser;
    auto root = parse_json_document(content, pool, parser);
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    auto envelope = parse_envelope(*root);
    if (!envelope) {
        return std::unexpected(std::move(envelope.error()));
    }
    auto data_result = require_object(envelope->data, "data");
    if (!data_result) {
        return std::unexpected(std::move(data_result.error()));
    }
    const JsonObject<JsonAny> &data = **data_result;
    auto name = require_text(data, {"provider"}, "data.provider");
    if (!name) {
        return std::unexpected(std::move(name.error()));
    }
    if (*name != expected_name) {
        return std::unexpected(invalid_field("data.provider", "must match the provider Data ID suffix"));
    }
    auto base_url = require_text(data, {"baseurl", "baseUrl"}, "data.baseurl");
    if (!base_url) {
        return std::unexpected(std::move(base_url.error()));
    }
    std::string normalized_url(*base_url);
    while (normalized_url.size() > 1 && normalized_url.back() == '/') {
        normalized_url.pop_back();
    }
    auto parsed_endpoint = parse_provider_endpoint(normalized_url);
    if (!parsed_endpoint) {
        return std::unexpected(invalid_field("data.baseurl", parsed_endpoint.error().message));
    }

    ProviderConfigSnapshot snapshot;
    snapshot.metadata =
            metadata(std::string(kProviderDataIdPrefix) + std::string(expected_name), md5, envelope->version);
    snapshot.name = std::string(expected_name);
    snapshot.base_url = std::move(normalized_url);

    const JsonAny *tokens = find_alias(data, {"api-tokens", "apiTokens"});
    if (tokens && !tokens->is_null()) {
        auto tokens_result = require_array(tokens, "data.api-tokens");
        if (!tokens_result) {
            return std::unexpected(std::move(tokens_result.error()));
        }
        std::unordered_set<std::string> seen;
        snapshot.api_tokens.reserve((**tokens_result).size());
        for (std::size_t i = 0; i < (**tokens_result).size(); ++i) {
            const std::string path = "data.api-tokens[" + std::to_string(i) + "]";
            auto item = require_object(&(**tokens_result)[i], path);
            if (!item) {
                return std::unexpected(std::move(item.error()));
            }
            auto token_name = require_text(**item, {"name"}, path + ".name");
            if (!token_name) {
                return std::unexpected(std::move(token_name.error()));
            }
            auto token = require_text(**item, {"token"}, path + ".token");
            if (!token) {
                return std::unexpected(std::move(token.error()));
            }
            if (token_name->empty() || token->empty()) {
                return std::unexpected(invalid_field(path, "token name and value must not be empty"));
            }
            if (!seen.emplace(*token_name).second) {
                return std::unexpected(make_error(LlmConfigErrorCode::DuplicateValue, path + ".name",
                                                  "duplicate provider token name"));
            }
            snapshot.api_tokens.push_back(
                    ProviderApiToken{.name = std::string(*token_name), .token = std::string(*token)});
        }
    }

    auto protocols_result = require_array(find_alias(data, {"protocol", "protocols"}), "data.protocol");
    if (!protocols_result) {
        return std::unexpected(std::move(protocols_result.error()));
    }
    if ((**protocols_result).empty()) {
        return std::unexpected(invalid_field("data.protocol", "must contain at least one protocol"));
    }
    std::unordered_set<std::uint8_t> seen_protocols;
    snapshot.protocols.reserve((**protocols_result).size());
    for (std::size_t i = 0; i < (**protocols_result).size(); ++i) {
        const std::string base_path = "data.protocol[" + std::to_string(i) + "]";
        auto item = require_object(&(**protocols_result)[i], base_path);
        if (!item) {
            return std::unexpected(std::move(item.error()));
        }
        auto type_text = require_text(**item, {"type"}, base_path + ".type");
        if (!type_text) {
            return std::unexpected(std::move(type_text.error()));
        }
        auto type = parse_protocol_type(*type_text, base_path + ".type");
        if (!type) {
            return std::unexpected(std::move(type.error()));
        }
        const auto type_key = static_cast<std::uint8_t>(*type);
        if (!seen_protocols.emplace(type_key).second) {
            return std::unexpected(make_error(LlmConfigErrorCode::DuplicateValue, base_path + ".type",
                                              "duplicate provider protocol type"));
        }
        auto path = require_text(**item, {"path"}, base_path + ".path");
        if (!path) {
            return std::unexpected(std::move(path.error()));
        }
        auto model = require_text(**item, {"model"}, base_path + ".model");
        if (!model) {
            return std::unexpected(std::move(model.error()));
        }
        if (path->empty() || path->front() != '/') {
            return std::unexpected(invalid_field(base_path + ".path", "must start with '/'"));
        }
        if (model->empty()) {
            return std::unexpected(invalid_field(base_path + ".model", "must not be empty"));
        }
        snapshot.protocols.push_back(
                ProviderProtocol{.type = *type, .path = std::string(*path), .model = std::string(*model)});
    }
    return snapshot;
}

std::expected<ModelsConfigSnapshot, LlmConfigError> parse_models_config(std::string_view content,
                                                                        std::string_view md5) {
    mem::BufPool pool;
    json::JsonParser parser;
    auto root = parse_json_document(content, pool, parser);
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    auto envelope = parse_envelope(*root);
    if (!envelope) {
        return std::unexpected(std::move(envelope.error()));
    }
    ModelsConfigSnapshot snapshot;
    snapshot.metadata = metadata(kModelsDataId, md5, envelope->version);
    if (!envelope->data || envelope->data->is_null()) {
        return snapshot;
    }
    auto models_result = require_array(envelope->data, "data");
    if (!models_result) {
        return std::unexpected(std::move(models_result.error()));
    }

    std::unordered_set<std::string> seen_models;
    snapshot.models.reserve((**models_result).size());
    for (std::size_t i = 0; i < (**models_result).size(); ++i) {
        const std::string base_path = "data[" + std::to_string(i) + "]";
        auto item = require_object(&(**models_result)[i], base_path);
        if (!item) {
            return std::unexpected(std::move(item.error()));
        }
        auto model_name = require_text(**item, {"model-name", "modelName"}, base_path + ".model-name");
        if (!model_name) {
            return std::unexpected(std::move(model_name.error()));
        }
        if (!valid_model_name(*model_name)) {
            return std::unexpected(invalid_field(base_path + ".model-name", "invalid model name"));
        }
        if (!seen_models.emplace(*model_name).second) {
            return std::unexpected(
                    make_error(LlmConfigErrorCode::DuplicateValue, base_path + ".model-name", "duplicate model name"));
        }

        ModelDefinition definition;
        definition.model_name = std::string(*model_name);
        const JsonAny *providers = find_alias(**item, {"providers"});
        if (providers && !providers->is_null()) {
            auto providers_result = require_array(providers, base_path + ".providers");
            if (!providers_result) {
                return std::unexpected(std::move(providers_result.error()));
            }
            std::unordered_set<std::string> seen;
            definition.providers.reserve((**providers_result).size());
            for (std::size_t provider_index = 0; provider_index < (**providers_result).size(); ++provider_index) {
                const JsonAny &provider = (**providers_result)[provider_index];
                const std::string path = base_path + ".providers[" + std::to_string(provider_index) + "]";
                if (!provider.is_text() || !valid_provider_name(provider.as_text())) {
                    return std::unexpected(invalid_field(path, "invalid provider name"));
                }
                if (!seen.emplace(provider.as_text()).second) {
                    return std::unexpected(
                            make_error(LlmConfigErrorCode::DuplicateValue, path, "duplicate model provider"));
                }
                definition.providers.emplace_back(provider.as_text());
            }
        }

        auto fallback =
                optional_text(**item, {"fallback-provider", "fallbackProvider"}, base_path + ".fallback-provider");
        if (!fallback) {
            return std::unexpected(std::move(fallback.error()));
        }
        if (*fallback && !(**fallback).empty()) {
            if (!valid_provider_name(**fallback)) {
                return std::unexpected(
                        invalid_field(base_path + ".fallback-provider", "invalid fallback provider name"));
            }
            if (std::find(definition.providers.begin(), definition.providers.end(), **fallback) !=
                definition.providers.end()) {
                return std::unexpected(make_error(LlmConfigErrorCode::DuplicateValue, base_path + ".fallback-provider",
                                                  "fallback provider duplicates a primary provider"));
            }
            definition.fallback_provider = std::string(**fallback);
        }
        if (definition.providers.empty() && !definition.fallback_provider) {
            return std::unexpected(
                    invalid_field(base_path + ".providers", "model requires a primary or fallback provider"));
        }

        const JsonAny *groups = find_alias(**item, {"allow-user-groups", "allowUserGroups"});
        if (groups && !groups->is_null()) {
            auto groups_result = require_array(groups, base_path + ".allow-user-groups");
            if (!groups_result) {
                return std::unexpected(std::move(groups_result.error()));
            }
            std::unordered_set<std::string> seen;
            for (std::size_t group_index = 0; group_index < (**groups_result).size(); ++group_index) {
                const JsonAny &group = (**groups_result)[group_index];
                const std::string path = base_path + ".allow-user-groups[" + std::to_string(group_index) + "]";
                if (!group.is_text() || !valid_user_group_name(group.as_text())) {
                    return std::unexpected(invalid_field(path, "invalid user group name"));
                }
                if (seen.emplace(group.as_text()).second) {
                    definition.allow_user_groups.emplace_back(group.as_text());
                }
            }
        }

        auto load_balance =
                parse_load_balance(find_alias(**item, {"load-balance", "loadBalance"}), base_path + ".load-balance");
        if (!load_balance) {
            return std::unexpected(std::move(load_balance.error()));
        }
        definition.load_balance = std::move(*load_balance);

        auto rate_limit = parse_rate_limit(find_alias(**item, {"rate-limit", "rateLimit"}), base_path + ".rate-limit");
        if (!rate_limit) {
            return std::unexpected(std::move(rate_limit.error()));
        }
        definition.rate_limit = std::move(*rate_limit);
        snapshot.models.push_back(std::move(definition));
    }
    return snapshot;
}

} // namespace fiber::ai_server
