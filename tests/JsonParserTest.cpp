#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "common/json/JsonParser.h"

using fiber::json::JsonParser;
using fiber::json::Token;
using fiber::json::TokenKind;
using fiber::json::TokenRole;

namespace {

struct TokenSnapshot {
    TokenKind kind = TokenKind::Null;
    TokenRole role = TokenRole::Value;
    std::size_t offset = 0;
    bool bval = false;
    std::int64_t inum = 0;
    double fnum = 0.0;
    std::string text;

    bool operator==(const TokenSnapshot &) const = default;
};

TokenSnapshot snapshot(const JsonParser &parser) {
    const Token *token = parser.current_token();
    EXPECT_NE(token, nullptr);
    if (!token) {
        return {};
    }

    TokenSnapshot result{
            .kind = token->kind,
            .role = token->role,
            .offset = parser.current_offset(),
    };
    switch (token->kind) {
        case TokenKind::Bool:
            result.bval = token->bval;
            break;
        case TokenKind::Integer:
            result.inum = token->inum;
            break;
        case TokenKind::Double:
            result.fnum = token->fnum;
            break;
        case TokenKind::BigNumber:
        case TokenKind::Text:
            result.text.assign(token->view);
            break;
        default:
            break;
    }
    return result;
}

JsonParser::Status drain(JsonParser &parser, std::vector<TokenSnapshot> &tokens) {
    while (true) {
        JsonParser::Status status = parser.next();
        if (status != JsonParser::Status::Token) {
            return status;
        }
        tokens.push_back(snapshot(parser));
    }
}

std::vector<TokenSnapshot> parse_complete(const std::string &json) {
    JsonParser parser;
    EXPECT_TRUE(parser.feed(json.data(), json.size()));
    parser.finish();

    std::vector<TokenSnapshot> tokens;
    JsonParser::Status status = drain(parser, tokens);
    EXPECT_EQ(status, JsonParser::Status::Complete) << (parser.error().message ? parser.error().message : "");
    return tokens;
}

} // namespace

TEST(JsonParserTest, ProducesSemanticTokensAndOffsets) {
    const std::string json = R"({"n":-1,"b":true,"z":null,"f":1.25,"s":"x","a":[]})";
    std::vector<TokenSnapshot> tokens = parse_complete(json);

    ASSERT_EQ(tokens.size(), 15u);
    EXPECT_EQ(tokens[0].kind, TokenKind::StartObj);
    EXPECT_EQ(tokens[0].offset, 0u);

    EXPECT_EQ(tokens[1].kind, TokenKind::Text);
    EXPECT_EQ(tokens[1].role, TokenRole::ObjectKey);
    EXPECT_EQ(tokens[1].text, "n");
    EXPECT_EQ(tokens[1].offset, json.find("\"n\""));

    EXPECT_EQ(tokens[2].kind, TokenKind::Integer);
    EXPECT_EQ(tokens[2].inum, -1);
    EXPECT_EQ(tokens[2].offset, json.find("-1"));

    EXPECT_EQ(tokens[3].kind, TokenKind::Text);
    EXPECT_EQ(tokens[3].role, TokenRole::ObjectKey);
    EXPECT_EQ(tokens[3].text, "b");
    EXPECT_EQ(tokens[4].kind, TokenKind::Bool);
    EXPECT_TRUE(tokens[4].bval);

    EXPECT_EQ(tokens[5].kind, TokenKind::Text);
    EXPECT_EQ(tokens[5].role, TokenRole::ObjectKey);
    EXPECT_EQ(tokens[5].text, "z");
    EXPECT_EQ(tokens[6].kind, TokenKind::Null);

    EXPECT_EQ(tokens[7].kind, TokenKind::Text);
    EXPECT_EQ(tokens[7].role, TokenRole::ObjectKey);
    EXPECT_EQ(tokens[7].text, "f");
    EXPECT_EQ(tokens[8].kind, TokenKind::Double);
    EXPECT_DOUBLE_EQ(tokens[8].fnum, 1.25);

    EXPECT_EQ(tokens[9].kind, TokenKind::Text);
    EXPECT_EQ(tokens[9].role, TokenRole::ObjectKey);
    EXPECT_EQ(tokens[9].text, "s");
    EXPECT_EQ(tokens[10].kind, TokenKind::Text);
    EXPECT_EQ(tokens[10].role, TokenRole::Value);
    EXPECT_EQ(tokens[10].text, "x");

    EXPECT_EQ(tokens[11].kind, TokenKind::Text);
    EXPECT_EQ(tokens[11].role, TokenRole::ObjectKey);
    EXPECT_EQ(tokens[11].text, "a");
    EXPECT_EQ(tokens[12].kind, TokenKind::StartArr);
    EXPECT_EQ(tokens[13].kind, TokenKind::EndArr);
    EXPECT_EQ(tokens[14].kind, TokenKind::EndObj);
    EXPECT_EQ(tokens[14].offset, json.size() - 1);
}

TEST(JsonParserTest, RequiresFinishBeforeComplete) {
    JsonParser parser;
    ASSERT_TRUE(parser.feed("null", 4));

    ASSERT_EQ(parser.next(), JsonParser::Status::Token);
    ASSERT_NE(parser.current_token(), nullptr);
    EXPECT_EQ(parser.current_token()->kind, TokenKind::Null);

    EXPECT_EQ(parser.next(), JsonParser::Status::NeedMore);
    EXPECT_EQ(parser.current_token(), nullptr);

    parser.finish();
    EXPECT_EQ(parser.next(), JsonParser::Status::Complete);
    EXPECT_EQ(parser.current_token(), nullptr);
}

TEST(JsonParserTest, FinishesNumberHeldAtChunkBoundary) {
    JsonParser parser;
    ASSERT_TRUE(parser.feed("123", 3));
    EXPECT_EQ(parser.next(), JsonParser::Status::NeedMore);

    parser.finish();
    ASSERT_EQ(parser.next(), JsonParser::Status::Token);
    ASSERT_NE(parser.current_token(), nullptr);
    EXPECT_EQ(parser.current_token()->kind, TokenKind::Integer);
    EXPECT_EQ(parser.current_token()->inum, 123);
    EXPECT_EQ(parser.current_offset(), 0u);
    EXPECT_EQ(parser.next(), JsonParser::Status::Complete);
}

TEST(JsonParserTest, ReturnsOutOfRangeNumbersAsBigNumberViews) {
    const std::string json = "[9223372036854775807,9223372036854775808,-9223372036854775809,1e9999]";
    std::vector<TokenSnapshot> tokens = parse_complete(json);

    ASSERT_EQ(tokens.size(), 6u);
    EXPECT_EQ(tokens[0].kind, TokenKind::StartArr);
    EXPECT_EQ(tokens[1].kind, TokenKind::Integer);
    EXPECT_EQ(tokens[1].inum, INT64_MAX);
    EXPECT_EQ(tokens[2].kind, TokenKind::BigNumber);
    EXPECT_EQ(tokens[2].text, "9223372036854775808");
    EXPECT_EQ(tokens[3].kind, TokenKind::BigNumber);
    EXPECT_EQ(tokens[3].text, "-9223372036854775809");
    EXPECT_EQ(tokens[4].kind, TokenKind::BigNumber);
    EXPECT_EQ(tokens[4].text, "1e9999");
    EXPECT_EQ(tokens[5].kind, TokenKind::EndArr);
}

TEST(JsonParserTest, DecodesEscapedTextIncludingEmbeddedNull) {
    const std::string json = R"({"key":"x\u0000y"})";
    std::vector<TokenSnapshot> tokens = parse_complete(json);

    ASSERT_EQ(tokens.size(), 4u);
    EXPECT_EQ(tokens[1].kind, TokenKind::Text);
    EXPECT_EQ(tokens[1].role, TokenRole::ObjectKey);
    EXPECT_EQ(tokens[1].text, "key");
    EXPECT_EQ(tokens[2].kind, TokenKind::Text);
    EXPECT_EQ(tokens[2].role, TokenRole::Value);
    ASSERT_EQ(tokens[2].text.size(), 3u);
    EXPECT_EQ(tokens[2].text[0], 'x');
    EXPECT_EQ(tokens[2].text[1], '\0');
    EXPECT_EQ(tokens[2].text[2], 'y');
}

TEST(JsonParserTest, HandlesEveryTwoChunkSplit) {
    const std::string json = "{\"text\":\"line\\n\\u00e9 \xF0\x9F\x98\x80\",\"number\":9223372036854775808,"
                             "\"array\":[true,null,-1.5e2]}";
    const std::vector<TokenSnapshot> expected = parse_complete(json);

    for (std::size_t split = 0; split <= json.size(); ++split) {
        SCOPED_TRACE(split);
        JsonParser parser;
        ASSERT_TRUE(parser.feed(json.data(), split));

        std::vector<TokenSnapshot> actual;
        EXPECT_EQ(drain(parser, actual), JsonParser::Status::NeedMore);

        ASSERT_TRUE(parser.feed(json.data() + split, json.size() - split));
        parser.finish();
        EXPECT_EQ(drain(parser, actual), JsonParser::Status::Complete)
                << (parser.error().message ? parser.error().message : "");
        EXPECT_EQ(actual, expected);
    }
}

TEST(JsonParserTest, ReportsSyntaxAndDocumentBoundaryErrors) {
    {
        JsonParser parser;
        ASSERT_TRUE(parser.feed("{\"a\":}", 6));
        parser.finish();
        std::vector<TokenSnapshot> tokens;
        EXPECT_EQ(drain(parser, tokens), JsonParser::Status::Error);
        EXPECT_STREQ(parser.error().message, "unallowed token at this point in JSON text");
        EXPECT_EQ(parser.error().offset, 5u);
    }
    {
        JsonParser parser;
        ASSERT_TRUE(parser.feed("null true", 9));
        parser.finish();
        std::vector<TokenSnapshot> tokens;
        EXPECT_EQ(drain(parser, tokens), JsonParser::Status::Error);
        EXPECT_STREQ(parser.error().message, "trailing garbage after JSON value");
        EXPECT_EQ(parser.error().offset, 5u);
    }
    {
        JsonParser parser;
        ASSERT_TRUE(parser.feed("[", 1));
        parser.finish();
        std::vector<TokenSnapshot> tokens;
        EXPECT_EQ(drain(parser, tokens), JsonParser::Status::Error);
        EXPECT_STREQ(parser.error().message, "premature EOF");
        EXPECT_EQ(parser.error().offset, 1u);
    }
}

TEST(JsonParserTest, RejectsFeedBeforePreviousChunkIsDrained) {
    JsonParser parser;
    ASSERT_TRUE(parser.feed("null", 4));
    ASSERT_EQ(parser.next(), JsonParser::Status::Token);
    EXPECT_FALSE(parser.feed("true", 4));
    EXPECT_STREQ(parser.error().message, "previous input chunk is not fully consumed");
    EXPECT_EQ(parser.next(), JsonParser::Status::Error);
}

TEST(JsonParserTest, ResetReusesParserAfterError) {
    JsonParser parser;
    ASSERT_TRUE(parser.feed("[", 1));
    parser.finish();

    std::vector<TokenSnapshot> tokens;
    EXPECT_EQ(drain(parser, tokens), JsonParser::Status::Error);

    parser.reset();
    ASSERT_TRUE(parser.feed("true", 4));
    parser.finish();
    tokens.clear();
    EXPECT_EQ(drain(parser, tokens), JsonParser::Status::Complete);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Bool);
    EXPECT_TRUE(tokens[0].bval);
    EXPECT_EQ(parser.error().message, nullptr);
}

TEST(JsonParserTest, RejectsExcessiveNesting) {
    std::string json(140, '[');
    json.append(140, ']');

    JsonParser parser;
    ASSERT_TRUE(parser.feed(json.data(), json.size()));
    parser.finish();

    std::vector<TokenSnapshot> tokens;
    EXPECT_EQ(drain(parser, tokens), JsonParser::Status::Error);
    EXPECT_STREQ(parser.error().message, "maximum JSON nesting depth exceeded");
}
