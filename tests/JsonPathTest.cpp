#include <fiber/common/json/JsonPath.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using fiber::json::JsonPathCaptureKind;
using fiber::json::JsonPathCompileErrorCode;
using fiber::json::JsonPathMatch;
using fiber::json::JsonPathProgram;
using fiber::json::JsonPathReplacement;
using fiber::json::JsonPathRewriteErrorCode;
using fiber::json::JsonPathRewriter;
using fiber::json::JsonPathRule;
using fiber::json::JsonPathVisitErrorCode;
using fiber::json::JsonPathVisitor;
using fiber::json::TokenKind;
using fiber::mem::BufPool;

struct CapturedMatch {
    std::uint32_t action = 0;
    TokenKind kind = TokenKind::Null;
    std::string value;
    std::string raw;
    std::string key_capture;
    std::string index_capture;
};

struct CaptureContext {
    std::string_view input;
    std::vector<CapturedMatch> matches;
    bool accept = true;

    static bool on_match(void *opaque, const JsonPathMatch &match) noexcept {
        auto &self = *static_cast<CaptureContext *>(opaque);
        CapturedMatch captured{
                .action = match.action,
                .kind = match.token.kind,
                .raw = std::string(self.input.substr(match.span.begin, match.span.size())),
        };
        if (match.token.kind == TokenKind::Text) {
            captured.value.assign(match.token.view);
        } else if (match.token.kind == TokenKind::Integer) {
            captured.value = std::to_string(match.token.inum);
        } else if (match.token.kind == TokenKind::Bool) {
            captured.value = match.token.bval ? "true" : "false";
        }
        if (const auto *key = match.variables.find("key")) {
            captured.key_capture.assign(key->text);
        }
        if (const auto *index = match.variables.find("idx")) {
            captured.index_capture.assign(index->text);
            EXPECT_EQ(index->kind, JsonPathCaptureKind::ArrayIndex);
        }
        self.matches.push_back(std::move(captured));
        return self.accept;
    }
};

fiber::mem::IoBuf make_body(std::string_view text) {
    fiber::mem::IoBuf body = fiber::mem::IoBuf::allocate(text.size());
    if (!text.empty()) {
        std::memcpy(body.writable_data(), text.data(), text.size());
        body.commit(text.size());
    }
    return body;
}

std::string flatten(const fiber::mem::IoBufChain &chain) {
    std::vector<iovec> parts(chain.size());
    const int count = chain.fill_write_iov(parts.data(), static_cast<int>(parts.size()));
    std::string result;
    result.reserve(chain.readable_bytes());
    for (int i = 0; i < count; ++i) {
        result.append(static_cast<const char *>(parts[i].iov_base), parts[i].iov_len);
    }
    return result;
}

struct RewriteValues {
    bool invalid = false;

    static bool on_match(void *opaque, const JsonPathMatch &match, JsonPathReplacement &replacement) noexcept {
        auto &self = *static_cast<RewriteValues *>(opaque);
        replacement.replace = true;
        if (self.invalid) {
            replacement.encoded_value = "not-json";
        } else if (match.action == 1) {
            replacement.encoded_value = R"("upstream")";
        } else {
            replacement.encoded_value = "false";
        }
        return true;
    }
};

TEST(JsonPathTest, ExtractsMultiplePathsInOneTraversal) {
    const JsonPathRule rules[] = {
            {.expression = "$.model", .action = 1},
            {.expression = "$.stream", .action = 2},
            {.expression = "$.messages[*idx].role", .action = 3},
    };
    auto compiled = JsonPathProgram::compile(rules);
    ASSERT_TRUE(compiled) << compiled.error().message;

    const std::string input = R"({"model":"gpt-4o","stream":true,"messages":[{"role":"user"},{"role":"assistant"}]})";
    BufPool pool;
    CaptureContext context{.input = input};
    auto result = fiber::json::visit_json_paths(
            *compiled, input, pool, JsonPathVisitor{.context = &context, .on_match = &CaptureContext::on_match});
    ASSERT_TRUE(result);
    ASSERT_EQ(context.matches.size(), 4u);
    EXPECT_EQ(context.matches[0].action, 1u);
    EXPECT_EQ(context.matches[0].value, "gpt-4o");
    EXPECT_EQ(context.matches[0].raw, R"("gpt-4o")");
    EXPECT_EQ(context.matches[1].action, 2u);
    EXPECT_EQ(context.matches[1].value, "true");
    EXPECT_EQ(context.matches[2].value, "user");
    EXPECT_EQ(context.matches[2].index_capture, "0");
    EXPECT_EQ(context.matches[3].value, "assistant");
    EXPECT_EQ(context.matches[3].index_capture, "1");
}

TEST(JsonPathTest, CapturesEscapedWildcardKeyBeforeParserAdvances) {
    const JsonPathRule rules[] = {
            {.expression = "$.items.*key.value", .action = 7},
    };
    auto compiled = JsonPathProgram::compile(rules);
    ASSERT_TRUE(compiled) << compiled.error().message;

    const std::string input = R"({"items":{"a\u0062":{"value":1},"plain":{"value":2}}})";
    BufPool pool;
    CaptureContext context{.input = input};
    auto result = fiber::json::visit_json_paths(
            *compiled, input, pool, JsonPathVisitor{.context = &context, .on_match = &CaptureContext::on_match});
    ASSERT_TRUE(result);
    ASSERT_EQ(context.matches.size(), 2u);
    EXPECT_EQ(context.matches[0].key_capture, "ab");
    EXPECT_EQ(context.matches[1].key_capture, "plain");
}

TEST(JsonPathTest, ReportsContainerRawSpanAndRootMatch) {
    const JsonPathRule rules[] = {
            {.expression = "$.payload", .action = 9},
    };
    auto compiled = JsonPathProgram::compile(rules);
    ASSERT_TRUE(compiled) << compiled.error().message;

    const std::string input = " { \"payload\" : [ 1, {\"x\":true} ], \"tail\":0 } ";
    BufPool pool;
    CaptureContext context{.input = input};
    auto result = fiber::json::visit_json_paths(
            *compiled, input, pool, JsonPathVisitor{.context = &context, .on_match = &CaptureContext::on_match});
    ASSERT_TRUE(result);
    ASSERT_EQ(context.matches.size(), 1u);
    EXPECT_EQ(context.matches[0].kind, TokenKind::StartArr);
    EXPECT_EQ(context.matches[0].raw, R"([ 1, {"x":true} ])");

    const JsonPathRule root_rule[] = {
            {.expression = "$", .action = 10},
    };
    auto root = JsonPathProgram::compile(root_rule);
    ASSERT_TRUE(root) << root.error().message;
    CaptureContext root_context{.input = input};
    result = fiber::json::visit_json_paths(
            *root, input, pool, JsonPathVisitor{.context = &root_context, .on_match = &CaptureContext::on_match});
    ASSERT_TRUE(result);
    ASSERT_EQ(root_context.matches.size(), 1u);
    EXPECT_EQ(root_context.matches[0].raw, R"({ "payload" : [ 1, {"x":true} ], "tail":0 })");
}

TEST(JsonPathTest, MissingAndIntermediateTypeMismatchAreNonMatches) {
    const JsonPathRule rules[] = {
            {.expression = "$.missing", .action = 1},
            {.expression = "$.nested.value", .action = 2},
    };
    auto compiled = JsonPathProgram::compile(rules);
    ASSERT_TRUE(compiled) << compiled.error().message;

    const std::string input = R"({"nested":42,"other":"kept"})";
    BufPool pool;
    CaptureContext context{.input = input};
    auto result = fiber::json::visit_json_paths(
            *compiled, input, pool, JsonPathVisitor{.context = &context, .on_match = &CaptureContext::on_match});
    ASSERT_TRUE(result);
    EXPECT_TRUE(context.matches.empty());
}

TEST(JsonPathTest, DuplicateObjectFieldsEachProduceAMatch) {
    const JsonPathRule rules[] = {
            {.expression = "$.model", .action = 1},
    };
    auto compiled = JsonPathProgram::compile(rules);
    ASSERT_TRUE(compiled) << compiled.error().message;

    const std::string input = R"({"model":"first","model":"last"})";
    BufPool pool;
    CaptureContext context{.input = input};
    auto result = fiber::json::visit_json_paths(
            *compiled, input, pool, JsonPathVisitor{.context = &context, .on_match = &CaptureContext::on_match});
    ASSERT_TRUE(result);
    ASSERT_EQ(context.matches.size(), 2u);
    EXPECT_EQ(context.matches[0].value, "first");
    EXPECT_EQ(context.matches[1].value, "last");
}

TEST(JsonPathTest, RewritesArbitraryMatchedValuesAndPreservesOtherBytes) {
    const JsonPathRule rules[] = {
            {.expression = "$.model", .action = 1},
            {.expression = "$.items[*].enabled", .action = 2},
    };
    auto compiled = JsonPathProgram::compile(rules);
    ASSERT_TRUE(compiled) << compiled.error().message;

    constexpr std::string_view input =
            R"({ "unknown":[1e+09,"\u0041"], "model":"public", "items":[{"enabled":true},{"enabled":null}] })";
    BufPool pool;
    fiber::mem::IoBufNodePool nodes;
    RewriteValues values;
    auto rewritten = fiber::json::rewrite_json_paths(*compiled, make_body(input), pool, nodes,
                                                     JsonPathRewriter{
                                                             .context = &values,
                                                             .on_match = &RewriteValues::on_match,
                                                     });

    ASSERT_TRUE(rewritten);
    EXPECT_EQ(flatten(*rewritten),
              R"({ "unknown":[1e+09,"\u0041"], "model":"upstream", "items":[{"enabled":false},{"enabled":false}] })");
}

TEST(JsonPathTest, RejectsInvalidEncodedReplacement) {
    const JsonPathRule rules[] = {
            {.expression = "$.model", .action = 1},
    };
    auto compiled = JsonPathProgram::compile(rules);
    ASSERT_TRUE(compiled) << compiled.error().message;

    BufPool pool;
    fiber::mem::IoBufNodePool nodes;
    RewriteValues values{.invalid = true};
    auto rewritten = fiber::json::rewrite_json_paths(*compiled, make_body(R"({"model":"public"})"), pool, nodes,
                                                     JsonPathRewriter{
                                                             .context = &values,
                                                             .on_match = &RewriteValues::on_match,
                                                     });

    ASSERT_FALSE(rewritten);
    EXPECT_EQ(rewritten.error().code, JsonPathRewriteErrorCode::InvalidReplacement);
    EXPECT_EQ(rewritten.error().action, 1u);
}

TEST(JsonPathTest, RejectsConflictingAndMalformedPrograms) {
    {
        const JsonPathRule rules[] = {
                {.expression = "$.a.b", .action = 1},
                {.expression = "$.a.*", .action = 2},
        };
        auto result = JsonPathProgram::compile(rules);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, JsonPathCompileErrorCode::WildcardConflict);
    }
    {
        const JsonPathRule rules[] = {
                {.expression = "$.a", .action = 1},
                {.expression = "$.a.b", .action = 2},
        };
        auto result = JsonPathProgram::compile(rules);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, JsonPathCompileErrorCode::PrefixConflict);
    }
    {
        const JsonPathRule rules[] = {
                {.expression = "$.a[*left].x", .action = 1},
                {.expression = "$.a[*right].y", .action = 2},
        };
        auto result = JsonPathProgram::compile(rules);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, JsonPathCompileErrorCode::CaptureConflict);
    }
    {
        const JsonPathRule rule[] = {
                {.expression = "$.a[nope]", .action = 1},
        };
        auto result = JsonPathProgram::compile(rule);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, JsonPathCompileErrorCode::InvalidExpression);
    }
    {
        const JsonPathRule rule[] = {
                {.expression = "$.a[-1]", .action = 1},
        };
        auto result = JsonPathProgram::compile(rule);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, JsonPathCompileErrorCode::InvalidExpression);
    }
}

TEST(JsonPathTest, ReportsInvalidJsonAndHandlerRejection) {
    const JsonPathRule rules[] = {
            {.expression = "$.value", .action = 17},
    };
    auto compiled = JsonPathProgram::compile(rules);
    ASSERT_TRUE(compiled) << compiled.error().message;

    BufPool pool;
    CaptureContext invalid_context{.input = R"({"value":1)"};
    auto invalid = fiber::json::visit_json_paths(
            *compiled, invalid_context.input, pool,
            JsonPathVisitor{.context = &invalid_context, .on_match = &CaptureContext::on_match});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, JsonPathVisitErrorCode::InvalidJson);

    CaptureContext rejected_context{
            .input = R"({"value":1})",
            .accept = false,
    };
    auto rejected = fiber::json::visit_json_paths(
            *compiled, rejected_context.input, pool,
            JsonPathVisitor{.context = &rejected_context, .on_match = &CaptureContext::on_match});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, JsonPathVisitErrorCode::HandlerRejected);
    EXPECT_EQ(rejected.error().action, 17u);
}

} // namespace
