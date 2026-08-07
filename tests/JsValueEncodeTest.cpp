#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <utility>

#include <fiber/common/json/JsonEncode.h>
#include <fiber/script/gc/GcInternal.h>
#include <fiber/script/json/JsValueEncode.h>

using fiber::json::Generator;
using fiber::json::OutputSink;
using fiber::script::GcArray;
using fiber::script::GcException;
using fiber::script::GcHeap;
using fiber::script::GcHeapKind;
using fiber::script::GcObject;
using fiber::script::GcString;
using fiber::script::JsNodeType;
using fiber::script::JsValue;

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

GcString *make_key(GcHeap &heap, const char *data) {
    return fiber::script::gc_new_string(&heap, data, std::strlen(data));
}

} // namespace

TEST(JsValueEncodeTest, EncodeObjectOrderAndOverwrite) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    GcObject *obj = fiber::script::gc_new_object(&heap, 4);
    ASSERT_NE(obj, nullptr);

    GcString *key_a = make_key(heap, "a");
    GcString *key_b = make_key(heap, "b");
    GcString *key_a2 = make_key(heap, "a");
    ASSERT_NE(key_a, nullptr);
    ASSERT_NE(key_b, nullptr);
    ASSERT_NE(key_a2, nullptr);

    ASSERT_TRUE(fiber::script::gc_object_set(&heap, obj, key_a, JsValue::make_integer(1)));
    ASSERT_TRUE(fiber::script::gc_object_set(&heap, obj, key_b, JsValue::make_integer(2)));
    ASSERT_TRUE(fiber::script::gc_object_set(&heap, obj, key_a2, JsValue::make_integer(3)));

    JsValue root = js_make_heap_ref(&obj->hdr, GcHeapKind::Object);

    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(fiber::script::json::encode_js_value(gen, root), Generator::Result::OK);
    EXPECT_EQ(sink.output, "{\"a\":3,\"b\":2}");
}

TEST(JsValueEncodeTest, EncodeArrayWithStrings) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    GcArray *arr = fiber::script::gc_new_array(&heap, 3);
    ASSERT_NE(arr, nullptr);

    JsValue str = JsValue::make_string(heap, "hi", 2);
    ASSERT_EQ(js_value_type(str), JsNodeType::String);

    arr->elems[0] = JsValue::make_integer(1);
    arr->elems[1] = JsValue::make_boolean(false);
    arr->elems[2] = std::move(str);
    arr->size = 3;

    JsValue root = js_make_heap_ref(&arr->hdr, GcHeapKind::Array);

    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(fiber::script::json::encode_js_value(gen, root), Generator::Result::OK);
    EXPECT_EQ(sink.output, "[1,false,\"hi\"]");
}

TEST(JsValueEncodeTest, EncodeUtf16HeapStringWithoutTemporaryUtf8String) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);

    const char16_t text[] = {static_cast<char16_t>(0x4E2D)};
    GcString *str = fiber::script::gc_new_string_utf16(&heap, text, 1);
    ASSERT_NE(str, nullptr);
    JsValue root = js_make_heap_ref(&str->hdr, GcHeapKind::String);

    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(fiber::script::json::encode_js_value(gen, root), Generator::Result::OK);

    std::string expected = "\"";
    expected.append("\xE4\xB8\xAD", 3);
    expected.push_back('"');
    EXPECT_EQ(sink.output, expected);
}

TEST(JsValueEncodeTest, RejectsMalformedUtf16HeapStringBeforeWriting) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);

    const char16_t bad[] = {static_cast<char16_t>(0xD800)};
    GcString *str = fiber::script::gc_new_string_utf16(&heap, bad, 1);
    ASSERT_NE(str, nullptr);
    JsValue root = js_make_heap_ref(&str->hdr, GcHeapKind::String);

    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(fiber::script::json::encode_js_value(gen, root), Generator::Result::InvalidString);
    EXPECT_TRUE(sink.output.empty());
}

TEST(JsValueEncodeTest, EncodeExceptionObject) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    GcObject *meta_obj = fiber::script::gc_new_object(&heap, 2);
    ASSERT_NE(meta_obj, nullptr);
    GcString *code_key = make_key(heap, "code");
    ASSERT_NE(code_key, nullptr);
    ASSERT_TRUE(fiber::script::gc_object_set(&heap, meta_obj, code_key, JsValue::make_integer(7)));
    JsValue meta = js_make_heap_ref(&meta_obj->hdr, GcHeapKind::Object);

    const char *name = "TypeError";
    const char *message = "boom";
    GcException *exc =
            fiber::script::gc_new_exception(&heap, 42, name, std::strlen(name), message, std::strlen(message), meta);
    ASSERT_NE(exc, nullptr);

    JsValue root = js_make_heap_ref(&exc->hdr, GcHeapKind::Exception);

    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(fiber::script::json::encode_js_value(gen, root), Generator::Result::OK);
    EXPECT_EQ(sink.output, "{\"position\":42,\"name\":\"TypeError\",\"message\":\"boom\",\"meta\":{\"code\":7}}");
}

TEST(JsValueEncodeTest, EncodeExceptionDefaultMeta) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    const char *name = "RangeError";
    GcException *exc = fiber::script::gc_new_exception(&heap, -1, name, std::strlen(name), nullptr, 0);
    ASSERT_NE(exc, nullptr);

    JsValue root = js_make_heap_ref(&exc->hdr, GcHeapKind::Exception);

    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(fiber::script::json::encode_js_value(gen, root), Generator::Result::OK);
    EXPECT_EQ(sink.output, "{\"position\":-1,\"name\":\"RangeError\",\"message\":null,\"meta\":null}");
}
