#ifndef FIBER_JSONPATH_H
#define FIBER_JSONPATH_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../mem/BufPool.h"
#include "../mem/IoBuf.h"
#include "../mem/IoBufChain.h"
#include "JsonParser.h"

namespace fiber::json {

class JsonPathWalker;

struct JsonPathRule {
    std::string_view expression;
    std::uint32_t action = 0;
};

enum class JsonPathCompileErrorCode : std::uint8_t {
    InvalidExpression,
    DuplicatePath,
    PrefixConflict,
    WildcardConflict,
    CaptureConflict,
    PathTooDeep,
};

struct JsonPathCompileError {
    JsonPathCompileErrorCode code = JsonPathCompileErrorCode::InvalidExpression;
    std::size_t rule_index = 0;
    std::size_t expression_offset = 0;
    std::string message;
};

class JsonPathProgram {
public:
    JsonPathProgram() = default;
    ~JsonPathProgram() = default;

    JsonPathProgram(const JsonPathProgram &) = delete;
    JsonPathProgram &operator=(const JsonPathProgram &) = delete;
    JsonPathProgram(JsonPathProgram &&) noexcept = default;
    JsonPathProgram &operator=(JsonPathProgram &&) noexcept = default;

    [[nodiscard]] static std::expected<JsonPathProgram, JsonPathCompileError>
    compile(std::span<const JsonPathRule> rules);

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t rule_count() const noexcept;
    [[nodiscard]] std::size_t max_capture_depth() const noexcept;

private:
    static constexpr std::uint32_t NoNode = UINT32_MAX;

    struct ExactEdge {
        std::string key;
        std::uint32_t child = NoNode;
    };

    struct IndexEdge {
        std::int32_t index = 0;
        std::uint32_t child = NoNode;
    };

    struct Node {
        std::vector<ExactEdge> exact;
        std::vector<IndexEdge> indices;
        std::uint32_t wildcard_key = NoNode;
        std::uint32_t wildcard_index = NoNode;
        std::string capture_name;
        std::optional<std::uint32_t> action;

        [[nodiscard]] bool has_children() const noexcept;
    };

    [[nodiscard]] std::uint32_t add_node();
    [[nodiscard]] std::uint32_t find_exact(std::uint32_t node, std::string_view key) const noexcept;
    [[nodiscard]] std::uint32_t find_index(std::uint32_t node, std::size_t index) const noexcept;
    [[nodiscard]] bool subtree_active(std::uint32_t node) const noexcept;

    std::vector<Node> nodes_;
    std::size_t rule_count_ = 0;
    std::size_t max_capture_depth_ = 0;

    friend class JsonPathWalker;
};

struct JsonValueSpan {
    std::size_t begin = 0;
    std::size_t end = 0;

    [[nodiscard]] std::size_t size() const noexcept { return end - begin; }
};

enum class JsonPathCaptureKind : std::uint8_t {
    ObjectKey,
    ArrayIndex,
};

struct JsonPathCapture {
    std::string_view name;
    std::string_view text;
    std::size_t index = 0;
    JsonPathCaptureKind kind = JsonPathCaptureKind::ObjectKey;
};

class JsonPathVarScope {
public:
    JsonPathVarScope() noexcept = default;
    explicit JsonPathVarScope(std::span<const JsonPathCapture> captures) noexcept : captures_(captures) {}

    [[nodiscard]] const JsonPathCapture *find(std::string_view name) const noexcept;
    [[nodiscard]] std::string_view get(std::string_view name) const noexcept;
    [[nodiscard]] std::span<const JsonPathCapture> captures() const noexcept { return captures_; }

private:
    std::span<const JsonPathCapture> captures_;
};

struct JsonPathMatch {
    std::uint32_t action = 0;
    Token token;
    JsonValueSpan span;
    JsonPathVarScope variables;
};

using JsonPathMatchFn = bool (*)(void *context, const JsonPathMatch &match) noexcept;

struct JsonPathVisitor {
    void *context = nullptr;
    JsonPathMatchFn on_match = nullptr;
};

enum class JsonPathVisitErrorCode : std::uint8_t {
    InvalidVisitor,
    InvalidJson,
    OutOfMemory,
    HandlerRejected,
};

struct JsonPathVisitError {
    JsonPathVisitErrorCode code = JsonPathVisitErrorCode::InvalidJson;
    ParseError parse_error;
    std::uint32_t action = 0;
};

[[nodiscard]] std::expected<void, JsonPathVisitError> visit_json_paths(const JsonPathProgram &program,
                                                                       std::string_view input, mem::BufPool &pool,
                                                                       JsonPathVisitor visitor) noexcept;

struct JsonPathReplacement {
    bool replace = false;
    std::string_view encoded_value;
};

using JsonPathRewriteFn = bool (*)(void *context, const JsonPathMatch &match,
                                   JsonPathReplacement &replacement) noexcept;

struct JsonPathRewriter {
    void *context = nullptr;
    JsonPathRewriteFn on_match = nullptr;
};

enum class JsonPathRewriteErrorCode : std::uint8_t {
    InvalidRewriter,
    InvalidJson,
    OutOfMemory,
    HandlerRejected,
    InvalidReplacement,
    InvalidSpan,
};

struct JsonPathRewriteError {
    JsonPathRewriteErrorCode code = JsonPathRewriteErrorCode::InvalidJson;
    JsonPathVisitError visit_error;
    std::uint32_t action = 0;
};

[[nodiscard]] std::expected<mem::IoBufChain, JsonPathRewriteError>
rewrite_json_paths(const JsonPathProgram &program, mem::IoBuf input, mem::BufPool &pool, mem::IoBufNodePool &node_pool,
                   JsonPathRewriter rewriter) noexcept;

} // namespace fiber::json

#endif // FIBER_JSONPATH_H
