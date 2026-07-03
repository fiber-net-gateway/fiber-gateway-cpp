#include <gtest/gtest.h>

#include <type_traits>

#include "script/Runtime.h"
#include "script/run/Binaries.h"
#include "script/run/Compares.h"
#include "script/run/Unaries.h"

using fiber::json::GcHeap;
using fiber::json::GcString;
using fiber::json::GcStringEncoding;
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

const GcString *as_string(const JsValue &value) { return js_value_heap_ptr<const GcString>(value); }

std::string string_to_utf8(const JsValue &value) {
    std::string out;
    auto *str = as_string(value);
    if (str) {
        EXPECT_TRUE(fiber::json::gc_string_to_utf8(str, out));
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
    auto out = handle(runtime, JsValue::make_undefined());

    auto status = fiber::script::run::Binaries::plus(runtime, out, lhs, rhs);
    ASSERT_TRUE(status.has_value());
    ASSERT_EQ(js_value_type(*out), JsNodeType::String);
    auto *str = as_string(*out);
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
    EXPECT_EQ(string_to_utf8(*out), expected);
}

TEST(ScriptValueOpsTest, ConcatUpgradesToUtf16ForWide) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    char euro_bytes[] = {static_cast<char>(0xE2), static_cast<char>(0x82), static_cast<char>(0xAC)};
    char ascii_bytes[] = {'A'};
    auto lhs = handle(runtime, JsValue::make_native_string(euro_bytes, sizeof(euro_bytes)));
    auto rhs = handle(runtime, JsValue::make_native_string(ascii_bytes, sizeof(ascii_bytes)));
    auto out = handle(runtime, JsValue::make_undefined());

    auto status = fiber::script::run::Binaries::plus(runtime, out, lhs, rhs);
    ASSERT_TRUE(status.has_value());
    ASSERT_EQ(js_value_type(*out), JsNodeType::String);
    auto *str = as_string(*out);
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
    EXPECT_EQ(string_to_utf8(*out), expected);
}

TEST(ScriptValueOpsTest, ConcatHeapAndNative) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    auto lhs = handle(runtime, JsValue::make_string(heap, "hi", 2));
    char right_bytes[] = {'!', '!'};
    auto rhs = handle(runtime, JsValue::make_native_string(right_bytes, sizeof(right_bytes)));
    auto out = handle(runtime, JsValue::make_undefined());

    auto status = fiber::script::run::Binaries::plus(runtime, out, lhs, rhs);
    ASSERT_TRUE(status.has_value());
    ASSERT_EQ(js_value_type(*out), JsNodeType::String);
    auto *str = as_string(*out);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(str->encoding, GcStringEncoding::Byte);
    EXPECT_EQ(str->len, 4u);

    const char expected_bytes[] = {'h', 'i', '!', '!'};
    std::string expected(expected_bytes, sizeof(expected_bytes));
    EXPECT_EQ(string_to_utf8(*out), expected);
}

TEST(ScriptValueOpsTest, AddInteger) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    auto lhs = handle(runtime, JsValue::make_integer(3));
    auto rhs = handle(runtime, JsValue::make_integer(4));
    auto out = handle(runtime, JsValue::make_undefined());

    auto status = fiber::script::run::Binaries::plus(runtime, out, lhs, rhs);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(js_value_type(*out), JsNodeType::Integer);
    EXPECT_EQ(js_value_int64(*out), 7);
}

TEST(ScriptValueOpsTest, AddStringAndNumberConcats) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    auto lhs = handle(runtime, JsValue::make_string(heap, "hi", 2));
    auto rhs = handle(runtime, JsValue::make_integer(1));
    auto out = handle(runtime, JsValue::make_undefined());

    auto status = fiber::script::run::Binaries::plus(runtime, out, lhs, rhs);
    ASSERT_TRUE(status.has_value());
    ASSERT_EQ(js_value_type(*out), JsNodeType::String);
    EXPECT_EQ(string_to_utf8(*out), "hi1");
}

TEST(ScriptValueOpsTest, AddPrimitiveAndStringConcats) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    auto lhs = handle(runtime, JsValue::make_boolean(false));
    auto rhs = handle(runtime, JsValue::make_string(heap, " value", 6));
    auto out = handle(runtime, JsValue::make_undefined());

    auto status = fiber::script::run::Binaries::plus(runtime, out, lhs, rhs);
    ASSERT_TRUE(status.has_value());
    ASSERT_EQ(js_value_type(*out), JsNodeType::String);
    EXPECT_EQ(string_to_utf8(*out), "false value");
}

TEST(ScriptValueOpsTest, UnaryLogicalNot) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    auto value = handle(runtime, JsValue::make_integer(0));
    auto out = handle(runtime, JsValue::make_undefined());

    auto status = fiber::script::run::Unaries::neg(out, value);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(js_value_type(*out), JsNodeType::Boolean);
    EXPECT_TRUE(js_value_bool(*out));
}

TEST(ScriptValueOpsTest, LooseAndStrictEquality) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    char one_bytes[] = {'1'};
    auto str = handle(runtime, JsValue::make_native_string(one_bytes, sizeof(one_bytes)));
    auto num = handle(runtime, JsValue::make_integer(1));
    auto out = handle(runtime, JsValue::make_undefined());

    auto loose = fiber::script::run::Compares::eq(out, str, num);
    ASSERT_TRUE(loose.has_value());
    EXPECT_EQ(js_value_type(*out), JsNodeType::Boolean);
    EXPECT_TRUE(js_value_bool(*out));

    auto strict = fiber::script::run::Compares::seq(out, str, num);
    ASSERT_TRUE(strict.has_value());
    EXPECT_EQ(js_value_type(*out), JsNodeType::Boolean);
    EXPECT_FALSE(js_value_bool(*out));

    auto null_value = handle(runtime, JsValue::make_null());
    auto undef_value = handle(runtime, JsValue::make_undefined());
    auto loose_null = fiber::script::run::Compares::eq(out, null_value, undef_value);
    ASSERT_TRUE(loose_null.has_value());
    EXPECT_TRUE(js_value_bool(*out));
    auto strict_null = fiber::script::run::Compares::seq(out, null_value, undef_value);
    ASSERT_TRUE(strict_null.has_value());
    EXPECT_FALSE(js_value_bool(*out));
}

TEST(ScriptValueOpsTest, CompareHeapByteStrings) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    auto lhs = handle(runtime, JsValue::make_string(heap, "ab", 2));
    auto rhs = handle(runtime, JsValue::make_string(heap, "aba", 3));
    auto out = handle(runtime, JsValue::make_undefined());

    auto lt = fiber::script::run::Compares::lt(out, lhs, rhs);
    ASSERT_TRUE(lt.has_value());
    EXPECT_TRUE(js_value_bool(*out));

    auto gt = fiber::script::run::Compares::gt(out, lhs, rhs);
    ASSERT_TRUE(gt.has_value());
    EXPECT_FALSE(js_value_bool(*out));
}

TEST(ScriptValueOpsTest, CompareHeapUtf16Strings) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    char omega_bytes[] = {static_cast<char>(0xCE), static_cast<char>(0xA9)};
    char euro_bytes[] = {static_cast<char>(0xE2), static_cast<char>(0x82), static_cast<char>(0xAC)};
    auto omega = handle(runtime, JsValue::make_string(heap, omega_bytes, sizeof(omega_bytes)));
    auto euro = handle(runtime, JsValue::make_string(heap, euro_bytes, sizeof(euro_bytes)));
    auto out = handle(runtime, JsValue::make_undefined());

    auto lt = fiber::script::run::Compares::lt(out, omega, euro);
    ASSERT_TRUE(lt.has_value());
    EXPECT_TRUE(js_value_bool(*out));

    auto eq = fiber::script::run::Compares::eq(out, euro, euro);
    ASSERT_TRUE(eq.has_value());
    EXPECT_TRUE(js_value_bool(*out));
}

TEST(ScriptValueOpsTest, CompareHeapByteAndHeapUtf16) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    auto ascii = handle(runtime, JsValue::make_string(heap, "A", 1));
    char euro_bytes[] = {static_cast<char>(0xE2), static_cast<char>(0x82), static_cast<char>(0xAC)};
    auto euro = handle(runtime, JsValue::make_string(heap, euro_bytes, sizeof(euro_bytes)));
    auto out = handle(runtime, JsValue::make_undefined());

    auto lt = fiber::script::run::Compares::lt(out, ascii, euro);
    ASSERT_TRUE(lt.has_value());
    EXPECT_TRUE(js_value_bool(*out));
}

TEST(ScriptValueOpsTest, CompareHeapAndNativeByte) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    char cafe_bytes[] = {'c', 'a', 'f', static_cast<char>(0xC3), static_cast<char>(0xA9)};
    auto heap_value = handle(runtime, JsValue::make_string(heap, cafe_bytes, sizeof(cafe_bytes)));
    auto native_value = handle(runtime, JsValue::make_native_string(cafe_bytes, sizeof(cafe_bytes)));
    auto out = handle(runtime, JsValue::make_undefined());

    auto eq = fiber::script::run::Compares::seq(out, heap_value, native_value);
    ASSERT_TRUE(eq.has_value());
    EXPECT_TRUE(js_value_bool(*out));
}

TEST(ScriptValueOpsTest, CompareNativeWithSurrogatePair) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    char smile_bytes[] = {static_cast<char>(0xF0), static_cast<char>(0x9F), static_cast<char>(0x98),
                          static_cast<char>(0x80)};
    auto heap_smile = handle(runtime, JsValue::make_string(heap, smile_bytes, sizeof(smile_bytes)));
    auto native_smile = handle(runtime, JsValue::make_native_string(smile_bytes, sizeof(smile_bytes)));
    auto out = handle(runtime, JsValue::make_undefined());

    auto eq = fiber::script::run::Compares::eq(out, heap_smile, native_smile);
    ASSERT_TRUE(eq.has_value());
    EXPECT_TRUE(js_value_bool(*out));

    auto bang = handle(runtime, JsValue::make_string(heap, "!", 1));
    auto gt = fiber::script::run::Compares::gt(out, heap_smile, bang);
    ASSERT_TRUE(gt.has_value());
    EXPECT_TRUE(js_value_bool(*out));
}

TEST(ScriptValueOpsTest, CompareInvalidUtf8) {
    GcHeap heap;
    fiber::script::ScriptRuntime runtime(heap);
    char bad_bytes[] = {static_cast<char>(0xC3), static_cast<char>(0x28)};
    auto bad = handle(runtime, JsValue::make_native_string(bad_bytes, sizeof(bad_bytes)));
    auto good = handle(runtime, JsValue::make_native_string(bad_bytes + 1, 1));
    auto out = handle(runtime, JsValue::make_undefined());

    auto status = fiber::script::run::Compares::eq(out, bad, good);
    ASSERT_FALSE(status.has_value());
    ASSERT_TRUE(status.is_abort());
    EXPECT_EQ(status.abort().reason, fiber::script::ScriptAbortReason::InvalidArgument);
}
