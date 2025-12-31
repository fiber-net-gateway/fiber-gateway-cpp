#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "common/json/JsonDecode.h"
#include "common/json/JsGc.h"

using fiber::json::GcArray;
using fiber::json::GcObject;
using fiber::json::GcString;
using fiber::json::GcHeap;
using fiber::json::JsNodeType;
using fiber::json::JsValue;
using fiber::json::Parser;
using fiber::json::StreamParser;

namespace {

std::string to_string(const GcString *str) {
    if (!str) {
        return {};
    }
    std::string out;
    if (!fiber::json::gc_string_to_utf8(str, out)) {
        return {};
    }
    return out;
}

const GcObject *as_object(const JsValue &value) {
    return reinterpret_cast<const GcObject *>(value.gc);
}

const GcArray *as_array(const JsValue &value) {
    return reinterpret_cast<const GcArray *>(value.gc);
}

const GcString *as_string(const JsValue &value) {
    return reinterpret_cast<const GcString *>(value.gc);
}

} // namespace

TEST(ParserTest, ParseObjectAndArray) {
    GcHeap heap;
    Parser parser(heap);
    JsValue root;
    const char *json = "{\"name\":\"fiber\",\"nums\":[1,2.5,true,null]}";
    ASSERT_TRUE(parser.parse(json, std::strlen(json), root)) << parser.error().message;
    ASSERT_EQ(root.type_, JsNodeType::Object);

    const GcObject *obj = as_object(root);
    ASSERT_EQ(obj->size, 2u);
    EXPECT_EQ(to_string(obj->entries[0].key), "name");
    ASSERT_EQ(obj->entries[0].value.type_, JsNodeType::HeapString);
    EXPECT_EQ(to_string(as_string(obj->entries[0].value)), "fiber");

    EXPECT_EQ(to_string(obj->entries[1].key), "nums");
    ASSERT_EQ(obj->entries[1].value.type_, JsNodeType::Array);
    const GcArray *arr = as_array(obj->entries[1].value);
    ASSERT_EQ(arr->size, 4u);
    EXPECT_EQ(arr->elems[0].type_, JsNodeType::Integer);
    EXPECT_EQ(arr->elems[0].i, 1);
    EXPECT_EQ(arr->elems[1].type_, JsNodeType::Float);
    EXPECT_NEAR(arr->elems[1].f, 2.5, 1e-9);
    EXPECT_EQ(arr->elems[2].type_, JsNodeType::Boolean);
    EXPECT_TRUE(arr->elems[2].b);
    EXPECT_EQ(arr->elems[3].type_, JsNodeType::Null);
}

TEST(ParserTest, ParseStringEscapes) {
    GcHeap heap;
    Parser parser(heap);
    JsValue root;
    const char *json = "{\"s\":\"line\\n\",\"u\":\"\\u4F60\\u597D\"}";
    ASSERT_TRUE(parser.parse(json, std::strlen(json), root)) << parser.error().message;

    const GcObject *obj = as_object(root);
    ASSERT_EQ(obj->size, 2u);
    EXPECT_EQ(to_string(obj->entries[0].key), "s");
    EXPECT_EQ(to_string(as_string(obj->entries[0].value)), "line\n");
    EXPECT_EQ(to_string(obj->entries[1].key), "u");
    const std::string expected = std::string("\xE4\xBD\xA0\xE5\xA5\xBD", 6);
    EXPECT_EQ(to_string(as_string(obj->entries[1].value)), expected);
}

TEST(ParserTest, RejectLeadingZero) {
    GcHeap heap;
    Parser parser(heap);
    JsValue root;
    const char *json = "{\"n\":01}";
    EXPECT_FALSE(parser.parse(json, std::strlen(json), root));
    EXPECT_FALSE(parser.error().message.empty());
}

TEST(ParserTest, StreamParseChunks) {
    GcHeap heap;
    StreamParser parser(heap);
    const char *chunk1 = "{\"a\":[1";
    const char *chunk2 = ",2],\"b\":\"x\"";
    const char *chunk3 = "}";

    EXPECT_NE(parser.parse(chunk1, std::strlen(chunk1)), StreamParser::Status::Error);
    EXPECT_NE(parser.parse(chunk2, std::strlen(chunk2)), StreamParser::Status::Error);
    EXPECT_EQ(parser.parse(chunk3, std::strlen(chunk3)), StreamParser::Status::Complete);
    ASSERT_TRUE(parser.has_result());
    ASSERT_EQ(parser.root().type_, JsNodeType::Object);
}

TEST(ParserTest, StreamFinishPrematureEof) {
    GcHeap heap;
    StreamParser parser(heap);
    const char *chunk = "{\"a\":1";
    EXPECT_NE(parser.parse(chunk, std::strlen(chunk)), StreamParser::Status::Error);
    EXPECT_EQ(parser.finish(), StreamParser::Status::Error);
}

TEST(ParserTest, RejectInvalidUtf8) {
    GcHeap heap;
    Parser parser(heap);
    JsValue root;
    const char json[] = "{\"s\":\"\xC3\x28\"}";
    EXPECT_FALSE(parser.parse(json, sizeof(json) - 1, root));
    EXPECT_FALSE(parser.error().message.empty());
}

TEST(ParserTest, RejectInvalidSurrogatePairs) {
    GcHeap heap;
    Parser parser(heap);
    JsValue root;
    const char *bad_high = "{\"s\":\"\\uD83D\"}";
    EXPECT_FALSE(parser.parse(bad_high, std::strlen(bad_high), root));
    EXPECT_FALSE(parser.error().message.empty());

    const char *bad_low = "{\"s\":\"\\uDC00\"}";
    EXPECT_FALSE(parser.parse(bad_low, std::strlen(bad_low), root));
    EXPECT_FALSE(parser.error().message.empty());
}

TEST(ParserTest, ParseSurrogatePair) {
    GcHeap heap;
    Parser parser(heap);
    JsValue root;
    const char *json = "{\"s\":\"\\uD83D\\uDE00\"}";
    ASSERT_TRUE(parser.parse(json, std::strlen(json), root)) << parser.error().message;
    const GcObject *obj = as_object(root);
    ASSERT_EQ(obj->size, 1u);
    const std::string expected = std::string("\xF0\x9F\x98\x80", 4);
    EXPECT_EQ(to_string(as_string(obj->entries[0].value)), expected);
}
