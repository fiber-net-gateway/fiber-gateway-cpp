#include <gtest/gtest.h>

#include "script/gc/GcInternal.h"

using fiber::script::GcArray;
using fiber::script::GcHeap;
using fiber::script::JsNodeType;
using fiber::script::JsValue;

TEST(ArrayTest, PushPopSetGet) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    GcArray *arr = fiber::script::gc_new_array(&heap, 1);
    ASSERT_NE(arr, nullptr);

    EXPECT_TRUE(fiber::script::gc_array_push(&heap, arr, JsValue::make_integer(1)));
    EXPECT_TRUE(fiber::script::gc_array_push(&heap, arr, JsValue::make_integer(2)));
    ASSERT_EQ(arr->size, 2u);

    const JsValue *v0 = fiber::script::gc_array_get(arr, 0);
    ASSERT_NE(v0, nullptr);
    EXPECT_EQ(js_value_type(*v0), JsNodeType::Integer);
    EXPECT_EQ(js_value_int64(*v0), 1);

    EXPECT_TRUE(fiber::script::gc_array_set(&heap, arr, 1, JsValue::make_integer(5)));
    const JsValue *v1 = fiber::script::gc_array_get(arr, 1);
    ASSERT_NE(v1, nullptr);
    EXPECT_EQ(js_value_type(*v1), JsNodeType::Integer);
    EXPECT_EQ(js_value_int64(*v1), 5);

    EXPECT_TRUE(fiber::script::gc_array_set(&heap, arr, 3, JsValue::make_integer(7)));
    EXPECT_EQ(arr->size, 4u);
    const JsValue *v2 = fiber::script::gc_array_get(arr, 2);
    ASSERT_NE(v2, nullptr);
    EXPECT_EQ(js_value_type(*v2), JsNodeType::Undefined);

    JsValue popped;
    EXPECT_TRUE(fiber::script::gc_array_pop(arr, &popped));
    EXPECT_EQ(js_value_type(popped), JsNodeType::Integer);
    EXPECT_EQ(js_value_int64(popped), 7);
    EXPECT_EQ(arr->size, 3u);

    EXPECT_EQ(fiber::script::gc_array_get(arr, 9), nullptr);
}

TEST(ArrayTest, InsertRemove) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    GcArray *arr = fiber::script::gc_new_array(&heap, 2);
    ASSERT_NE(arr, nullptr);

    EXPECT_TRUE(fiber::script::gc_array_push(&heap, arr, JsValue::make_integer(1)));
    EXPECT_TRUE(fiber::script::gc_array_push(&heap, arr, JsValue::make_integer(3)));
    EXPECT_TRUE(fiber::script::gc_array_insert(&heap, arr, 1, JsValue::make_integer(2)));

    const JsValue *v0 = fiber::script::gc_array_get(arr, 0);
    const JsValue *v1 = fiber::script::gc_array_get(arr, 1);
    const JsValue *v2 = fiber::script::gc_array_get(arr, 2);
    ASSERT_NE(v0, nullptr);
    ASSERT_NE(v1, nullptr);
    ASSERT_NE(v2, nullptr);
    EXPECT_EQ(js_value_int64(*v0), 1);
    EXPECT_EQ(js_value_int64(*v1), 2);
    EXPECT_EQ(js_value_int64(*v2), 3);

    JsValue removed;
    EXPECT_TRUE(fiber::script::gc_array_remove(arr, 1, &removed));
    EXPECT_EQ(js_value_type(removed), JsNodeType::Integer);
    EXPECT_EQ(js_value_int64(removed), 2);
    EXPECT_EQ(arr->size, 2u);

    EXPECT_TRUE(fiber::script::gc_array_insert(&heap, arr, 10, JsValue::make_integer(4)));
    EXPECT_EQ(arr->size, 3u);
    const JsValue *v3 = fiber::script::gc_array_get(arr, 2);
    ASSERT_NE(v3, nullptr);
    EXPECT_EQ(js_value_int64(*v3), 4);

    EXPECT_FALSE(fiber::script::gc_array_remove(arr, 9, nullptr));
}
