#include <gtest/gtest.h>

#include <type_traits>

#include "script/Runtime.h"
#include "script/ScriptResult.h"
#include "script/run/Binaries.h"
#include "script/run/Compares.h"
#include "script/run/Unaries.h"

using fiber::script::CallResult;
using fiber::script::GcHeap;
using fiber::script::GcString;
using fiber::script::GcStringEncoding;
using fiber::script::JsNodeType;
using fiber::script::JsValue;
using fiber::script::ResultPayload;

namespace {

fiber::script::ValueHandle handle(fiber::script::ScriptRuntime &runtime, JsValue value) {
    fiber::script::ValueHandle out = runtime.global_value();
    EXPECT_NE(out, nullptr);
    if (out) {
        *out = value;
    }
    return out;
}

const GcString *as_string(const JsValue &value) { return js_value_heap_ptr<const GcString>(value); }

std::string string_to_utf8(const JsValue &value) {
    std::string out;
    auto *str = as_string(value);
    if (str) {
        EXPECT_TRUE(fiber::script::gc_string_to_utf8(str, out));
    }
    return out;
}

} // namespace

TEST(ScriptValueOpsTest, JsValueIsTriviallyCopyable) { EXPECT_TRUE(std::is_trivially_copyable_v<JsValue>); }

TEST(ScriptValueOpsTest, ConcatKeepsByteForNativeUtf8) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    char left_bytes[] = {static_cast<char>(0xC3), static_cast<char>(0xA9)};
    char right_bytes[] = {static_cast<char>(0xC3), static_cast<char>(0x9F)};
    auto lhs = handle(runtime, JsValue::make_native_string(left_bytes, sizeof(left_bytes)));
    auto rhs = handle(runtime, JsValue::make_native_string(right_bytes, sizeof(right_bytes)));
    ResultPayload result;
    auto status = fiber::script::run::Binaries::plus(runtime, lhs, rhs, result);
    ASSERT_EQ(status, CallResult::Success);
    ASSERT_EQ(js_value_type(result.value), JsNodeType::String);
    auto *str = as_string(result.value);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(str->encoding, GcStringEncoding::Byte);
    EXPECT_EQ(str->len, 2u);

    const char expected_bytes[] = {
            static_cast<char>(0xC3),
            static_cast<char>(0xA9),
            static_cast<char>(0xC3),
            static_cast<char>(0x9F),
    };
    std::string expected(expected_bytes, sizeof(expected_bytes));
    EXPECT_EQ(string_to_utf8(result.value), expected);
}

TEST(ScriptValueOpsTest, ConcatUpgradesToUtf16ForWide) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    char euro_bytes[] = {static_cast<char>(0xE2), static_cast<char>(0x82), static_cast<char>(0xAC)};
    char ascii_bytes[] = {'A'};
    auto lhs = handle(runtime, JsValue::make_native_string(euro_bytes, sizeof(euro_bytes)));
    auto rhs = handle(runtime, JsValue::make_native_string(ascii_bytes, sizeof(ascii_bytes)));
    ResultPayload result;
    auto status = fiber::script::run::Binaries::plus(runtime, lhs, rhs, result);
    ASSERT_EQ(status, CallResult::Success);
    ASSERT_EQ(js_value_type(result.value), JsNodeType::String);
    auto *str = as_string(result.value);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(str->encoding, GcStringEncoding::Utf16);
    EXPECT_EQ(str->len, 2u);

    const char expected_bytes[] = {
            static_cast<char>(0xE2),
            static_cast<char>(0x82),
            static_cast<char>(0xAC),
            'A',
    };
    std::string expected(expected_bytes, sizeof(expected_bytes));
    EXPECT_EQ(string_to_utf8(result.value), expected);
}

TEST(ScriptValueOpsTest, ConcatHeapAndNative) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    auto lhs = handle(runtime, JsValue::make_string(heap, "hi", 2));
    char right_bytes[] = {'!', '!'};
    auto rhs = handle(runtime, JsValue::make_native_string(right_bytes, sizeof(right_bytes)));
    ResultPayload result;
    auto status = fiber::script::run::Binaries::plus(runtime, lhs, rhs, result);
    ASSERT_EQ(status, CallResult::Success);
    ASSERT_EQ(js_value_type(result.value), JsNodeType::String);
    auto *str = as_string(result.value);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(str->encoding, GcStringEncoding::Byte);
    EXPECT_EQ(str->len, 4u);

    const char expected_bytes[] = {'h', 'i', '!', '!'};
    std::string expected(expected_bytes, sizeof(expected_bytes));
    EXPECT_EQ(string_to_utf8(result.value), expected);
}

TEST(ScriptValueOpsTest, AddInteger) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    auto lhs = handle(runtime, JsValue::make_integer(3));
    auto rhs = handle(runtime, JsValue::make_integer(4));
    ResultPayload result;
    auto status = fiber::script::run::Binaries::plus(runtime, lhs, rhs, result);
    ASSERT_EQ(status, CallResult::Success);
    EXPECT_EQ(js_value_type(result.value), JsNodeType::Integer);
    EXPECT_EQ(js_value_int64(result.value), 7);
}

TEST(ScriptValueOpsTest, AddStringAndNumberConcats) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    auto lhs = handle(runtime, JsValue::make_string(heap, "hi", 2));
    auto rhs = handle(runtime, JsValue::make_integer(1));
    ResultPayload result;
    auto status = fiber::script::run::Binaries::plus(runtime, lhs, rhs, result);
    ASSERT_EQ(status, CallResult::Success);
    ASSERT_EQ(js_value_type(result.value), JsNodeType::String);
    EXPECT_EQ(string_to_utf8(result.value), "hi1");
}

TEST(ScriptValueOpsTest, AddPrimitiveAndStringConcats) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    auto lhs = handle(runtime, JsValue::make_boolean(false));
    auto rhs = handle(runtime, JsValue::make_string(heap, " value", 6));
    ResultPayload result;
    auto status = fiber::script::run::Binaries::plus(runtime, lhs, rhs, result);
    ASSERT_EQ(status, CallResult::Success);
    ASSERT_EQ(js_value_type(result.value), JsNodeType::String);
    EXPECT_EQ(string_to_utf8(result.value), "false value");
}

TEST(ScriptValueOpsTest, UnaryLogicalNot) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    auto value = handle(runtime, JsValue::make_integer(0));
    ResultPayload result;
    auto status = fiber::script::run::Unaries::neg(runtime, value, result);
    ASSERT_EQ(status, CallResult::Success);
    EXPECT_EQ(js_value_type(result.value), JsNodeType::Boolean);
    EXPECT_TRUE(js_value_bool(result.value));
}

TEST(ScriptValueOpsTest, LooseAndStrictEquality) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    char one_bytes[] = {'1'};
    auto str = handle(runtime, JsValue::make_native_string(one_bytes, sizeof(one_bytes)));
    auto num = handle(runtime, JsValue::make_integer(1));
    EXPECT_TRUE(fiber::script::run::Compares::eq(str, num));
    EXPECT_FALSE(fiber::script::run::Compares::seq(str, num));

    auto null_value = handle(runtime, JsValue::make_null());
    auto undef_value = handle(runtime, JsValue::make_undefined());
    EXPECT_TRUE(fiber::script::run::Compares::eq(null_value, undef_value));
    EXPECT_FALSE(fiber::script::run::Compares::seq(null_value, undef_value));
}

TEST(ScriptValueOpsTest, CompareHeapByteStrings) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    auto lhs = handle(runtime, JsValue::make_string(heap, "ab", 2));
    auto rhs = handle(runtime, JsValue::make_string(heap, "aba", 3));

    EXPECT_TRUE(fiber::script::run::Compares::lt(lhs, rhs));
    EXPECT_FALSE(fiber::script::run::Compares::gt(lhs, rhs));
}

TEST(ScriptValueOpsTest, CompareHeapUtf16Strings) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    char omega_bytes[] = {static_cast<char>(0xCE), static_cast<char>(0xA9)};
    char euro_bytes[] = {static_cast<char>(0xE2), static_cast<char>(0x82), static_cast<char>(0xAC)};
    auto omega = handle(runtime, JsValue::make_string(heap, omega_bytes, sizeof(omega_bytes)));
    auto euro = handle(runtime, JsValue::make_string(heap, euro_bytes, sizeof(euro_bytes)));

    EXPECT_TRUE(fiber::script::run::Compares::lt(omega, euro));
    EXPECT_TRUE(fiber::script::run::Compares::eq(euro, euro));
}

TEST(ScriptValueOpsTest, CompareHeapByteAndHeapUtf16) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    auto ascii = handle(runtime, JsValue::make_string(heap, "A", 1));
    char euro_bytes[] = {static_cast<char>(0xE2), static_cast<char>(0x82), static_cast<char>(0xAC)};
    auto euro = handle(runtime, JsValue::make_string(heap, euro_bytes, sizeof(euro_bytes)));

    EXPECT_TRUE(fiber::script::run::Compares::lt(ascii, euro));
}

TEST(ScriptValueOpsTest, CompareHeapAndNativeByte) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    char cafe_bytes[] = {'c', 'a', 'f', static_cast<char>(0xC3), static_cast<char>(0xA9)};
    auto heap_value = handle(runtime, JsValue::make_string(heap, cafe_bytes, sizeof(cafe_bytes)));
    auto native_value = handle(runtime, JsValue::make_native_string(cafe_bytes, sizeof(cafe_bytes)));

    EXPECT_TRUE(fiber::script::run::Compares::seq(heap_value, native_value));
}

TEST(ScriptValueOpsTest, CompareNativeWithSurrogatePair) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    char smile_bytes[] = {static_cast<char>(0xF0), static_cast<char>(0x9F), static_cast<char>(0x98),
                          static_cast<char>(0x80)};
    auto heap_smile = handle(runtime, JsValue::make_string(heap, smile_bytes, sizeof(smile_bytes)));
    auto native_smile = handle(runtime, JsValue::make_native_string(smile_bytes, sizeof(smile_bytes)));

    EXPECT_TRUE(fiber::script::run::Compares::eq(heap_smile, native_smile));

    auto bang = handle(runtime, JsValue::make_string(heap, "!", 1));
    EXPECT_TRUE(fiber::script::run::Compares::gt(heap_smile, bang));
}

TEST(ScriptValueOpsTest, CompareInvalidUtf8) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    char bad_bytes[] = {static_cast<char>(0xC3), static_cast<char>(0x28)};
    auto bad = handle(runtime, JsValue::make_native_string(bad_bytes, sizeof(bad_bytes)));
    auto good = handle(runtime, JsValue::make_native_string(bad_bytes + 1, 1));

    // Malformed UTF-8 folds to false (no abort) for equality and relations.
    EXPECT_FALSE(fiber::script::run::Compares::eq(bad, good));
    EXPECT_FALSE(fiber::script::run::Compares::lt(bad, good));
    EXPECT_FALSE(fiber::script::run::Compares::gt(bad, good));
}
