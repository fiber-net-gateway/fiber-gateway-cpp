#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "script/gc/GcInternal.h"

using fiber::script::GcArray;
using fiber::script::GcHeap;
using fiber::script::GcIterator;
using fiber::script::GcIteratorMode;
using fiber::script::GcObject;
using fiber::script::GcString;
using fiber::script::JsNodeType;
using fiber::script::JsValue;

namespace {

std::string to_string(const GcString *str) {
    if (!str) {
        return {};
    }
    std::string out;
    if (!fiber::script::gc_string_to_utf8(str, out)) {
        return {};
    }
    return out;
}

const GcString *as_string(const JsValue &value) { return js_value_heap_ptr<const GcString>(value); }

const GcArray *as_array(const JsValue &value) { return js_value_heap_ptr<const GcArray>(value); }

GcString *make_key(GcHeap &heap, const char *data) {
    return fiber::script::gc_new_string(&heap, data, std::strlen(data));
}

} // namespace

TEST(IteratorTest, ArrayIteratorSeesAppends) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    GcArray *arr = fiber::script::gc_new_array(&heap, 4);
    ASSERT_NE(arr, nullptr);

    arr->elems[0] = JsValue::make_integer(1);
    arr->size = 1;
    arr->version += 1;
    arr->elems[1] = JsValue::make_integer(2);
    arr->size = 2;
    arr->version += 1;

    GcIterator *iter = fiber::script::gc_new_array_iterator(&heap, arr, GcIteratorMode::Values);
    ASSERT_NE(iter, nullptr);

    JsValue out;
    bool done = false;
    ASSERT_TRUE(fiber::script::gc_iterator_next(&heap, iter, out, done));
    EXPECT_FALSE(done);
    EXPECT_EQ(js_value_type(out), JsNodeType::Integer);
    EXPECT_EQ(js_value_int64(out), 1);

    arr->elems[2] = JsValue::make_integer(3);
    arr->size = 3;
    arr->version += 1;

    ASSERT_TRUE(fiber::script::gc_iterator_next(&heap, iter, out, done));
    EXPECT_FALSE(done);
    EXPECT_EQ(js_value_type(out), JsNodeType::Integer);
    EXPECT_EQ(js_value_int64(out), 2);

    ASSERT_TRUE(fiber::script::gc_iterator_next(&heap, iter, out, done));
    EXPECT_FALSE(done);
    EXPECT_EQ(js_value_type(out), JsNodeType::Integer);
    EXPECT_EQ(js_value_int64(out), 3);

    ASSERT_TRUE(fiber::script::gc_iterator_next(&heap, iter, out, done));
    EXPECT_TRUE(done);
}

TEST(IteratorTest, ObjectIteratorSnapshotOnMutation) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    GcObject *obj = fiber::script::gc_new_object(&heap, 4);
    ASSERT_NE(obj, nullptr);
    GcString *key_a = make_key(heap, "a");
    GcString *key_b = make_key(heap, "b");
    GcString *key_c = make_key(heap, "c");
    GcString *key_d = make_key(heap, "d");
    ASSERT_NE(key_a, nullptr);
    ASSERT_NE(key_b, nullptr);
    ASSERT_NE(key_c, nullptr);
    ASSERT_NE(key_d, nullptr);

    ASSERT_TRUE(fiber::script::gc_object_set(&heap, obj, key_a, JsValue::make_integer(1)));
    ASSERT_TRUE(fiber::script::gc_object_set(&heap, obj, key_b, JsValue::make_integer(2)));

    GcIterator *iter = fiber::script::gc_new_object_iterator(&heap, obj, GcIteratorMode::Keys);
    ASSERT_NE(iter, nullptr);

    JsValue out;
    bool done = false;
    ASSERT_TRUE(fiber::script::gc_iterator_next(&heap, iter, out, done));
    EXPECT_FALSE(done);
    EXPECT_EQ(js_value_type(out), JsNodeType::String);
    EXPECT_EQ(to_string(as_string(out)), "a");

    ASSERT_TRUE(fiber::script::gc_object_set(&heap, obj, key_c, JsValue::make_integer(3)));

    ASSERT_TRUE(fiber::script::gc_iterator_next(&heap, iter, out, done));
    EXPECT_FALSE(done);
    EXPECT_EQ(js_value_type(out), JsNodeType::String);
    EXPECT_EQ(to_string(as_string(out)), "b");

    ASSERT_TRUE(fiber::script::gc_object_set(&heap, obj, key_d, JsValue::make_integer(4)));

    ASSERT_TRUE(fiber::script::gc_iterator_next(&heap, iter, out, done));
    EXPECT_FALSE(done);
    EXPECT_EQ(js_value_type(out), JsNodeType::String);
    EXPECT_EQ(to_string(as_string(out)), "c");

    ASSERT_TRUE(fiber::script::gc_iterator_next(&heap, iter, out, done));
    EXPECT_TRUE(done);
}

TEST(IteratorTest, ObjectIteratorEntries) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    GcObject *obj = fiber::script::gc_new_object(&heap, 2);
    ASSERT_NE(obj, nullptr);
    GcString *key = make_key(heap, "k");
    ASSERT_NE(key, nullptr);
    ASSERT_TRUE(fiber::script::gc_object_set(&heap, obj, key, JsValue::make_integer(9)));

    GcIterator *iter = fiber::script::gc_new_object_iterator(&heap, obj, GcIteratorMode::Entries);
    ASSERT_NE(iter, nullptr);

    JsValue out;
    bool done = false;
    ASSERT_TRUE(fiber::script::gc_iterator_next(&heap, iter, out, done));
    EXPECT_FALSE(done);
    ASSERT_EQ(js_value_type(out), JsNodeType::Array);
    const GcArray *arr = as_array(out);
    ASSERT_NE(arr, nullptr);
    ASSERT_EQ(arr->size, 2u);
    EXPECT_EQ(js_value_type(arr->elems[0]), JsNodeType::String);
    EXPECT_EQ(to_string(as_string(arr->elems[0])), "k");
    EXPECT_EQ(js_value_type(arr->elems[1]), JsNodeType::Integer);
    EXPECT_EQ(js_value_int64(arr->elems[1]), 9);
}
