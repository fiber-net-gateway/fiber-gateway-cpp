#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <utility>

#include "common/json/JsonEncode.h"
#include "common/json/JsGc.h"
#include "common/json/JsValueEncode.h"

using fiber::json::GcArray;
using fiber::json::GcHeap;
using fiber::json::GcObject;
using fiber::json::GcString;
using fiber::json::Generator;
using fiber::json::JsNodeType;
using fiber::json::JsValue;
using fiber::json::OutputSink;

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

    void reset() override {
        output.clear();
    }

    std::string output;
};

GcString *make_key(GcHeap &heap, const char *data) {
    return fiber::json::gc_new_string(&heap, data, std::strlen(data));
}

} // namespace

TEST(JsValueEncodeTest, EncodeObjectOrderAndOverwrite) {
    GcHeap heap;
    GcObject *obj = fiber::json::gc_new_object(&heap, 4);
    ASSERT_NE(obj, nullptr);

    GcString *key_a = make_key(heap, "a");
    GcString *key_b = make_key(heap, "b");
    GcString *key_a2 = make_key(heap, "a");
    ASSERT_NE(key_a, nullptr);
    ASSERT_NE(key_b, nullptr);
    ASSERT_NE(key_a2, nullptr);

    ASSERT_TRUE(fiber::json::gc_object_set(&heap, obj, key_a, JsValue::make_integer(1)));
    ASSERT_TRUE(fiber::json::gc_object_set(&heap, obj, key_b, JsValue::make_integer(2)));
    ASSERT_TRUE(fiber::json::gc_object_set(&heap, obj, key_a2, JsValue::make_integer(3)));

    JsValue root;
    root.type_ = JsNodeType::Object;
    root.gc = &obj->hdr;

    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(fiber::json::encode_js_value(gen, root), Generator::Result::OK);
    EXPECT_EQ(sink.output, "{\"a\":3,\"b\":2}");
}

TEST(JsValueEncodeTest, EncodeArrayWithStrings) {
    GcHeap heap;
    GcArray *arr = fiber::json::gc_new_array(&heap, 3);
    ASSERT_NE(arr, nullptr);

    JsValue str = JsValue::make_string(heap, "hi", 2);
    ASSERT_EQ(str.type_, JsNodeType::HeapString);

    arr->elems[0] = JsValue::make_integer(1);
    arr->elems[1] = JsValue::make_boolean(false);
    arr->elems[2] = std::move(str);
    arr->size = 3;

    JsValue root;
    root.type_ = JsNodeType::Array;
    root.gc = &arr->hdr;

    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(fiber::json::encode_js_value(gen, root), Generator::Result::OK);
    EXPECT_EQ(sink.output, "[1,false,\"hi\"]");
}
