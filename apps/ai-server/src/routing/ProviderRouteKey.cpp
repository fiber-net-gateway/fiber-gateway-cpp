#include "ProviderRouteKey.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <fiber/common/json/Utf.h>

namespace fiber::ai_server {
namespace {

class PrefixBuilder {
public:
    PrefixBuilder(char *data, std::size_t capacity) noexcept : data_(data), capacity_(capacity) {}

    void append(std::string_view text) noexcept {
        std::size_t input = 0;
        while (input < text.size() && size_ < capacity_) {
            const std::size_t begin = input;
            std::uint32_t codepoint = 0;
            if (!json::utf8_next_codepoint(text.data(), text.size(), input, codepoint)) {
                return;
            }
            const std::size_t bytes = input - begin;
            if (bytes > capacity_ - size_) {
                return;
            }
            std::memcpy(data_ + size_, text.data() + begin, bytes);
            size_ += bytes;
        }
    }

    [[nodiscard]] bool full() const noexcept { return size_ == capacity_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] std::string_view view() const noexcept { return {data_, size_}; }

private:
    char *data_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t size_ = 0;
};

std::string_view present_nonempty(const json::Nullable<std::string_view> &value) noexcept {
    return value.is_present() && !value->empty() ? *value : std::string_view{};
}

void append_messages(PrefixBuilder &builder, const LlmRoutingData &routing) noexcept {
    for (std::size_t i = 0; i < routing.message_roles.size() && !builder.full(); ++i) {
        if (routing.message_roles[i].is_present()) {
            builder.append(*routing.message_roles[i]);
        }
        builder.append(":");
        if (i < routing.message_content_texts.size() && routing.message_content_texts[i].is_present()) {
            builder.append(*routing.message_content_texts[i]);
        }
        builder.append("\n");
    }
}

} // namespace

std::expected<std::string_view, ProviderRouteKeyError> build_provider_route_key(LlmWireProtocol protocol,
                                                                                const LlmRoutingData &routing,
                                                                                const LoadBalanceConfig &config,
                                                                                mem::BufPool &pool) noexcept {
    if (const std::string_view metadata = present_nonempty(routing.metadata_route_key); !metadata.empty()) {
        return metadata;
    }
    if (protocol == LlmWireProtocol::OpenAiChatCompletions) {
        if (const std::string_view cache_key = present_nonempty(routing.prompt_cache_key); !cache_key.empty()) {
            return cache_key;
        }
    } else {
        if (const std::string_view container = present_nonempty(routing.container); !container.empty()) {
            return container;
        }
    }

    const std::size_t limit = config.prefix_max_bytes > 0
                                      ? static_cast<std::size_t>(config.prefix_max_bytes)
                                      : static_cast<std::size_t>(LoadBalanceConfig::kDefaultPrefixMaxBytes);
    auto *storage = static_cast<char *>(pool.alloc(limit, alignof(char)));
    if (!storage) {
        return std::unexpected(ProviderRouteKeyError::OutOfMemory);
    }
    PrefixBuilder builder(storage, limit);
    if (protocol == LlmWireProtocol::AnthropicMessages && routing.system_text.is_present() &&
        !routing.system_text->empty()) {
        builder.append(*routing.system_text);
        if (!builder.empty()) {
            builder.append("\n");
        }
    }
    append_messages(builder, routing);
    if (!builder.empty()) {
        return builder.view();
    }
    if (routing.model.is_present()) {
        return *routing.model;
    }
    return std::string_view{};
}

} // namespace fiber::ai_server
