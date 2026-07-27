#ifndef FIBER_AI_SERVER_LLM_BODY_H
#define FIBER_AI_SERVER_LLM_BODY_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <utility>

#include <common/json/JsonValue.h>
#include <common/mem/BufPool.h>
#include <common/mem/IoBuf.h>
#include <common/mem/IoBufChain.h>

namespace fiber::ai_server {

enum class LlmWireProtocol : std::uint8_t {
    OpenAiChatCompletions,
    AnthropicMessages,
};

enum class LlmBodyErrorCode : std::uint8_t {
    InvalidJson,
    ExpectedObject,
    InvalidFieldType,
    OutOfMemory,
    InvalidReplacement,
    InvalidPatch,
};

struct LlmBodyError {
    LlmBodyErrorCode code = LlmBodyErrorCode::InvalidJson;
    std::size_t offset = 0;
    std::string_view field;
    const char *message = nullptr;
};

struct LlmRoutingData {
    json::Nullable<std::string_view> model;
    json::Nullable<bool> stream;
    json::Nullable<std::string_view> metadata_route_key;
    json::Nullable<std::string_view> container;
    json::Nullable<std::string_view> prompt_cache_key;
    json::Nullable<std::string_view> system_text;
    json::JsonArray<json::Nullable<std::string_view>> message_roles;
    json::JsonArray<json::Nullable<std::string_view>> message_content_texts;
    std::size_t messages_count = 0;
    std::size_t tools_count = 0;
};

enum class LlmBodyPatchKind : std::uint8_t {
    Model,
    Stream,
};

struct LlmBodyPatchSite {
    std::size_t begin = 0;
    std::size_t end = 0;
    LlmBodyPatchKind kind = LlmBodyPatchKind::Model;
};

class ParsedLlmBody {
public:
    ParsedLlmBody() noexcept = default;
    ~ParsedLlmBody() = default;

    ParsedLlmBody(const ParsedLlmBody &) = delete;
    ParsedLlmBody &operator=(const ParsedLlmBody &) = delete;
    ParsedLlmBody(ParsedLlmBody &&) noexcept = default;
    ParsedLlmBody &operator=(ParsedLlmBody &&) noexcept = default;

    [[nodiscard]] static std::expected<ParsedLlmBody, LlmBodyError> parse(LlmWireProtocol protocol, mem::IoBuf body,
                                                                          mem::BufPool &pool) noexcept;

    [[nodiscard]] const LlmRoutingData &routing() const noexcept { return routing_; }
    [[nodiscard]] const mem::IoBuf &raw_body() const noexcept { return raw_body_; }
    [[nodiscard]] std::size_t body_size() const noexcept { return raw_body_.readable(); }
    [[nodiscard]] const json::JsonArray<LlmBodyPatchSite> &patch_sites() const noexcept { return patch_sites_; }

    [[nodiscard]] std::expected<mem::IoBufChain, LlmBodyError>
    rewrite(std::string_view upstream_model, std::optional<bool> stream, mem::IoBufNodePool &node_pool) const noexcept;

private:
    ParsedLlmBody(mem::IoBuf body, LlmRoutingData routing, json::JsonArray<LlmBodyPatchSite> patch_sites) noexcept :
        raw_body_(std::move(body)), routing_(routing), patch_sites_(patch_sites) {}

    mem::IoBuf raw_body_;
    LlmRoutingData routing_;
    json::JsonArray<LlmBodyPatchSite> patch_sites_;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_LLM_BODY_H
