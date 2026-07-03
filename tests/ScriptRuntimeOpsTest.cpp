#include <gtest/gtest.h>

#include "script/Runtime.h"
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

fiber::script::ValueHandle handle(fiber::script::ScriptRuntime &runtime, JsValue value) {
    fiber::script::ValueHandle out = runtime.global_value();
    EXPECT_NE(out, nullptr);
    if (out) {
        *out = value;
    }
    return out;
}

JsValue make_array(GcHeap &heap, std::initializer_list<JsValue> values) {
    JsValue arr = JsValue::make_array(heap, values.size());
    auto *arr_ptr = js_value_heap_ptr<GcArray>(arr);
    for (const auto &value: values) {
        if (!fiber::json::gc_array_push(&heap, arr_ptr, value)) {
            ADD_FAILURE() << "gc_array_push failed";
            break;
        }
    }
    return arr;
}

JsValue make_object_with_key(GcHeap &heap, const char *key, std::size_t key_len, const JsValue &value) {
    JsValue obj = JsValue::make_object(heap, 1);
    auto *obj_ptr = js_value_heap_ptr<GcObject>(obj);
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
    fiber::script::ScriptRuntime runtime(heap);
    auto lhs = handle(runtime, JsValue::make_object(heap, 0));
    auto rhs = handle(runtime, JsValue::make_integer(1));
    auto out = handle(runtime, JsValue::make_undefined());
    auto status = fiber::script::run::Binaries::plus(runtime, out, lhs, rhs);
    ASSERT_FALSE(status.has_value());
    ASSERT_TRUE(status.is_abort());
    EXPECT_EQ(status.abort().reason, fiber::script::ScriptAbortReason::TypeError);
}

TEST(ScriptRuntimeOpsTest, BinaryDivideByZero) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    auto lhs = handle(runtime, JsValue::make_integer(5));
    auto rhs = handle(runtime, JsValue::make_integer(0));
    auto out = handle(runtime, JsValue::make_undefined());
    auto status = fiber::script::run::Binaries::divide(runtime, out, lhs, rhs);
    ASSERT_FALSE(status.has_value());
    ASSERT_TRUE(status.is_abort());
    EXPECT_EQ(status.abort().reason, fiber::script::ScriptAbortReason::DivisionByZero);
}

TEST(ScriptRuntimeOpsTest, UnaryPlusTypeError) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    char data[] = {'a'};
    auto value = handle(runtime, JsValue::make_native_string(data, sizeof(data)));
    auto out = handle(runtime, JsValue::make_undefined());
    auto status = fiber::script::run::Unaries::plus(out, value);
    ASSERT_FALSE(status.has_value());
    ASSERT_TRUE(status.is_abort());
    EXPECT_EQ(status.abort().reason, fiber::script::ScriptAbortReason::TypeError);
}

TEST(ScriptRuntimeOpsTest, AccessIndexSetInvalidKey) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    auto arr = handle(runtime, JsValue::make_array(heap, 0));
    auto *arr_ptr = js_value_heap_ptr<GcArray>(*arr);
    ASSERT_TRUE(fiber::json::gc_array_push(&heap, arr_ptr, JsValue::make_integer(1)));
    char key_bytes[] = {'a'};
    auto key = handle(runtime, JsValue::make_native_string(key_bytes, sizeof(key_bytes)));
    auto value = handle(runtime, JsValue::make_integer(2));
    fiber::script::ResultPayload result;
    auto status = fiber::script::run::Access::index_set(runtime, arr, key, value, result);
    ASSERT_EQ(status, fiber::script::CallResult::Abort);
    EXPECT_EQ(result.abort.reason, fiber::script::ScriptAbortReason::IndexError);
}

TEST(ScriptRuntimeOpsTest, AccessIndexSetOutOfBounds) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    auto arr = handle(runtime, JsValue::make_array(heap, 0));
    auto *arr_ptr = js_value_heap_ptr<GcArray>(*arr);
    ASSERT_TRUE(fiber::json::gc_array_push(&heap, arr_ptr, JsValue::make_integer(1)));
    auto key = handle(runtime, JsValue::make_integer(3));
    auto value = handle(runtime, JsValue::make_integer(2));
    fiber::script::ResultPayload result;
    auto status = fiber::script::run::Access::index_set(runtime, arr, key, value, result);
    ASSERT_EQ(status, fiber::script::CallResult::Abort);
    EXPECT_EQ(result.abort.reason, fiber::script::ScriptAbortReason::IndexError);
}

TEST(ScriptRuntimeOpsTest, AccessPropSetNonObject) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    auto parent = handle(runtime, JsValue::make_integer(1));
    auto value = handle(runtime, JsValue::make_integer(2));
    char key_bytes[] = {'a'};
    auto key = handle(runtime, JsValue::make_native_string(key_bytes, sizeof(key_bytes)));
    fiber::script::ResultPayload result;
    auto status = fiber::script::run::Access::prop_set(runtime, parent, value, key, result);
    ASSERT_EQ(status, fiber::script::CallResult::Abort);
    EXPECT_EQ(result.abort.reason, fiber::script::ScriptAbortReason::IndexError);
}

TEST(ScriptRuntimeOpsTest, InSemanticsArray) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    auto arr = handle(runtime, make_array(heap, {JsValue::make_integer(1), JsValue::make_integer(2)}));
    auto key = handle(runtime, JsValue::make_integer(1));
    auto out = handle(runtime, JsValue::make_undefined());
    auto hit = fiber::script::run::Binaries::in(runtime, out, key, arr);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(js_value_type(*out), JsNodeType::Boolean);
    EXPECT_TRUE(js_value_bool(*out));

    *key = JsValue::make_integer(2);
    auto miss = fiber::script::run::Binaries::in(runtime, out, key, arr);
    ASSERT_TRUE(miss.has_value());
    EXPECT_FALSE(js_value_bool(*out));
}

TEST(ScriptRuntimeOpsTest, InSemanticsObject) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    auto obj = handle(runtime, make_object_with_key(heap, "a", 1, JsValue::make_integer(1)));
    auto heap_key = handle(runtime, JsValue::make_string(heap, "a", 1));
    auto out = handle(runtime, JsValue::make_undefined());
    auto heap_hit = fiber::script::run::Binaries::in(runtime, out, heap_key, obj);
    ASSERT_TRUE(heap_hit.has_value());
    EXPECT_TRUE(js_value_bool(*out));

    char key_bytes[] = {'a'};
    auto native_key = handle(runtime, JsValue::make_native_string(key_bytes, sizeof(key_bytes)));
    auto native_hit = fiber::script::run::Binaries::in(runtime, out, native_key, obj);
    ASSERT_TRUE(native_hit.has_value());
    EXPECT_TRUE(js_value_bool(*out));

    auto missing_key = handle(runtime, JsValue::make_string(heap, "b", 1));
    auto miss = fiber::script::run::Binaries::in(runtime, out, missing_key, obj);
    ASSERT_TRUE(miss.has_value());
    EXPECT_FALSE(js_value_bool(*out));
}
