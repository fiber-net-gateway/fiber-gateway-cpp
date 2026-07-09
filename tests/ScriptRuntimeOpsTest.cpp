#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "script/ScriptResult.h"
#include "script/gc/GcInternal.h"
#include "script/run/Access.h"
#include "script/run/Binaries.h"
#include "script/run/Compares.h"
#include "script/run/Unaries.h"

using fiber::script::CallResult;
using fiber::script::GcArray;
using fiber::script::GcHeap;
using fiber::script::GcObject;
using fiber::script::GcString;
using fiber::script::JsNodeType;
using fiber::script::JsValue;
using fiber::script::ResultPayload;

namespace {

fiber::script::ValueHandle handle(fiber::script::GcHeap &heap, JsValue value) {
    fiber::script::ValueHandle out = heap.global_value();
    EXPECT_NE(out, nullptr);
    if (out) {
        *out = value;
    }
    return out;
}

JsValue make_array(GcHeap &heap, std::initializer_list<JsValue> values) {
    JsValue arr = JsValue::make_array(heap, values.size());
    auto *arr_ptr = js_value_heap_ptr<GcArray>(arr);
    GcHeap::NoGcScope no_gc(heap);
    for (const auto &value: values) {
        if (!fiber::script::gc_array_push(&heap, arr_ptr, value)) {
            ADD_FAILURE() << "gc_array_push failed";
            break;
        }
    }
    return arr;
}

JsValue make_object_with_key(GcHeap &heap, const char *key, std::size_t key_len, const JsValue &value) {
    JsValue obj = JsValue::make_object(heap, 1);
    auto *obj_ptr = js_value_heap_ptr<GcObject>(obj);
    GcHeap::NoGcScope no_gc(heap);
    GcString *key_str = fiber::script::gc_new_string(&heap, key, key_len);
    if (!key_str) {
        ADD_FAILURE() << "gc_new_string failed";
        return obj;
    }
    if (!fiber::script::gc_object_set(&heap, obj_ptr, key_str, value)) {
        ADD_FAILURE() << "gc_object_set failed";
    }
    return obj;
}

JsValue make_object(GcHeap &heap, std::initializer_list<std::pair<std::string_view, JsValue>> entries) {
    JsValue obj = JsValue::make_object(heap, entries.size());
    auto *obj_ptr = js_value_heap_ptr<GcObject>(obj);
    GcHeap::NoGcScope no_gc(heap);
    for (const auto &entry: entries) {
        GcString *key_str = fiber::script::gc_new_string(&heap, entry.first.data(), entry.first.size());
        if (!key_str || !fiber::script::gc_object_set(&heap, obj_ptr, key_str, entry.second)) {
            ADD_FAILURE() << "make_object set failed";
        }
    }
    return obj;
}

} // namespace

TEST(ScriptRuntimeOpsTest, BinaryPlusTypeError) {
    GcHeap heap;
    auto lhs = handle(heap, JsValue::make_object(heap, 0));
    auto rhs = handle(heap, JsValue::make_integer(1));
    ResultPayload result;
    auto status = fiber::script::run::Binaries::plus(heap, lhs, rhs, result);
    ASSERT_EQ(status, CallResult::Exception);
    EXPECT_EQ(fiber::script::js_value_exception_kind(result.exception), fiber::script::ExceptionKind::TypeError);
}

TEST(ScriptRuntimeOpsTest, BinaryDivideByZero) {
    GcHeap heap;
    auto lhs = handle(heap, JsValue::make_integer(5));
    auto rhs = handle(heap, JsValue::make_integer(0));
    ResultPayload result;
    auto status = fiber::script::run::Binaries::divide(heap, lhs, rhs, result);
    ASSERT_EQ(status, CallResult::Exception);
    EXPECT_EQ(fiber::script::js_value_exception_kind(result.exception), fiber::script::ExceptionKind::RangeError);
}

TEST(ScriptRuntimeOpsTest, UnaryPlusTypeError) {
    GcHeap heap;
    char data[] = {'a'};
    auto value = handle(heap, JsValue::make_native_string(data, sizeof(data)));
    ResultPayload result;
    auto status = fiber::script::run::Unaries::plus(heap, value, result);
    ASSERT_EQ(status, CallResult::Exception);
    EXPECT_EQ(fiber::script::js_value_exception_kind(result.exception), fiber::script::ExceptionKind::TypeError);
}

TEST(ScriptRuntimeOpsTest, AccessIndexSetInvalidKey) {
    GcHeap heap;
    auto arr = handle(heap, JsValue::make_array(heap, 0));
    auto *arr_ptr = js_value_heap_ptr<GcArray>(*arr);
    {
        GcHeap::NoGcScope no_gc(heap);
        ASSERT_TRUE(fiber::script::gc_array_push(&heap, arr_ptr, JsValue::make_integer(1)));
    }
    char key_bytes[] = {'a'};
    auto key = handle(heap, JsValue::make_native_string(key_bytes, sizeof(key_bytes)));
    auto value = handle(heap, JsValue::make_integer(2));
    ResultPayload result;
    auto status = fiber::script::run::Access::index_set(heap, arr, key, value, result);
    ASSERT_EQ(status, CallResult::Exception);
    EXPECT_EQ(fiber::script::js_value_exception_kind(result.exception), fiber::script::ExceptionKind::TypeError);
}

TEST(ScriptRuntimeOpsTest, AccessIndexSetOutOfBounds) {
    GcHeap heap;
    auto arr = handle(heap, JsValue::make_array(heap, 0));
    auto *arr_ptr = js_value_heap_ptr<GcArray>(*arr);
    {
        GcHeap::NoGcScope no_gc(heap);
        ASSERT_TRUE(fiber::script::gc_array_push(&heap, arr_ptr, JsValue::make_integer(1)));
    }
    auto key = handle(heap, JsValue::make_integer(3));
    auto value = handle(heap, JsValue::make_integer(2));
    ResultPayload result;
    auto status = fiber::script::run::Access::index_set(heap, arr, key, value, result);
    ASSERT_EQ(status, CallResult::Exception);
    EXPECT_EQ(fiber::script::js_value_exception_kind(result.exception), fiber::script::ExceptionKind::RangeError);
}

TEST(ScriptRuntimeOpsTest, AccessPropSetNonObject) {
    GcHeap heap;
    auto parent = handle(heap, JsValue::make_integer(1));
    auto value = handle(heap, JsValue::make_integer(2));
    char key_bytes[] = {'a'};
    auto key = handle(heap, JsValue::make_native_string(key_bytes, sizeof(key_bytes)));
    ResultPayload result;
    auto status = fiber::script::run::Access::prop_set(heap, parent, value, key, result);
    ASSERT_EQ(status, CallResult::Exception);
    EXPECT_EQ(fiber::script::js_value_exception_kind(result.exception), fiber::script::ExceptionKind::TypeError);
}

TEST(ScriptRuntimeOpsTest, InSemanticsArray) {
    GcHeap heap;
    auto arr = handle(heap, make_array(heap, {JsValue::make_integer(1), JsValue::make_integer(2)}));
    auto key = handle(heap, JsValue::make_integer(1));
    ResultPayload result;
    auto hit = fiber::script::run::Binaries::in(heap, key, arr, result);
    ASSERT_EQ(hit, CallResult::Success);
    EXPECT_EQ(js_value_type(result.value), JsNodeType::Boolean);
    EXPECT_TRUE(js_value_bool(result.value));

    *key = JsValue::make_integer(2);
    auto miss = fiber::script::run::Binaries::in(heap, key, arr, result);
    ASSERT_EQ(miss, CallResult::Success);
    EXPECT_FALSE(js_value_bool(result.value));
}

TEST(ScriptRuntimeOpsTest, InSemanticsObject) {
    GcHeap heap;
    auto obj = handle(heap, make_object_with_key(heap, "a", 1, JsValue::make_integer(1)));
    auto heap_key = handle(heap, JsValue::make_string(heap, "a", 1));
    ResultPayload result;
    auto heap_hit = fiber::script::run::Binaries::in(heap, heap_key, obj, result);
    ASSERT_EQ(heap_hit, CallResult::Success);
    EXPECT_TRUE(js_value_bool(result.value));

    char key_bytes[] = {'a'};
    auto native_key = handle(heap, JsValue::make_native_string(key_bytes, sizeof(key_bytes)));
    auto native_hit = fiber::script::run::Binaries::in(heap, native_key, obj, result);
    ASSERT_EQ(native_hit, CallResult::Success);
    EXPECT_TRUE(js_value_bool(result.value));

    auto missing_key = handle(heap, JsValue::make_string(heap, "b", 1));
    auto miss = fiber::script::run::Binaries::in(heap, missing_key, obj, result);
    ASSERT_EQ(miss, CallResult::Success);
    EXPECT_FALSE(js_value_bool(result.value));
}

TEST(ScriptRuntimeOpsTest, InSemanticsObjectMatchesBorrowedUtf8KeysWithoutAllocation) {
    GcHeap heap;
    char latin1_bytes[] = {static_cast<char>(0xC3), static_cast<char>(0xA9)};
    auto latin_obj =
            handle(heap, make_object_with_key(heap, latin1_bytes, sizeof(latin1_bytes), JsValue::make_integer(1)));
    auto latin_key = handle(heap, JsValue::make_native_string(latin1_bytes, sizeof(latin1_bytes)));
    ResultPayload result;
    auto latin_hit = fiber::script::run::Binaries::in(heap, latin_key, latin_obj, result);
    ASSERT_EQ(latin_hit, CallResult::Success);
    EXPECT_TRUE(js_value_bool(result.value));

    char emoji_bytes[] = {static_cast<char>(0xF0), static_cast<char>(0x9F), static_cast<char>(0x98),
                          static_cast<char>(0x80)};
    auto emoji_obj =
            handle(heap, make_object_with_key(heap, emoji_bytes, sizeof(emoji_bytes), JsValue::make_integer(2)));
    auto emoji_key = handle(heap, JsValue::make_native_string(emoji_bytes, sizeof(emoji_bytes)));
    auto emoji_hit = fiber::script::run::Binaries::in(heap, emoji_key, emoji_obj, result);
    ASSERT_EQ(emoji_hit, CallResult::Success);
    EXPECT_TRUE(js_value_bool(result.value));

    char invalid_bytes[] = {static_cast<char>(0xC3), static_cast<char>(0x28)};
    auto invalid_key = handle(heap, JsValue::make_native_string(invalid_bytes, sizeof(invalid_bytes)));
    auto invalid_miss = fiber::script::run::Binaries::in(heap, invalid_key, latin_obj, result);
    ASSERT_EQ(invalid_miss, CallResult::Success);
    EXPECT_FALSE(js_value_bool(result.value));
}

TEST(ScriptRuntimeOpsTest, ExpandObjectMergesAndOverwritesInOrder) {
    GcHeap heap;
    auto target = handle(heap, make_object(heap, {{"a", JsValue::make_integer(1)}, {"b", JsValue::make_integer(2)}}));
    auto addition = handle(heap, make_object(heap, {{"b", JsValue::make_integer(3)}, {"c", JsValue::make_integer(4)}}));
    ResultPayload result;
    auto status = fiber::script::run::Access::expand_object(heap, target, addition, result);
    ASSERT_EQ(status, CallResult::Success);
    ASSERT_EQ(js_value_type(result.value), JsNodeType::Object);
    const GcObject *obj = js_value_heap_ptr<const GcObject>(result.value);
    ASSERT_NE(obj, nullptr);

    // Insertion order: a, b (overwritten in place, keeps position), c appended.
    std::vector<std::string> keys;
    std::vector<std::int64_t> values;
    for (const fiber::script::GcObjectEntry *e = fiber::script::gc_object_first_entry(obj); e != nullptr;
         e = fiber::script::gc_object_next_entry(obj, e)) {
        std::string k;
        fiber::script::gc_string_to_utf8(e->key, k);
        keys.push_back(k);
        values.push_back(js_value_int64(e->value));
    }
    ASSERT_EQ(keys.size(), 3u);
    EXPECT_EQ(keys[0], "a");
    EXPECT_EQ(keys[1], "b");
    EXPECT_EQ(keys[2], "c");
    EXPECT_EQ(values[0], 1);
    EXPECT_EQ(values[1], 3); // overwritten by addition
    EXPECT_EQ(values[2], 4);
}

TEST(ScriptRuntimeOpsTest, ExpandArrayFromObjectPreservesInsertionOrder) {
    GcHeap heap;
    auto target = handle(heap, JsValue::make_array(heap, 0));
    auto addition =
            handle(heap, make_object(heap, {{"a", JsValue::make_integer(10)}, {"b", JsValue::make_integer(20)}}));
    ResultPayload result;
    auto status = fiber::script::run::Access::expand_array(heap, target, addition, result);
    ASSERT_EQ(status, CallResult::Success);
    ASSERT_EQ(js_value_type(result.value), JsNodeType::Array);
    const GcArray *arr = js_value_heap_ptr<const GcArray>(result.value);
    ASSERT_NE(arr, nullptr);
    ASSERT_EQ(arr->size, 2u);
    EXPECT_EQ(js_value_int64(*fiber::script::gc_array_get(arr, 0)), 10);
    EXPECT_EQ(js_value_int64(*fiber::script::gc_array_get(arr, 1)), 20);
}

TEST(ScriptRuntimeOpsTest, ExpandArrayFromArrayAppends) {
    GcHeap heap;
    auto target = handle(heap, make_array(heap, {JsValue::make_integer(1), JsValue::make_integer(2)}));
    auto addition = handle(heap, make_array(heap, {JsValue::make_integer(3), JsValue::make_integer(4)}));
    ResultPayload result;
    auto status = fiber::script::run::Access::expand_array(heap, target, addition, result);
    ASSERT_EQ(status, CallResult::Success);
    const GcArray *arr = js_value_heap_ptr<const GcArray>(result.value);
    ASSERT_NE(arr, nullptr);
    ASSERT_EQ(arr->size, 4u);
    EXPECT_EQ(js_value_int64(*fiber::script::gc_array_get(arr, 0)), 1);
    EXPECT_EQ(js_value_int64(*fiber::script::gc_array_get(arr, 1)), 2);
    EXPECT_EQ(js_value_int64(*fiber::script::gc_array_get(arr, 2)), 3);
    EXPECT_EQ(js_value_int64(*fiber::script::gc_array_get(arr, 3)), 4);
}
