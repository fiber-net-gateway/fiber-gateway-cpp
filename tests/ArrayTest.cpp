#include <gtest/gtest.h>

#include "common/json/JsGc.h"

using fiber::json::GcArray;
using fiber::json::GcHeap;
using fiber::json::JsNodeType;
using fiber::json::JsValue;

TEST(ArrayTest, PushPopSetGet) {
    GcHeap heap;
    GcArray *arr = fiber::json::gc_new_array(&heap, 1);
    ASSERT_NE(arr, nullptr);

    EXPECT_TRUE(fiber::json::gc_array_push(&heap, arr, JsValue::make_integer(1)));
    EXPECT_TRUE(fiber::json::gc_array_push(&heap, arr, JsValue::make_integer(2)));
    ASSERT_EQ(arr->size, 2u);

    const JsValue *v0 = fiber::json::gc_array_get(arr, 0);
    ASSERT_NE(v0, nullptr);
    EXPECT_EQ(v0->type_, JsNodeType::Integer);
    EXPECT_EQ(v0->i, 1);

    EXPECT_TRUE(fiber::json::gc_array_set(&heap, arr, 1, JsValue::make_integer(5)));
    const JsValue *v1 = fiber::json::gc_array_get(arr, 1);
    ASSERT_NE(v1, nullptr);
    EXPECT_EQ(v1->type_, JsNodeType::Integer);
    EXPECT_EQ(v1->i, 5);

    EXPECT_TRUE(fiber::json::gc_array_set(&heap, arr, 3, JsValue::make_integer(7)));
    EXPECT_EQ(arr->size, 4u);
    const JsValue *v2 = fiber::json::gc_array_get(arr, 2);
    ASSERT_NE(v2, nullptr);
    EXPECT_EQ(v2->type_, JsNodeType::Undefined);

    JsValue popped;
    EXPECT_TRUE(fiber::json::gc_array_pop(arr, &popped));
    EXPECT_EQ(popped.type_, JsNodeType::Integer);
    EXPECT_EQ(popped.i, 7);
    EXPECT_EQ(arr->size, 3u);

    EXPECT_EQ(fiber::json::gc_array_get(arr, 9), nullptr);
}

TEST(ArrayTest, InsertRemove) {
    GcHeap heap;
    GcArray *arr = fiber::json::gc_new_array(&heap, 2);
    ASSERT_NE(arr, nullptr);

    EXPECT_TRUE(fiber::json::gc_array_push(&heap, arr, JsValue::make_integer(1)));
    EXPECT_TRUE(fiber::json::gc_array_push(&heap, arr, JsValue::make_integer(3)));
    EXPECT_TRUE(fiber::json::gc_array_insert(&heap, arr, 1, JsValue::make_integer(2)));

    const JsValue *v0 = fiber::json::gc_array_get(arr, 0);
    const JsValue *v1 = fiber::json::gc_array_get(arr, 1);
    const JsValue *v2 = fiber::json::gc_array_get(arr, 2);
    ASSERT_NE(v0, nullptr);
    ASSERT_NE(v1, nullptr);
    ASSERT_NE(v2, nullptr);
    EXPECT_EQ(v0->i, 1);
    EXPECT_EQ(v1->i, 2);
    EXPECT_EQ(v2->i, 3);

    JsValue removed;
    EXPECT_TRUE(fiber::json::gc_array_remove(arr, 1, &removed));
    EXPECT_EQ(removed.type_, JsNodeType::Integer);
    EXPECT_EQ(removed.i, 2);
    EXPECT_EQ(arr->size, 2u);

    EXPECT_TRUE(fiber::json::gc_array_insert(&heap, arr, 10, JsValue::make_integer(4)));
    EXPECT_EQ(arr->size, 3u);
    const JsValue *v3 = fiber::json::gc_array_get(arr, 2);
    ASSERT_NE(v3, nullptr);
    EXPECT_EQ(v3->i, 4);

    EXPECT_FALSE(fiber::json::gc_array_remove(arr, 9, nullptr));
}
