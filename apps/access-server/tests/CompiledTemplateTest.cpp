#include <gtest/gtest.h>

#include <string>

#include "routing/CompiledTemplate.h"

namespace {

using fiber::access_server::parse_template;
using fiber::access_server::TemplateParseError;

TEST(CompiledTemplateTest, KeepsPlainAndEmptyTemplatesStatic) {
    auto plain = parse_template("plain text");
    ASSERT_TRUE(plain);
    EXPECT_FALSE(plain->dynamic());
    EXPECT_EQ(plain->trailing_literal, "plain text");
    EXPECT_EQ(plain->literal_size, 10U);

    auto empty = parse_template("");
    ASSERT_TRUE(empty);
    EXPECT_FALSE(empty->dynamic());
    EXPECT_TRUE(empty->trailing_literal.empty());
    EXPECT_EQ(empty->literal_size, 0U);
}

TEST(CompiledTemplateTest, SplitsExpressionsAndKeepsTheirLeadingLiterals) {
    auto compiled = parse_template("a=${first};b=${second};tail");

    ASSERT_TRUE(compiled);
    ASSERT_EQ(compiled->expressions.size(), 2U);
    EXPECT_EQ(compiled->expressions[0].leading_literal, "a=");
    EXPECT_EQ(compiled->expressions[0].source, "first");
    EXPECT_FALSE(compiled->expressions[0].program.valid());
    EXPECT_EQ(compiled->expressions[1].leading_literal, ";b=");
    EXPECT_EQ(compiled->expressions[1].source, "second");
    EXPECT_FALSE(compiled->expressions[1].program.valid());
    EXPECT_EQ(compiled->trailing_literal, ";tail");
    EXPECT_EQ(compiled->literal_size, 10U);
}

TEST(CompiledTemplateTest, DecodesOnlyJavaTemplateEscapes) {
    auto compiled = parse_template(R"(literal=\$\{\}\\;tick=`)");

    ASSERT_TRUE(compiled);
    EXPECT_FALSE(compiled->dynamic());
    EXPECT_EQ(compiled->trailing_literal, "literal=${}\\;tick=`");

    auto invalid = parse_template(R"(line=\n)");
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error(), TemplateParseError::InvalidEscape);

    auto trailing = parse_template(std::string("tail\\"));
    ASSERT_FALSE(trailing);
    EXPECT_EQ(trailing.error(), TemplateParseError::InvalidEscape);
}

TEST(CompiledTemplateTest, LeavesBackslashesInsideExpressionsForTheScriptParser) {
    auto compiled = parse_template(R"(${left\}tail)");

    ASSERT_TRUE(compiled);
    ASSERT_EQ(compiled->expressions.size(), 1U);
    EXPECT_EQ(compiled->expressions[0].source, "left\\");
    EXPECT_EQ(compiled->trailing_literal, "tail");
}

TEST(CompiledTemplateTest, RejectsEmptyAndUnclosedExpressions) {
    auto empty = parse_template("${}");
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error(), TemplateParseError::EmptyExpression);

    auto unclosed = parse_template("before-${value");
    ASSERT_FALSE(unclosed);
    EXPECT_EQ(unclosed.error(), TemplateParseError::UnclosedExpression);
}

} // namespace
