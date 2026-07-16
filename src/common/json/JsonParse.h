#ifndef FIBER_JSONPARSE_H
#define FIBER_JSONPARSE_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include "../mem/BufPool.h"
#include "JsonParser.h"
#include "JsonValue.h"

namespace fiber::json {

enum class ParseStatus : std::uint8_t {
    Done,
    Error,
};

// Typed parsers require finish() to have been called. On success, scalar
// parsers remain on their token, while container parsers remain on the matching
// EndArr or EndObj token. Text and container storage is owned by pool.
[[nodiscard]] ParseStatus parse_null(JsonParser &parser, mem::BufPool &pool, std::nullptr_t &out) noexcept;
[[nodiscard]] ParseStatus parse_bool(JsonParser &parser, mem::BufPool &pool, bool &out) noexcept;
[[nodiscard]] ParseStatus parse_text(JsonParser &parser, mem::BufPool &pool, std::string_view &out) noexcept;
[[nodiscard]] ParseStatus parse_integer(JsonParser &parser, mem::BufPool &pool, std::int64_t &out) noexcept;
[[nodiscard]] ParseStatus parse_double(JsonParser &parser, mem::BufPool &pool, double &out) noexcept;
[[nodiscard]] ParseStatus skip_value(JsonParser &parser, mem::BufPool &pool, std::nullptr_t &out) noexcept;
[[nodiscard]] ParseStatus skip_value(JsonParser &parser, mem::BufPool &pool) noexcept;

namespace detail {

[[nodiscard]] inline ParseStatus fail(JsonParser &parser, const char *message) noexcept {
    (void) parser.fail(message);
    return ParseStatus::Error;
}

[[nodiscard]] inline ParseStatus require_finished(JsonParser &parser) noexcept {
    if (!parser.input_finished()) {
        return fail(parser, "typed JSON parse requires finished input");
    }
    return ParseStatus::Done;
}

[[nodiscard]] inline ParseStatus next_token(JsonParser &parser, const char *end_message) noexcept {
    switch (parser.next()) {
        case JsonParser::Status::Token:
            return ParseStatus::Done;
        case JsonParser::Status::NeedMore:
            return fail(parser, "typed JSON parse requires finished input");
        case JsonParser::Status::Complete:
            return fail(parser, end_message);
        case JsonParser::Status::Error:
            return ParseStatus::Error;
    }
    return fail(parser, "invalid parser state");
}

template<typename ParserFn, typename T>
[[nodiscard]] ParseStatus invoke_parser(ParserFn &&parser_fn, JsonParser &parser, mem::BufPool &pool, T &out) noexcept {
    static_assert(std::is_nothrow_invocable_r_v<ParseStatus, ParserFn, JsonParser &, mem::BufPool &, T &>,
                  "JSON value parser must be noexcept and return ParseStatus");
    return std::invoke(std::forward<ParserFn>(parser_fn), parser, pool, out);
}

template<typename T>
class PoolArrayBuilder {
    static_assert(std::is_trivially_copyable_v<T>, "pool-backed JSON values must be trivially copyable");

    static constexpr std::size_t ChunkTargetBytes = 1024;
    static constexpr std::size_t ChunkElements = sizeof(T) >= ChunkTargetBytes ? 1 : ChunkTargetBytes / sizeof(T);

    struct Chunk {
        Chunk *next = nullptr;
        std::size_t size = 0;
        alignas(T) unsigned char storage[sizeof(T) * ChunkElements];

        [[nodiscard]] T *values() noexcept { return reinterpret_cast<T *>(storage); }
        [[nodiscard]] const T *values() const noexcept { return reinterpret_cast<const T *>(storage); }
    };

public:
    explicit PoolArrayBuilder(mem::BufPool &pool) noexcept : pool_(&pool) {}

    [[nodiscard]] bool append(const T &value) noexcept {
        if (size_ == std::numeric_limits<std::size_t>::max()) {
            return false;
        }
        if (!tail_ || tail_->size == ChunkElements) {
            auto *chunk = static_cast<Chunk *>(pool_->alloc(sizeof(Chunk), alignof(Chunk)));
            if (!chunk) {
                return false;
            }
            std::construct_at(chunk);
            if (tail_) {
                tail_->next = chunk;
            } else {
                head_ = chunk;
            }
            tail_ = chunk;
        }

        std::memcpy(tail_->storage + tail_->size * sizeof(T), &value, sizeof(T));
        tail_->size += 1;
        size_ += 1;
        return true;
    }

    [[nodiscard]] bool finish(T *&data, std::size_t &size) noexcept {
        data = nullptr;
        size = 0;
        if (size_ == 0) {
            return true;
        }
        if (size_ > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            return false;
        }

        auto *result_data = static_cast<unsigned char *>(pool_->alloc(size_ * sizeof(T), alignof(T)));
        if (!result_data) {
            return false;
        }

        std::size_t offset = 0;
        for (const Chunk *chunk = head_; chunk; chunk = chunk->next) {
            const std::size_t bytes = chunk->size * sizeof(T);
            std::memcpy(result_data + offset * sizeof(T), chunk->values(), bytes);
            offset += chunk->size;
        }

        data = reinterpret_cast<T *>(result_data);
        size = size_;
        return true;
    }

private:
    mem::BufPool *pool_ = nullptr;
    Chunk *head_ = nullptr;
    Chunk *tail_ = nullptr;
    std::size_t size_ = 0;
};

} // namespace detail

template<typename T, typename EP>
[[nodiscard]] ParseStatus parse_array(JsonParser &parser, mem::BufPool &pool, JsonArray<T> &out,
                                      EP &&element_parser) noexcept {
    static_assert(std::is_nothrow_default_constructible_v<T>,
                  "JSON array element must be nothrow default constructible");
    static_assert(std::is_trivially_copyable_v<T>, "JSON array element must be trivially copyable");

    if (detail::require_finished(parser) != ParseStatus::Done) {
        return ParseStatus::Error;
    }

    const Token *token = parser.current_token();
    if (!token || token->role != TokenRole::Value || token->kind != TokenKind::StartArr) {
        return detail::fail(parser, "expected array");
    }

    detail::PoolArrayBuilder<T> builder(pool);
    if (detail::next_token(parser, "unexpected end of JSON array") != ParseStatus::Done) {
        return ParseStatus::Error;
    }

    token = parser.current_token();
    if (token->kind == TokenKind::EndArr) {
        out = {};
        return ParseStatus::Done;
    }

    while (true) {
        T value{};
        if (detail::invoke_parser(element_parser, parser, pool, value) != ParseStatus::Done) {
            return ParseStatus::Error;
        }
        if (!builder.append(value)) {
            return detail::fail(parser, "out of memory");
        }
        if (detail::next_token(parser, "unexpected end of JSON array") != ParseStatus::Done) {
            return ParseStatus::Error;
        }

        token = parser.current_token();
        if (token->kind == TokenKind::EndArr) {
            break;
        }
    }

    T *data = nullptr;
    std::size_t size = 0;
    if (!builder.finish(data, size)) {
        return detail::fail(parser, "out of memory");
    }
    out = JsonArray<T>(data, size);
    return ParseStatus::Done;
}

template<auto EP, typename T>
[[nodiscard]] ParseStatus parse_array(JsonParser &parser, mem::BufPool &pool, JsonArray<T> &out) noexcept {
    return parse_array(parser, pool, out, EP);
}

template<typename P, typename EP>
[[nodiscard]] ParseStatus parse_object(JsonParser &parser, mem::BufPool &pool, JsonObject<P> &out,
                                       EP &&property_parser) noexcept {
    static_assert(std::is_nothrow_default_constructible_v<P>,
                  "JSON object property must be nothrow default constructible");
    static_assert(std::is_trivially_copyable_v<P>, "JSON object property must be trivially copyable");

    using Entry = typename JsonObject<P>::Entry;
    static_assert(std::is_trivially_copyable_v<Entry>, "JSON object entry must be trivially copyable");

    if (detail::require_finished(parser) != ParseStatus::Done) {
        return ParseStatus::Error;
    }

    const Token *token = parser.current_token();
    if (!token || token->role != TokenRole::Value || token->kind != TokenKind::StartObj) {
        return detail::fail(parser, "expected object");
    }

    detail::PoolArrayBuilder<Entry> builder(pool);
    if (detail::next_token(parser, "unexpected end of JSON object") != ParseStatus::Done) {
        return ParseStatus::Error;
    }

    token = parser.current_token();
    if (token->kind == TokenKind::EndObj) {
        out = {};
        return ParseStatus::Done;
    }

    while (true) {
        token = parser.current_token();
        if (token->kind != TokenKind::Text || token->role != TokenRole::ObjectKey) {
            return detail::fail(parser, "expected object key");
        }

        std::string_view key;
        if (token->view.empty()) {
            key = {};
        } else {
            auto *key_data = static_cast<char *>(pool.alloc(token->view.size(), alignof(char)));
            if (!key_data) {
                return detail::fail(parser, "out of memory");
            }
            std::memcpy(key_data, token->view.data(), token->view.size());
            key = std::string_view(key_data, token->view.size());
        }

        if (detail::next_token(parser, "object key without value") != ParseStatus::Done) {
            return ParseStatus::Error;
        }

        P value{};
        if (detail::invoke_parser(property_parser, parser, pool, value) != ParseStatus::Done) {
            return ParseStatus::Error;
        }
        if (!builder.append(Entry{.key = key, .value = value})) {
            return detail::fail(parser, "out of memory");
        }
        if (detail::next_token(parser, "unexpected end of JSON object") != ParseStatus::Done) {
            return ParseStatus::Error;
        }

        token = parser.current_token();
        if (token->kind == TokenKind::EndObj) {
            break;
        }
    }

    Entry *entries = nullptr;
    std::size_t size = 0;
    if (!builder.finish(entries, size)) {
        return detail::fail(parser, "out of memory");
    }
    out = JsonObject<P>(entries, size);
    return ParseStatus::Done;
}

template<auto EP, typename P>
[[nodiscard]] ParseStatus parse_object(JsonParser &parser, mem::BufPool &pool, JsonObject<P> &out) noexcept {
    return parse_object(parser, pool, out, EP);
}

template<typename T, typename VP>
[[nodiscard]] ParseStatus parse_optional(JsonParser &parser, mem::BufPool &pool, std::optional<T> &out,
                                         VP &&value_parser) noexcept {
    static_assert(std::is_nothrow_default_constructible_v<T>,
                  "optional JSON value must be nothrow default constructible");
    static_assert(std::is_nothrow_move_constructible_v<T>, "optional JSON value must be nothrow move constructible");

    if (detail::require_finished(parser) != ParseStatus::Done) {
        return ParseStatus::Error;
    }

    const Token *token = parser.current_token();
    if (token && token->role == TokenRole::Value && token->kind == TokenKind::Null) {
        out.reset();
        return ParseStatus::Done;
    }

    T value{};
    if (detail::invoke_parser(std::forward<VP>(value_parser), parser, pool, value) != ParseStatus::Done) {
        return ParseStatus::Error;
    }
    out.emplace(std::move(value));
    return ParseStatus::Done;
}

template<auto VP, typename T>
[[nodiscard]] ParseStatus parse_optional(JsonParser &parser, mem::BufPool &pool, std::optional<T> &out) noexcept {
    return parse_optional(parser, pool, out, VP);
}

template<typename T, typename VP>
[[nodiscard]] ParseStatus parse_nullable(JsonParser &parser, mem::BufPool &pool, Nullable<T> &out,
                                         VP &&value_parser) noexcept {
    static_assert(std::is_nothrow_default_constructible_v<T>,
                  "nullable JSON value must be nothrow default constructible");
    static_assert(std::is_nothrow_move_assignable_v<T>, "nullable JSON value must be nothrow move assignable");

    if (detail::require_finished(parser) != ParseStatus::Done) {
        return ParseStatus::Error;
    }

    const Token *token = parser.current_token();
    if (token && token->role == TokenRole::Value && token->kind == TokenKind::Null) {
        out.set_null();
        return ParseStatus::Done;
    }

    T value{};
    if (detail::invoke_parser(std::forward<VP>(value_parser), parser, pool, value) != ParseStatus::Done) {
        return ParseStatus::Error;
    }
    out.set_present(std::move(value));
    return ParseStatus::Done;
}

template<auto VP, typename T>
[[nodiscard]] ParseStatus parse_nullable(JsonParser &parser, mem::BufPool &pool, Nullable<T> &out) noexcept {
    return parse_nullable(parser, pool, out, VP);
}

template<typename T, typename VP>
[[nodiscard]] ParseStatus parse_document(JsonParser &parser, mem::BufPool &pool, T &out, VP &&value_parser) noexcept {
    static_assert(std::is_nothrow_default_constructible_v<T>,
                  "JSON document value must be nothrow default constructible");
    static_assert(std::is_nothrow_move_assignable_v<T>, "JSON document value must be nothrow move assignable");

    if (detail::require_finished(parser) != ParseStatus::Done) {
        return ParseStatus::Error;
    }
    if (!parser.current_token() && detail::next_token(parser, "expected a JSON document") != ParseStatus::Done) {
        return ParseStatus::Error;
    }

    T value{};
    if (detail::invoke_parser(std::forward<VP>(value_parser), parser, pool, value) != ParseStatus::Done) {
        return ParseStatus::Error;
    }

    switch (parser.next()) {
        case JsonParser::Status::Complete:
            out = std::move(value);
            return ParseStatus::Done;
        case JsonParser::Status::Token:
            return detail::fail(parser, "value parser did not consume the complete JSON value");
        case JsonParser::Status::NeedMore:
            return detail::fail(parser, "typed JSON parse requires finished input");
        case JsonParser::Status::Error:
            return ParseStatus::Error;
    }
    return detail::fail(parser, "invalid parser state");
}

template<auto VP, typename T>
[[nodiscard]] ParseStatus parse_document(JsonParser &parser, mem::BufPool &pool, T &out) noexcept {
    return parse_document(parser, pool, out, VP);
}

} // namespace fiber::json

#endif // FIBER_JSONPARSE_H
