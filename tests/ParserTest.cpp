#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "common/json/JsGc.h"
#include "common/json/JsonDecode.h"

using fiber::json::GcArray;
using fiber::json::GcHeap;
using fiber::json::GcObject;
using fiber::json::GcObjectEntry;
using fiber::json::GcString;
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

const GcObject *as_object(const JsValue &value) { return js_value_heap_ptr<const GcObject>(value); }

GcObject *as_object_mutable(const JsValue &value) { return js_value_heap_ptr<GcObject>(const_cast<JsValue &>(value)); }

const GcArray *as_array(const JsValue &value) { return js_value_heap_ptr<const GcArray>(value); }

const GcString *as_string(const JsValue &value) { return js_value_heap_ptr<const GcString>(value); }

const GcObjectEntry *entry_at(const GcObject *obj, std::size_t index) {
    return fiber::json::gc_object_entry_at(obj, index);
}

GcString *make_key(GcHeap &heap, const char *data) {
    return fiber::json::gc_new_string(&heap, data, std::strlen(data));
}

} // namespace

TEST(ParserTest, ParseObjectAndArray) {
    GcHeap heap;
    Parser parser(heap);
    JsValue root;
    const char *json = "{\"name\":\"fiber\",\"nums\":[1,2.5,true,null]}";
    ASSERT_TRUE(parser.parse(json, std::strlen(json), root)) << parser.error().message;
    ASSERT_EQ(js_value_type(root), JsNodeType::Object);

    const GcObject *obj = as_object(root);
    ASSERT_EQ(obj->size, 2u);
    const GcObjectEntry *name_entry = entry_at(obj, 0);
    ASSERT_NE(name_entry, nullptr);
    EXPECT_EQ(to_string(name_entry->key), "name");
    ASSERT_EQ(js_value_type(name_entry->value), JsNodeType::String);
    EXPECT_EQ(to_string(as_string(name_entry->value)), "fiber");

    const GcObjectEntry *nums_entry = entry_at(obj, 1);
    ASSERT_NE(nums_entry, nullptr);
    EXPECT_EQ(to_string(nums_entry->key), "nums");
    ASSERT_EQ(js_value_type(nums_entry->value), JsNodeType::Array);
    const GcArray *arr = as_array(nums_entry->value);
    ASSERT_EQ(arr->size, 4u);
    EXPECT_EQ(js_value_type(arr->elems[0]), JsNodeType::Integer);
    EXPECT_EQ(js_value_int64(arr->elems[0]), 1);
    EXPECT_EQ(js_value_type(arr->elems[1]), JsNodeType::Float);
    EXPECT_NEAR(js_value_double(arr->elems[1]), 2.5, 1e-9);
    EXPECT_EQ(js_value_type(arr->elems[2]), JsNodeType::Boolean);
    EXPECT_TRUE(js_value_bool(arr->elems[2]));
    EXPECT_EQ(js_value_type(arr->elems[3]), JsNodeType::Null);
}

TEST(ParserTest, ParseStringEscapes) {
    GcHeap heap;
    Parser parser(heap);
    JsValue root;
    const char *json = R"({"s":"line\n","u":"\u4F60\u597D"})";
    ASSERT_TRUE(parser.parse(json, std::strlen(json), root)) << parser.error().message;

    const GcObject *obj = as_object(root);
    ASSERT_EQ(obj->size, 2u);
    const GcObjectEntry *s_entry = entry_at(obj, 0);
    ASSERT_NE(s_entry, nullptr);
    EXPECT_EQ(to_string(s_entry->key), "s");
    EXPECT_EQ(to_string(as_string(s_entry->value)), "line\n");
    const GcObjectEntry *u_entry = entry_at(obj, 1);
    ASSERT_NE(u_entry, nullptr);
    EXPECT_EQ(to_string(u_entry->key), "u");
    const std::string expected = std::string("\xE4\xBD\xA0\xE5\xA5\xBD", 6);
    EXPECT_EQ(to_string(as_string(u_entry->value)), expected);
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
    ASSERT_EQ(js_value_type(parser.root()), JsNodeType::Object);
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
    const GcObjectEntry *entry = entry_at(obj, 0);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(to_string(as_string(entry->value)), expected);
}

TEST(ParserTest, DuplicateKeysOverwrite) {
    GcHeap heap;
    Parser parser(heap);
    JsValue root;
    const char *json = "{\"a\":1,\"a\":2,\"b\":3}";
    ASSERT_TRUE(parser.parse(json, std::strlen(json), root)) << parser.error().message;
    const GcObject *obj = as_object(root);
    ASSERT_EQ(obj->size, 2u);
    const GcObjectEntry *a_entry = entry_at(obj, 0);
    ASSERT_NE(a_entry, nullptr);
    EXPECT_EQ(to_string(a_entry->key), "a");
    ASSERT_EQ(js_value_type(a_entry->value), JsNodeType::Integer);
    EXPECT_EQ(js_value_int64(a_entry->value), 2);
    const GcObjectEntry *b_entry = entry_at(obj, 1);
    ASSERT_NE(b_entry, nullptr);
    EXPECT_EQ(to_string(b_entry->key), "b");
    ASSERT_EQ(js_value_type(b_entry->value), JsNodeType::Integer);
    EXPECT_EQ(js_value_int64(a_entry->value), 2);
    EXPECT_EQ(js_value_int64(b_entry->value), 3);

    GcString *key_a = make_key(heap, "a");
    ASSERT_NE(key_a, nullptr);
    const JsValue *value = fiber::json::gc_object_get(obj, key_a);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(js_value_type(*value), JsNodeType::Integer);
    EXPECT_EQ(js_value_int64(*value), 2);
}

TEST(ParserTest, RemoveKeysKeepsOrder) {
    GcHeap heap;
    Parser parser(heap);
    JsValue root;
    const char *json = "{\"a\":1,\"b\":2,\"c\":3}";
    ASSERT_TRUE(parser.parse(json, std::strlen(json), root)) << parser.error().message;
    GcObject *obj = as_object_mutable(root);
    ASSERT_EQ(obj->size, 3u);
    GcString *key_b = make_key(heap, "b");
    ASSERT_NE(key_b, nullptr);
    EXPECT_TRUE(fiber::json::gc_object_remove(obj, key_b));
    EXPECT_EQ(obj->size, 2u);

    const GcObjectEntry *first = entry_at(obj, 0);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(to_string(first->key), "a");
    const GcObjectEntry *second = entry_at(obj, 1);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(to_string(second->key), "c");

    const JsValue *missing = fiber::json::gc_object_get(obj, key_b);
    EXPECT_EQ(missing, nullptr);
}
