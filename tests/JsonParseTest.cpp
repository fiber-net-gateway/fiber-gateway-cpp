#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "common/json/JsonParse.h"

namespace {

using fiber::json::JsonArray;
using fiber::json::JsonObject;
using fiber::json::JsonParser;
using fiber::json::Nullable;
using fiber::json::ParseStatus;
using fiber::json::TokenKind;
using fiber::json::TokenRole;
using fiber::mem::BufPool;

ParseStatus parse_integer_array(JsonParser &parser, BufPool &pool, JsonArray<std::int64_t> &out) noexcept {
    return fiber::json::parse_array<fiber::json::parse_integer>(parser, pool, out);
}

ParseStatus parse_integer_matrix(JsonParser &parser, BufPool &pool, JsonArray<JsonArray<std::int64_t>> &out) noexcept {
    return fiber::json::parse_array<parse_integer_array>(parser, pool, out);
}

ParseStatus parse_integer_object(JsonParser &parser, BufPool &pool, JsonObject<std::int64_t> &out) noexcept {
    return fiber::json::parse_object<fiber::json::parse_integer>(parser, pool, out);
}

ParseStatus parse_optional_integer(JsonParser &parser, BufPool &pool, std::optional<std::int64_t> &out) noexcept {
    return fiber::json::parse_optional<fiber::json::parse_integer>(parser, pool, out);
}

ParseStatus parse_nullable_integer(JsonParser &parser, BufPool &pool, Nullable<std::int64_t> &out) noexcept {
    return fiber::json::parse_nullable<fiber::json::parse_integer>(parser, pool, out);
}

ParseStatus parse_nullable_integer_array(JsonParser &parser, BufPool &pool,
                                         JsonArray<Nullable<std::int64_t>> &out) noexcept {
    return fiber::json::parse_array<parse_nullable_integer>(parser, pool, out);
}

struct Config {
    Nullable<std::string_view> name;
    Nullable<std::int64_t> timeout;
    std::optional<std::int64_t> limit;
    bool limit_present = false;
};

ParseStatus next_config_token(JsonParser &parser, const char *message) noexcept {
    switch (parser.next()) {
        case JsonParser::Status::Token:
            return ParseStatus::Done;
        case JsonParser::Status::Error:
            return ParseStatus::Error;
        case JsonParser::Status::NeedMore:
            (void) parser.fail("typed JSON parse requires finished input");
            return ParseStatus::Error;
        case JsonParser::Status::Complete:
            (void) parser.fail(message);
            return ParseStatus::Error;
    }
    (void) parser.fail("invalid parser state");
    return ParseStatus::Error;
}

ParseStatus parse_config(JsonParser &parser, BufPool &pool, Config &out) noexcept {
    const fiber::json::Token *token = parser.current_token();
    if (!token || token->role != TokenRole::Value || token->kind != TokenKind::StartObj) {
        (void) parser.fail("expected config object");
        return ParseStatus::Error;
    }

    Config result;
    if (next_config_token(parser, "unexpected end of config object") != ParseStatus::Done) {
        return ParseStatus::Error;
    }
    if (parser.current_token()->kind == TokenKind::EndObj) {
        out = result;
        return ParseStatus::Done;
    }

    while (true) {
        token = parser.current_token();
        if (token->kind != TokenKind::Text || token->role != TokenRole::ObjectKey) {
            (void) parser.fail("expected config field");
            return ParseStatus::Error;
        }

        enum class Field : std::uint8_t {
            Name,
            Timeout,
            Limit,
            Unknown,
        };

        Field field = Field::Unknown;
        if (token->view == "name") {
            field = Field::Name;
        } else if (token->view == "timeout") {
            field = Field::Timeout;
        } else if (token->view == "limit") {
            field = Field::Limit;
        }

        if (next_config_token(parser, "config field without value") != ParseStatus::Done) {
            return ParseStatus::Error;
        }

        ParseStatus status = ParseStatus::Error;
        switch (field) {
            case Field::Name:
                if (!result.name.is_absent()) {
                    (void) parser.fail("duplicate config field");
                    return ParseStatus::Error;
                }
                status = fiber::json::parse_nullable<fiber::json::parse_text>(parser, pool, result.name);
                break;
            case Field::Timeout:
                if (!result.timeout.is_absent()) {
                    (void) parser.fail("duplicate config field");
                    return ParseStatus::Error;
                }
                status = fiber::json::parse_nullable<fiber::json::parse_integer>(parser, pool, result.timeout);
                break;
            case Field::Limit:
                if (result.limit_present) {
                    (void) parser.fail("duplicate config field");
                    return ParseStatus::Error;
                }
                result.limit_present = true;
                status = fiber::json::parse_optional<fiber::json::parse_integer>(parser, pool, result.limit);
                break;
            case Field::Unknown: {
                std::nullptr_t skipped = nullptr;
                status = fiber::json::skip_value(parser, pool, skipped);
                break;
            }
        }
        if (status != ParseStatus::Done) {
            return ParseStatus::Error;
        }

        if (next_config_token(parser, "unexpected end of config object") != ParseStatus::Done) {
            return ParseStatus::Error;
        }
        if (parser.current_token()->kind == TokenKind::EndObj) {
            out = result;
            return ParseStatus::Done;
        }
    }
}

template<auto VP, typename T>
ParseStatus parse_complete(std::string_view json, BufPool &pool, T &out, JsonParser &parser) {
    EXPECT_TRUE(parser.feed(json.data(), json.size()));
    parser.finish();
    return fiber::json::parse_document<VP>(parser, pool, out);
}

TEST(JsonParseTest, ParsesAndOwnsBasicValues) {
    BufPool pool;

    {
        JsonParser parser;
        bool value = false;
        ASSERT_EQ(parse_complete<fiber::json::parse_bool>("true", pool, value, parser), ParseStatus::Done);
        EXPECT_TRUE(value);
    }

    {
        JsonParser parser;
        std::nullptr_t value = nullptr;
        ASSERT_EQ(parse_complete<fiber::json::parse_null>("null", pool, value, parser), ParseStatus::Done);
    }

    {
        JsonParser parser;
        std::string_view text = "old";
        const std::string json = R"("x\u0000y")";
        ASSERT_EQ(parse_complete<fiber::json::parse_text>(json, pool, text, parser), ParseStatus::Done);
        ASSERT_EQ(text.size(), 3u);
        EXPECT_EQ(text[0], 'x');
        EXPECT_EQ(text[1], '\0');
        EXPECT_EQ(text[2], 'y');
        EXPECT_EQ(parser.current_token(), nullptr);
    }

    {
        JsonParser parser;
        std::int64_t integer = 0;
        ASSERT_EQ(parse_complete<fiber::json::parse_integer>("-9223372036854775808", pool, integer, parser),
                  ParseStatus::Done);
        EXPECT_EQ(integer, INT64_MIN);
    }

    {
        JsonParser parser;
        double number = 0.0;
        ASSERT_EQ(parse_complete<fiber::json::parse_double>("9223372036854775808", pool, number, parser),
                  ParseStatus::Done);
        EXPECT_GT(number, 9.0e18);
    }
}

TEST(JsonParseTest, ParsesArrayIntoContiguousPoolStorage) {
    std::string json = "[";
    for (std::int64_t i = 0; i < 300; ++i) {
        if (i != 0) {
            json.push_back(',');
        }
        json.append(std::to_string(i));
    }
    json.push_back(']');

    BufPool pool;
    JsonParser parser;
    JsonArray<std::int64_t> array;
    ASSERT_EQ(parse_complete<parse_integer_array>(json, pool, array, parser), ParseStatus::Done);
    ASSERT_EQ(array.size(), 300u);
    for (std::size_t i = 0; i < array.size(); ++i) {
        EXPECT_EQ(array[i], static_cast<std::int64_t>(i));
    }
}

TEST(JsonParseTest, ComposesNestedArrayParsers) {
    BufPool pool;
    JsonParser parser;
    JsonArray<JsonArray<std::int64_t>> matrix;
    ASSERT_EQ(parse_complete<parse_integer_matrix>("[[1,2],[],[3]]", pool, matrix, parser), ParseStatus::Done);

    ASSERT_EQ(matrix.size(), 3u);
    ASSERT_EQ(matrix[0].size(), 2u);
    EXPECT_EQ(matrix[0][0], 1);
    EXPECT_EQ(matrix[0][1], 2);
    EXPECT_TRUE(matrix[1].empty());
    ASSERT_EQ(matrix[2].size(), 1u);
    EXPECT_EQ(matrix[2][0], 3);
}

TEST(JsonParseTest, ObjectPreservesOrderAndDuplicateKeys) {
    BufPool pool;
    JsonParser parser;
    JsonObject<std::int64_t> object;
    ASSERT_EQ(parse_complete<parse_integer_object>(R"({"a":1,"\u0061":2,"b":3})", pool, object, parser),
              ParseStatus::Done);

    ASSERT_EQ(object.size(), 3u);
    EXPECT_EQ(object[0].key, "a");
    EXPECT_EQ(object[1].key, "a");
    EXPECT_EQ(object[2].key, "b");

    ASSERT_NE(object.find_first("a"), nullptr);
    EXPECT_EQ(object.find_first("a")->value, 1);
    ASSERT_NE(object.find_last("a"), nullptr);
    EXPECT_EQ(object.find_last("a")->value, 2);
    EXPECT_EQ(object.find("missing"), nullptr);
}

TEST(JsonParseTest, SupportsOptionalAndThreeStateNullableValues) {
    BufPool pool;

    {
        JsonParser parser;
        std::optional<std::int64_t> value = 7;
        ASSERT_EQ(parse_complete<parse_optional_integer>("null", pool, value, parser), ParseStatus::Done);
        EXPECT_FALSE(value.has_value());
    }

    {
        JsonParser parser;
        Nullable<std::int64_t> value;
        ASSERT_TRUE(value.is_absent());
        ASSERT_EQ(parse_complete<parse_nullable_integer>("null", pool, value, parser), ParseStatus::Done);
        EXPECT_TRUE(value.is_null());
    }

    {
        JsonParser parser;
        Nullable<std::int64_t> value;
        ASSERT_EQ(parse_complete<parse_nullable_integer>("42", pool, value, parser), ParseStatus::Done);
        ASSERT_TRUE(value.is_present());
        EXPECT_EQ(value.value(), 42);
        ASSERT_TRUE(value.to_optional().has_value());
        EXPECT_EQ(*value.to_optional(), 42);
    }

    {
        JsonParser parser;
        JsonArray<Nullable<std::int64_t>> values;
        ASSERT_EQ(parse_complete<parse_nullable_integer_array>("[1,null,2]", pool, values, parser), ParseStatus::Done);
        ASSERT_EQ(values.size(), 3u);
        EXPECT_TRUE(values[0].is_present());
        EXPECT_TRUE(values[1].is_null());
        EXPECT_TRUE(values[2].is_present());
        EXPECT_FALSE(values[0].is_absent());
        EXPECT_FALSE(values[1].is_absent());
    }
}

TEST(JsonParseTest, CustomStructDistinguishesAbsentNullAndPresent) {
    BufPool pool;
    JsonParser parser;
    Config config;
    const std::string json = R"({"name":"gateway","timeout":null,"limit":null,"ignored":{"nested":[1,2,3]}})";
    ASSERT_EQ(parse_complete<parse_config>(json, pool, config, parser), ParseStatus::Done);

    ASSERT_TRUE(config.name.is_present());
    EXPECT_EQ(config.name.value(), "gateway");
    EXPECT_TRUE(config.timeout.is_null());
    EXPECT_TRUE(config.limit_present);
    EXPECT_FALSE(config.limit.has_value());

    JsonParser absent_parser;
    Config absent_config;
    ASSERT_EQ(parse_complete<parse_config>("{}", pool, absent_config, absent_parser), ParseStatus::Done);
    EXPECT_TRUE(absent_config.name.is_absent());
    EXPECT_TRUE(absent_config.timeout.is_absent());
    EXPECT_FALSE(absent_config.limit_present);
}

TEST(JsonParseTest, ReportsSemanticErrorsAndLeavesOutputUnchanged) {
    BufPool pool;

    {
        JsonParser parser;
        std::int64_t value = 9;
        EXPECT_EQ(parse_complete<fiber::json::parse_integer>("1.5", pool, value, parser), ParseStatus::Error);
        EXPECT_EQ(value, 9);
        ASSERT_NE(parser.error().message, nullptr);
        EXPECT_STREQ(parser.error().message, "expected integer");
        EXPECT_EQ(parser.error().offset, 0u);
    }

    {
        JsonParser parser;
        std::string_view value = "unchanged";
        ASSERT_TRUE(parser.feed(R"("changed")", 9));
        EXPECT_EQ(fiber::json::parse_document<fiber::json::parse_text>(parser, pool, value), ParseStatus::Error);
        EXPECT_EQ(value, "unchanged");
        ASSERT_NE(parser.error().message, nullptr);
        EXPECT_STREQ(parser.error().message, "typed JSON parse requires finished input");
    }

    {
        JsonParser parser;
        double value = 1.0;
        EXPECT_EQ(parse_complete<fiber::json::parse_double>("1e9999", pool, value, parser), ParseStatus::Error);
        EXPECT_DOUBLE_EQ(value, 1.0);
        EXPECT_STREQ(parser.error().message, "floating point out of range");
    }

    {
        JsonParser parser;
        std::int64_t value = 9;
        EXPECT_EQ(parse_complete<fiber::json::parse_integer>("1 true", pool, value, parser), ParseStatus::Error);
        EXPECT_EQ(value, 9);
        EXPECT_STREQ(parser.error().message, "trailing garbage after JSON value");
    }
}

} // namespace
