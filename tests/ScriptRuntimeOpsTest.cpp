#include <gtest/gtest.h>

#include "script/run/Access.h"
#include "script/run/Binaries.h"
#include "script/run/Compares.h"
#include "script/run/Unaries.h"

using fiber::json::GcArray;
using fiber::json::GcHeap;
using fiber::json::GcObject;
using fiber::json::GcString;
using fiber::json::JsNodeType;
using fiber::json::JsValue;

namespace {

JsValue make_array(GcHeap &heap, std::initializer_list<JsValue> values) {
    JsValue arr = JsValue::make_array(heap, values.size());
    auto *arr_ptr = reinterpret_cast<GcArray *>(arr.gc);
    for (const auto &value : values) {
        if (!fiber::json::gc_array_push(&heap, arr_ptr, value)) {
            ADD_FAILURE() << "gc_array_push failed";
            break;
        }
    }
    return arr;
}

JsValue make_object_with_key(GcHeap &heap, const char *key, std::size_t key_len, const JsValue &value) {
    JsValue obj = JsValue::make_object(heap, 1);
    auto *obj_ptr = reinterpret_cast<GcObject *>(obj.gc);
    GcString *key_str = fiber::json::gc_new_string(&heap, key, key_len);
    if (!key_str) {
        ADD_FAILURE() << "gc_new_string failed";
        return obj;
    }
    if (!fiber::json::gc_object_set(&heap, obj_ptr, key_str, value)) {
        ADD_FAILURE() << "gc_object_set failed";
    }
    return obj;
}

} // namespace

TEST(ScriptRuntimeOpsTest, BinaryPlusTypeError) {
    GcHeap heap;
    JsValue lhs = JsValue::make_string(heap, "hi", 2);
    JsValue rhs = JsValue::make_integer(1);
    auto result = fiber::script::run::Binaries::plus(lhs, rhs, &heap);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().name, "EXEC_TYPE_ERROR");
}

TEST(ScriptRuntimeOpsTest, BinaryDivideByZero) {
    JsValue lhs = JsValue::make_integer(5);
    JsValue rhs = JsValue::make_integer(0);
    auto result = fiber::script::run::Binaries::divide(lhs, rhs, nullptr);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().name, "EXEC_DIVISION_BY_ZERO");
}

TEST(ScriptRuntimeOpsTest, UnaryPlusTypeError) {
    char data[] = {'a'};
    JsValue value = JsValue::make_native_string(data, sizeof(data));
    auto result = fiber::script::run::Unaries::plus(value);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().name, "EXEC_TYPE_ERROR");
}

TEST(ScriptRuntimeOpsTest, AccessIndexSetInvalidKey) {
    GcHeap heap;
    JsValue arr = JsValue::make_array(heap, 0);
    auto *arr_ptr = reinterpret_cast<GcArray *>(arr.gc);
    ASSERT_TRUE(fiber::json::gc_array_push(&heap, arr_ptr, JsValue::make_integer(1)));
    char key_bytes[] = {'a'};
    JsValue key = JsValue::make_native_string(key_bytes, sizeof(key_bytes));
    auto result = fiber::script::run::Access::index_set(arr, key, JsValue::make_integer(2), &heap);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().name, "EXEC_INDEX_ERROR");
}

TEST(ScriptRuntimeOpsTest, AccessIndexSetOutOfBounds) {
    GcHeap heap;
    JsValue arr = JsValue::make_array(heap, 0);
    auto *arr_ptr = reinterpret_cast<GcArray *>(arr.gc);
    ASSERT_TRUE(fiber::json::gc_array_push(&heap, arr_ptr, JsValue::make_integer(1)));
    JsValue key = JsValue::make_integer(3);
    auto result = fiber::script::run::Access::index_set(arr, key, JsValue::make_integer(2), &heap);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().name, "EXEC_INDEX_ERROR");
}

TEST(ScriptRuntimeOpsTest, AccessPropSetNonObject) {
    GcHeap heap;
    JsValue parent = JsValue::make_integer(1);
    char key_bytes[] = {'a'};
    JsValue key = JsValue::make_native_string(key_bytes, sizeof(key_bytes));
    auto result = fiber::script::run::Access::prop_set(parent, JsValue::make_integer(2), key, &heap);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().name, "EXEC_INDEX_ERROR");
}

TEST(ScriptRuntimeOpsTest, InSemanticsArray) {
    GcHeap heap;
    JsValue arr = make_array(heap, {JsValue::make_integer(1), JsValue::make_integer(2)});
    auto hit = fiber::script::run::Binaries::in(JsValue::make_integer(1), arr, &heap);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit.value().type_, JsNodeType::Boolean);
    EXPECT_TRUE(hit.value().b);

    auto miss = fiber::script::run::Binaries::in(JsValue::make_integer(2), arr, &heap);
    ASSERT_TRUE(miss.has_value());
    EXPECT_FALSE(miss.value().b);
}

TEST(ScriptRuntimeOpsTest, InSemanticsObject) {
    GcHeap heap;
    JsValue obj = make_object_with_key(heap, "a", 1, JsValue::make_integer(1));
    JsValue heap_key = JsValue::make_string(heap, "a", 1);
    auto heap_hit = fiber::script::run::Binaries::in(heap_key, obj, &heap);
    ASSERT_TRUE(heap_hit.has_value());
    EXPECT_TRUE(heap_hit.value().b);

    char key_bytes[] = {'a'};
    JsValue native_key = JsValue::make_native_string(key_bytes, sizeof(key_bytes));
    auto native_hit = fiber::script::run::Binaries::in(native_key, obj, &heap);
    ASSERT_TRUE(native_hit.has_value());
    EXPECT_TRUE(native_hit.value().b);

    auto miss = fiber::script::run::Binaries::in(JsValue::make_string(heap, "b", 1), obj, &heap);
    ASSERT_TRUE(miss.has_value());
    EXPECT_FALSE(miss.value().b);
}
