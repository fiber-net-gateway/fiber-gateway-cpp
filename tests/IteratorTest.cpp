#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "script/gc/GcInternal.h"

using fiber::script::GcArray;
using fiber::script::GcHeap;
using fiber::script::GcIterator;
using fiber::script::GcIterStep;
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

GcString *make_key(GcHeap &heap, const char *data) {
    return fiber::script::gc_new_string(&heap, data, std::strlen(data));
}

} // namespace

TEST(IteratorTest, ArrayIteratorVisitsEachElement) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    GcArray *arr = fiber::script::gc_new_array(&heap, 4);
    ASSERT_NE(arr, nullptr);
    arr->elems[0] = JsValue::make_integer(10);
    arr->elems[1] = JsValue::make_integer(20);
    arr->size = 2;

    GcIterator *iter = fiber::script::gc_new_array_iterator(&heap, arr);
    ASSERT_NE(iter, nullptr);

    EXPECT_EQ(fiber::script::gc_iterator_next(&heap, iter), GcIterStep::Item);
    EXPECT_TRUE(iter->has_current);
    EXPECT_EQ(js_value_type(iter->current_key), JsNodeType::Integer);
    EXPECT_EQ(js_value_int64(iter->current_key), 0);
    EXPECT_EQ(js_value_int64(iter->current_value), 10);

    EXPECT_EQ(fiber::script::gc_iterator_next(&heap, iter), GcIterStep::Item);
    EXPECT_EQ(js_value_int64(iter->current_key), 1);
    EXPECT_EQ(js_value_int64(iter->current_value), 20);

    EXPECT_EQ(fiber::script::gc_iterator_next(&heap, iter), GcIterStep::Done);
    EXPECT_FALSE(iter->has_current);
}

TEST(IteratorTest, ArrayIteratorFailsOnMutation) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    GcArray *arr = fiber::script::gc_new_array(&heap, 4);
    ASSERT_NE(arr, nullptr);
    arr->elems[0] = JsValue::make_integer(1);
    arr->elems[1] = JsValue::make_integer(2);
    arr->size = 2;

    GcIterator *iter = fiber::script::gc_new_array_iterator(&heap, arr);
    ASSERT_NE(iter, nullptr);

    EXPECT_EQ(fiber::script::gc_iterator_next(&heap, iter), GcIterStep::Item);
    EXPECT_EQ(js_value_int64(iter->current_value), 1);

    // Mutating the array during iteration must be detected on the next step.
    arr->elems[2] = JsValue::make_integer(3);
    arr->size = 3;
    arr->version += 1;

    EXPECT_EQ(fiber::script::gc_iterator_next(&heap, iter), GcIterStep::Mutated);
}

TEST(IteratorTest, ObjectIteratorVisitsEachEntry) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    GcObject *obj = fiber::script::gc_new_object(&heap, 4);
    ASSERT_NE(obj, nullptr);
    GcString *key_a = make_key(heap, "a");
    GcString *key_b = make_key(heap, "b");
    ASSERT_NE(key_a, nullptr);
    ASSERT_NE(key_b, nullptr);
    ASSERT_TRUE(fiber::script::gc_object_set(&heap, obj, key_a, JsValue::make_integer(1)));
    ASSERT_TRUE(fiber::script::gc_object_set(&heap, obj, key_b, JsValue::make_integer(2)));

    GcIterator *iter = fiber::script::gc_new_object_iterator(&heap, obj);
    ASSERT_NE(iter, nullptr);

    EXPECT_EQ(fiber::script::gc_iterator_next(&heap, iter), GcIterStep::Item);
    EXPECT_TRUE(iter->has_current);
    EXPECT_EQ(to_string(as_string(iter->current_key)), "a");
    EXPECT_EQ(js_value_int64(iter->current_value), 1);

    EXPECT_EQ(fiber::script::gc_iterator_next(&heap, iter), GcIterStep::Item);
    EXPECT_EQ(to_string(as_string(iter->current_key)), "b");
    EXPECT_EQ(js_value_int64(iter->current_value), 2);

    EXPECT_EQ(fiber::script::gc_iterator_next(&heap, iter), GcIterStep::Done);
    EXPECT_FALSE(iter->has_current);
}

TEST(IteratorTest, ObjectIteratorFailsOnMutation) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    GcObject *obj = fiber::script::gc_new_object(&heap, 4);
    ASSERT_NE(obj, nullptr);
    GcString *key_a = make_key(heap, "a");
    GcString *key_b = make_key(heap, "b");
    GcString *key_c = make_key(heap, "c");
    ASSERT_NE(key_a, nullptr);
    ASSERT_NE(key_b, nullptr);
    ASSERT_NE(key_c, nullptr);
    ASSERT_TRUE(fiber::script::gc_object_set(&heap, obj, key_a, JsValue::make_integer(1)));
    ASSERT_TRUE(fiber::script::gc_object_set(&heap, obj, key_b, JsValue::make_integer(2)));

    GcIterator *iter = fiber::script::gc_new_object_iterator(&heap, obj);
    ASSERT_NE(iter, nullptr);

    EXPECT_EQ(fiber::script::gc_iterator_next(&heap, iter), GcIterStep::Item);
    EXPECT_EQ(to_string(as_string(iter->current_key)), "a");

    // Adding a key during iteration must be detected on the next step.
    ASSERT_TRUE(fiber::script::gc_object_set(&heap, obj, key_c, JsValue::make_integer(3)));

    EXPECT_EQ(fiber::script::gc_iterator_next(&heap, iter), GcIterStep::Mutated);
}

TEST(IteratorTest, EmptyIteratorIsImmediatelyDone) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    GcIterator *iter = fiber::script::gc_new_array_iterator(&heap, nullptr);
    ASSERT_NE(iter, nullptr);
    EXPECT_EQ(fiber::script::gc_iterator_next(&heap, iter), GcIterStep::Done);
    EXPECT_FALSE(iter->has_current);
}
