#include <gtest/gtest.h>

#include <limits>
#include <type_traits>

#include "script/JsGc.h"
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

fiber::script::ValueHandle handle(fiber::script::GcHeap &heap, JsValue value) {
    fiber::script::ValueHandle out = heap.global_value();
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
    char left_bytes[] = {static_cast<char>(0xC3), static_cast<char>(0xA9)};
    char right_bytes[] = {static_cast<char>(0xC3), static_cast<char>(0x9F)};
    auto lhs = handle(heap, JsValue::make_native_string(left_bytes, sizeof(left_bytes)));
    auto rhs = handle(heap, JsValue::make_native_string(right_bytes, sizeof(right_bytes)));
    ResultPayload result;
    auto status = fiber::script::run::Binaries::plus(heap, lhs, rhs, result);
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
    char euro_bytes[] = {static_cast<char>(0xE2), static_cast<char>(0x82), static_cast<char>(0xAC)};
    char ascii_bytes[] = {'A'};
    auto lhs = handle(heap, JsValue::make_native_string(euro_bytes, sizeof(euro_bytes)));
    auto rhs = handle(heap, JsValue::make_native_string(ascii_bytes, sizeof(ascii_bytes)));
    ResultPayload result;
    auto status = fiber::script::run::Binaries::plus(heap, lhs, rhs, result);
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
    auto lhs = handle(heap, JsValue::make_string(heap, "hi", 2));
    char right_bytes[] = {'!', '!'};
    auto rhs = handle(heap, JsValue::make_native_string(right_bytes, sizeof(right_bytes)));
    ResultPayload result;
    auto status = fiber::script::run::Binaries::plus(heap, lhs, rhs, result);
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
    auto lhs = handle(heap, JsValue::make_integer(3));
    auto rhs = handle(heap, JsValue::make_integer(4));
    ResultPayload result;
    auto status = fiber::script::run::Binaries::plus(heap, lhs, rhs, result);
    ASSERT_EQ(status, CallResult::Success);
    EXPECT_EQ(js_value_type(result.value), JsNodeType::Integer);
    EXPECT_EQ(js_value_int64(result.value), 7);
}

TEST(ScriptValueOpsTest, AddStringAndNumberConcats) {
    GcHeap heap;
    auto lhs = handle(heap, JsValue::make_string(heap, "hi", 2));
    auto rhs = handle(heap, JsValue::make_integer(1));
    ResultPayload result;
    auto status = fiber::script::run::Binaries::plus(heap, lhs, rhs, result);
    ASSERT_EQ(status, CallResult::Success);
    ASSERT_EQ(js_value_type(result.value), JsNodeType::String);
    EXPECT_EQ(string_to_utf8(result.value), "hi1");
}

TEST(ScriptValueOpsTest, AddPrimitiveAndStringConcats) {
    GcHeap heap;
    auto lhs = handle(heap, JsValue::make_boolean(false));
    auto rhs = handle(heap, JsValue::make_string(heap, " value", 6));
    ResultPayload result;
    auto status = fiber::script::run::Binaries::plus(heap, lhs, rhs, result);
    ASSERT_EQ(status, CallResult::Success);
    ASSERT_EQ(js_value_type(result.value), JsNodeType::String);
    EXPECT_EQ(string_to_utf8(result.value), "false value");
}

TEST(ScriptValueOpsTest, UnaryLogicalNot) {
    GcHeap heap;
    auto value = handle(heap, JsValue::make_integer(0));
    ResultPayload result;
    auto status = fiber::script::run::Unaries::neg(heap, value, result);
    ASSERT_EQ(status, CallResult::Success);
    EXPECT_EQ(js_value_type(result.value), JsNodeType::Boolean);
    EXPECT_TRUE(js_value_bool(result.value));
}

TEST(ScriptValueOpsTest, LooseAndStrictEquality) {
    GcHeap heap;
    char one_bytes[] = {'1'};
    auto str = handle(heap, JsValue::make_native_string(one_bytes, sizeof(one_bytes)));
    auto num = handle(heap, JsValue::make_integer(1));
    EXPECT_TRUE(fiber::script::run::Compares::eq(str, num));
    EXPECT_FALSE(fiber::script::run::Compares::seq(str, num));

    auto null_value = handle(heap, JsValue::make_null());
    auto undef_value = handle(heap, JsValue::make_undefined());
    EXPECT_TRUE(fiber::script::run::Compares::eq(null_value, undef_value));
    EXPECT_FALSE(fiber::script::run::Compares::seq(null_value, undef_value));
}

TEST(ScriptValueOpsTest, LooseEqualityParsesNumericStringsWithoutAllocation) {
    static_assert(noexcept(fiber::script::run::Compares::eq(nullptr, nullptr)));

    GcHeap heap;
    char decimal_bytes[] = {' ', '+', '1', '.', '2', '5', 'e', '2', ' '};
    auto decimal = handle(heap, JsValue::make_native_string(decimal_bytes, sizeof(decimal_bytes)));
    auto number = handle(heap, JsValue::make_integer(125));
    EXPECT_TRUE(fiber::script::run::Compares::eq(decimal, number));
    EXPECT_FALSE(fiber::script::run::Compares::seq(decimal, number));

    char infinity_bytes[] = {'I', 'n', 'f', 'i', 'n', 'i', 't', 'y'};
    auto infinity = handle(heap, JsValue::make_native_string(infinity_bytes, sizeof(infinity_bytes)));
    auto inf_number = handle(heap, JsValue::make_float(std::numeric_limits<double>::infinity()));
    EXPECT_TRUE(fiber::script::run::Compares::eq(infinity, inf_number));

    char invalid_bytes[] = {'1', 'e'};
    auto invalid = handle(heap, JsValue::make_native_string(invalid_bytes, sizeof(invalid_bytes)));
    EXPECT_FALSE(fiber::script::run::Compares::eq(invalid, number));

    char hex_bytes[] = {'0', 'x', '1', '0'};
    auto hex = handle(heap, JsValue::make_native_string(hex_bytes, sizeof(hex_bytes)));
    auto sixteen = handle(heap, JsValue::make_integer(16));
    EXPECT_TRUE(fiber::script::run::Compares::eq(hex, sixteen));

    char trailing_bytes[] = {'I', 'n', 'f', 'i', 'n', 'i', 't', 'y', 'x'};
    auto trailing = handle(heap, JsValue::make_native_string(trailing_bytes, sizeof(trailing_bytes)));
    EXPECT_FALSE(fiber::script::run::Compares::eq(trailing, inf_number));
}

TEST(ScriptValueOpsTest, CompareHeapByteStrings) {
    GcHeap heap;
    auto lhs = handle(heap, JsValue::make_string(heap, "ab", 2));
    auto rhs = handle(heap, JsValue::make_string(heap, "aba", 3));

    EXPECT_TRUE(fiber::script::run::Compares::lt(lhs, rhs));
    EXPECT_FALSE(fiber::script::run::Compares::gt(lhs, rhs));
}

TEST(ScriptValueOpsTest, CompareHeapUtf16Strings) {
    GcHeap heap;
    char omega_bytes[] = {static_cast<char>(0xCE), static_cast<char>(0xA9)};
    char euro_bytes[] = {static_cast<char>(0xE2), static_cast<char>(0x82), static_cast<char>(0xAC)};
    auto omega = handle(heap, JsValue::make_string(heap, omega_bytes, sizeof(omega_bytes)));
    auto euro = handle(heap, JsValue::make_string(heap, euro_bytes, sizeof(euro_bytes)));

    EXPECT_TRUE(fiber::script::run::Compares::lt(omega, euro));
    EXPECT_TRUE(fiber::script::run::Compares::eq(euro, euro));
}

TEST(ScriptValueOpsTest, CompareHeapByteAndHeapUtf16) {
    GcHeap heap;
    auto ascii = handle(heap, JsValue::make_string(heap, "A", 1));
    char euro_bytes[] = {static_cast<char>(0xE2), static_cast<char>(0x82), static_cast<char>(0xAC)};
    auto euro = handle(heap, JsValue::make_string(heap, euro_bytes, sizeof(euro_bytes)));

    EXPECT_TRUE(fiber::script::run::Compares::lt(ascii, euro));
}

TEST(ScriptValueOpsTest, CompareHeapAndNativeByte) {
    GcHeap heap;
    char cafe_bytes[] = {'c', 'a', 'f', static_cast<char>(0xC3), static_cast<char>(0xA9)};
    auto heap_value = handle(heap, JsValue::make_string(heap, cafe_bytes, sizeof(cafe_bytes)));
    auto native_value = handle(heap, JsValue::make_native_string(cafe_bytes, sizeof(cafe_bytes)));

    EXPECT_TRUE(fiber::script::run::Compares::seq(heap_value, native_value));
}

TEST(ScriptValueOpsTest, CompareNativeWithSurrogatePair) {
    GcHeap heap;
    char smile_bytes[] = {static_cast<char>(0xF0), static_cast<char>(0x9F), static_cast<char>(0x98),
                          static_cast<char>(0x80)};
    auto heap_smile = handle(heap, JsValue::make_string(heap, smile_bytes, sizeof(smile_bytes)));
    auto native_smile = handle(heap, JsValue::make_native_string(smile_bytes, sizeof(smile_bytes)));

    EXPECT_TRUE(fiber::script::run::Compares::eq(heap_smile, native_smile));

    auto bang = handle(heap, JsValue::make_string(heap, "!", 1));
    EXPECT_TRUE(fiber::script::run::Compares::gt(heap_smile, bang));
}

TEST(ScriptValueOpsTest, CompareInvalidUtf8) {
    GcHeap heap;
    char bad_bytes[] = {static_cast<char>(0xC3), static_cast<char>(0x28)};
    auto bad = handle(heap, JsValue::make_native_string(bad_bytes, sizeof(bad_bytes)));
    auto good = handle(heap, JsValue::make_native_string(bad_bytes + 1, 1));

    // Malformed UTF-8 folds to false (no abort) for equality and relations.
    EXPECT_FALSE(fiber::script::run::Compares::eq(bad, good));
    EXPECT_FALSE(fiber::script::run::Compares::lt(bad, good));
    EXPECT_FALSE(fiber::script::run::Compares::gt(bad, good));
}
