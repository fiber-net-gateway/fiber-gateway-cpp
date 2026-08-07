#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include <fiber/common/json/JsonEncode.h>
#include <fiber/script/gc/GcInternal.h>
#include <fiber/script/json/JsValueDecode.h>
#include <fiber/script/json/JsValueEncode.h>

using fiber::json::DecodeStatus;
using fiber::json::Generator;
using fiber::json::OutputSink;
using fiber::json::ParseError;
using fiber::script::GcArray;
using fiber::script::GcHeap;
using fiber::script::GcObject;
using fiber::script::GcString;
using fiber::script::JsNodeType;
using fiber::script::JsValue;
using fiber::script::ValueHandle;

namespace {

class StringSink final : public OutputSink {
public:
    [[nodiscard]] bool write(const char *data, size_t len) override {
        if (len == 0) {
            return true;
        }
        if (!data) {
            return false;
        }
        output.append(data, len);
        return true;
    }

    void reset() override { output.clear(); }

    std::string output;
};

ValueHandle decode_ok(GcHeap &heap, const char *text) {
    ValueHandle out = heap.global_value();
    EXPECT_NE(out, nullptr);
    if (!out) {
        return nullptr;
    }
    ParseError error;
    DecodeStatus status = fiber::script::json::decode_js_value(heap, text, std::strlen(text), out, &error);
    EXPECT_EQ(status, DecodeStatus::Complete) << (error.message ? error.message : "");
    return out;
}

GcString *make_key(GcHeap &heap, const char *key) { return fiber::script::gc_new_string(&heap, key, std::strlen(key)); }

const JsValue *object_get(GcHeap &heap, const GcObject *obj, const char *key) {
    GcHeap::NoGcScope no_gc(heap);
    GcString *key_str = make_key(heap, key);
    EXPECT_NE(key_str, nullptr);
    if (!key_str) {
        return nullptr;
    }
    return fiber::script::gc_object_get(obj, key_str);
}

std::string to_utf8(const JsValue &value) {
    auto *str = fiber::script::js_value_heap_ptr<const GcString>(value);
    EXPECT_NE(str, nullptr);
    std::string out;
    EXPECT_TRUE(fiber::script::gc_string_to_utf8(str, out));
    return out;
}

} // namespace

TEST(JsValueDecodeTest, DecodeScalarValues) {
    {
        GcHeap heap;
        ValueHandle out = decode_ok(heap, "null");
        ASSERT_NE(out, nullptr);
        EXPECT_EQ(fiber::script::js_value_type(*out), JsNodeType::Null);
    }
    {
        GcHeap heap;
        ValueHandle out = decode_ok(heap, "true");
        ASSERT_NE(out, nullptr);
        EXPECT_EQ(fiber::script::js_value_type(*out), JsNodeType::Boolean);
        EXPECT_TRUE(fiber::script::js_value_bool(*out));
    }
    {
        GcHeap heap;
        ValueHandle out = decode_ok(heap, "-42");
        ASSERT_NE(out, nullptr);
        EXPECT_EQ(fiber::script::js_value_type(*out), JsNodeType::Integer);
        EXPECT_EQ(fiber::script::js_value_int64(*out), -42);
    }
    {
        GcHeap heap;
        ValueHandle out = decode_ok(heap, "1.25");
        ASSERT_NE(out, nullptr);
        EXPECT_EQ(fiber::script::js_value_type(*out), JsNodeType::Float);
        EXPECT_DOUBLE_EQ(fiber::script::js_value_double(*out), 1.25);
    }
}

TEST(JsValueDecodeTest, DecodesEscapedUnicodeString) {
    GcHeap heap;
    ValueHandle out = decode_ok(heap, "\"line\\n\\u00E9 \\uD83D\\uDE00\"");
    ASSERT_NE(out, nullptr);
    ASSERT_EQ(fiber::script::js_value_type(*out), JsNodeType::String);

    std::string expected("line\n", 5);
    expected.push_back(static_cast<char>(0xC3));
    expected.push_back(static_cast<char>(0xA9));
    expected.push_back(' ');
    expected.push_back(static_cast<char>(0xF0));
    expected.push_back(static_cast<char>(0x9F));
    expected.push_back(static_cast<char>(0x98));
    expected.push_back(static_cast<char>(0x80));
    EXPECT_EQ(to_utf8(*out), expected);
}

TEST(JsValueDecodeTest, DecodesArrayAndObject) {
    GcHeap heap;
    ValueHandle out = decode_ok(heap, "{\"items\":[1,false,{\"name\":\"fiber\"}],\"empty\":{}}");
    ASSERT_NE(out, nullptr);
    ASSERT_EQ(fiber::script::js_value_type(*out), JsNodeType::Object);

    auto *obj = fiber::script::js_value_heap_ptr<GcObject>(*out);
    ASSERT_NE(obj, nullptr);

    const JsValue *items_value = object_get(heap, obj, "items");
    ASSERT_NE(items_value, nullptr);
    ASSERT_EQ(fiber::script::js_value_type(*items_value), JsNodeType::Array);
    auto *items = fiber::script::js_value_heap_ptr<const GcArray>(*items_value);
    ASSERT_NE(items, nullptr);
    ASSERT_EQ(items->size, 3u);
    EXPECT_EQ(fiber::script::js_value_int64(items->elems[0]), 1);
    EXPECT_FALSE(fiber::script::js_value_bool(items->elems[1]));

    ASSERT_EQ(fiber::script::js_value_type(items->elems[2]), JsNodeType::Object);
    auto *nested = fiber::script::js_value_heap_ptr<const GcObject>(items->elems[2]);
    ASSERT_NE(nested, nullptr);
    const JsValue *name = object_get(heap, nested, "name");
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(to_utf8(*name), "fiber");

    const JsValue *empty = object_get(heap, obj, "empty");
    ASSERT_NE(empty, nullptr);
    ASSERT_EQ(fiber::script::js_value_type(*empty), JsNodeType::Object);
    auto *empty_obj = fiber::script::js_value_heap_ptr<const GcObject>(*empty);
    ASSERT_NE(empty_obj, nullptr);
    EXPECT_EQ(empty_obj->size, 0u);
}

TEST(JsValueDecodeTest, DuplicateKeysOverwriteInOriginalOrder) {
    GcHeap heap;
    ValueHandle out = decode_ok(heap, "{\"a\":1,\"b\":2,\"a\":3}");
    ASSERT_NE(out, nullptr);

    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(fiber::script::json::encode_js_value(gen, *out), Generator::Result::OK);
    EXPECT_EQ(sink.output, "{\"a\":3,\"b\":2}");
}

TEST(JsValueDecodeTest, RejectsInvalidInput) {
    GcHeap heap;
    ValueHandle out = heap.global_value();
    ASSERT_NE(out, nullptr);

    ParseError error;
    DecodeStatus status = fiber::script::json::decode_js_value(heap, "{\"a\":1} trailing",
                                                               std::strlen("{\"a\":1} trailing"), out, &error);
    EXPECT_EQ(status, DecodeStatus::Error);
    EXPECT_NE(error.message, nullptr);
    EXPECT_TRUE(fiber::script::js_value_is_undefined(*out));

    status = fiber::script::json::decode_js_value(heap, "9223372036854775808", std::strlen("9223372036854775808"), out,
                                                  &error);
    EXPECT_EQ(status, DecodeStatus::Error);
    EXPECT_NE(error.message, nullptr);
    EXPECT_TRUE(fiber::script::js_value_is_undefined(*out));
}

TEST(JsValueDecodeTest, RejectsTooDeepJson) {
    GcHeap heap;
    ValueHandle out = heap.global_value();
    ASSERT_NE(out, nullptr);

    std::string json;
    for (int i = 0; i < 140; ++i) {
        json.push_back('[');
    }
    for (int i = 0; i < 140; ++i) {
        json.push_back(']');
    }

    ParseError error;
    DecodeStatus status = fiber::script::json::decode_js_value(heap, json.data(), json.size(), out, &error);
    EXPECT_NE(status, DecodeStatus::Complete);
    EXPECT_NE(error.message, nullptr);
    EXPECT_TRUE(fiber::script::js_value_is_undefined(*out));
}

TEST(JsValueDecodeTest, SurvivesCollectionDuringBuild) {
    GcHeap heap;
    heap.threshold = 1;
    ValueHandle out = heap.global_value();
    ASSERT_NE(out, nullptr);

    const char *json = "{\"items\":[{\"name\":\"a\",\"value\":1},{\"name\":\"b\",\"value\":2}],\"ok\":true}";
    ParseError error;
    DecodeStatus status = fiber::script::json::decode_js_value(heap, json, std::strlen(json), out, &error);
    ASSERT_EQ(status, DecodeStatus::Complete) << (error.message ? error.message : "");

    heap.collect();

    ASSERT_EQ(fiber::script::js_value_type(*out), JsNodeType::Object);
    auto *obj = fiber::script::js_value_heap_ptr<GcObject>(*out);
    ASSERT_NE(obj, nullptr);
    const JsValue *items_value = object_get(heap, obj, "items");
    ASSERT_NE(items_value, nullptr);
    auto *items = fiber::script::js_value_heap_ptr<const GcArray>(*items_value);
    ASSERT_NE(items, nullptr);
    ASSERT_EQ(items->size, 2u);

    const JsValue *ok = object_get(heap, obj, "ok");
    ASSERT_NE(ok, nullptr);
    EXPECT_TRUE(fiber::script::js_value_bool(*ok));
}

TEST(JsValueDecodeTest, DoesNotCollectDuringDecodeWhenThresholdCrossed) {
    GcHeap heap;
    // threshold=1 forces every decoded allocation to request a collection.
    heap.threshold = 1;
    ValueHandle out = heap.global_value();
    ASSERT_NE(out, nullptr);

    const char *json = "{\"items\":[1,2,3],\"name\":\"hello\"}";
    ParseError error;
    DecodeStatus status = fiber::script::json::decode_js_value(heap, json, std::strlen(json), out, &error);
    ASSERT_EQ(status, DecodeStatus::Complete) << (error.message ? error.message : "");

    // Decoding crossed the limit repeatedly, but the deferred NoGcScope wrapped
    // around decode suppressed every collection (without it, the per-primitive
    // NoGcScopes inside gc_make_*/gc_array_push/gc_object_set would each fire a
    // full collect as depth returned to 0). A collect raises threshold to
    // >= 1MiB; observing it still at 1 proves none ran during decode.
    EXPECT_EQ(heap.threshold, 1u);

    // The decoded result stays rooted and intact through a post-decode collect.
    heap.collect();
    ASSERT_EQ(fiber::script::js_value_type(*out), JsNodeType::Object);
}

TEST(JsValueDecodeTest, RoundTripsThroughEncoder) {
    GcHeap heap;
    ValueHandle out = decode_ok(heap, "{\"n\":1,\"arr\":[true,null,\"x\"]}");
    ASSERT_NE(out, nullptr);

    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(fiber::script::json::encode_js_value(gen, *out), Generator::Result::OK);
    EXPECT_EQ(sink.output, "{\"n\":1,\"arr\":[true,null,\"x\"]}");
}
