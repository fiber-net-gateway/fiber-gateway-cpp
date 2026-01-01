#include <gtest/gtest.h>

#include "common/json/JsValueOps.h"

using fiber::json::GcHeap;
using fiber::json::GcString;
using fiber::json::GcStringEncoding;
using fiber::json::JsBinaryOp;
using fiber::json::JsNodeType;
using fiber::json::JsOpError;
using fiber::json::JsUnaryOp;
using fiber::json::JsValue;

namespace {

const GcString *as_string(const JsValue &value) {
    return reinterpret_cast<const GcString *>(value.gc);
}

std::string string_to_utf8(const JsValue &value) {
    std::string out;
    auto *str = as_string(value);
    if (str) {
        EXPECT_TRUE(fiber::json::gc_string_to_utf8(str, out));
    }
    return out;
}

} // namespace

TEST(JsValueOpsTest, ConcatKeepsByteForNativeUtf8) {
    GcHeap heap;
    char left_bytes[] = {static_cast<char>(0xC3), static_cast<char>(0xA9)};
    char right_bytes[] = {static_cast<char>(0xC3), static_cast<char>(0x9F)};
    JsValue lhs = JsValue::make_native_string(left_bytes, sizeof(left_bytes));
    JsValue rhs = JsValue::make_native_string(right_bytes, sizeof(right_bytes));

    auto result = fiber::json::js_binary_op(JsBinaryOp::Add, lhs, rhs, &heap);
    ASSERT_EQ(result.error, JsOpError::None);
    ASSERT_EQ(result.value.type_, JsNodeType::HeapString);
    auto *str = as_string(result.value);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(str->encoding, GcStringEncoding::Byte);
    EXPECT_EQ(str->len, 2u);

    const char expected_bytes[] = {
        static_cast<char>(0xC3), static_cast<char>(0xA9),
        static_cast<char>(0xC3), static_cast<char>(0x9F),
    };
    std::string expected(expected_bytes, sizeof(expected_bytes));
    EXPECT_EQ(string_to_utf8(result.value), expected);
}

TEST(JsValueOpsTest, ConcatUpgradesToUtf16ForWide) {
    GcHeap heap;
    char euro_bytes[] = {static_cast<char>(0xE2), static_cast<char>(0x82), static_cast<char>(0xAC)};
    char ascii_bytes[] = {'A'};
    JsValue lhs = JsValue::make_native_string(euro_bytes, sizeof(euro_bytes));
    JsValue rhs = JsValue::make_native_string(ascii_bytes, sizeof(ascii_bytes));

    auto result = fiber::json::js_binary_op(JsBinaryOp::Add, lhs, rhs, &heap);
    ASSERT_EQ(result.error, JsOpError::None);
    ASSERT_EQ(result.value.type_, JsNodeType::HeapString);
    auto *str = as_string(result.value);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(str->encoding, GcStringEncoding::Utf16);
    EXPECT_EQ(str->len, 2u);

    const char expected_bytes[] = {
        static_cast<char>(0xE2), static_cast<char>(0x82), static_cast<char>(0xAC), 'A',
    };
    std::string expected(expected_bytes, sizeof(expected_bytes));
    EXPECT_EQ(string_to_utf8(result.value), expected);
}

TEST(JsValueOpsTest, ConcatHeapAndNative) {
    GcHeap heap;
    JsValue lhs = JsValue::make_string(heap, "hi", 2);
    char right_bytes[] = {'!', '!'};
    JsValue rhs = JsValue::make_native_string(right_bytes, sizeof(right_bytes));

    auto result = fiber::json::js_binary_op(JsBinaryOp::Add, lhs, rhs, &heap);
    ASSERT_EQ(result.error, JsOpError::None);
    ASSERT_EQ(result.value.type_, JsNodeType::HeapString);
    auto *str = as_string(result.value);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(str->encoding, GcStringEncoding::Byte);
    EXPECT_EQ(str->len, 4u);

    const char expected_bytes[] = {'h', 'i', '!', '!'};
    std::string expected(expected_bytes, sizeof(expected_bytes));
    EXPECT_EQ(string_to_utf8(result.value), expected);
}

TEST(JsValueOpsTest, AddInteger) {
    JsValue lhs = JsValue::make_integer(3);
    JsValue rhs = JsValue::make_integer(4);
    auto result = fiber::json::js_binary_op(JsBinaryOp::Add, lhs, rhs, nullptr);
    ASSERT_EQ(result.error, JsOpError::None);
    EXPECT_EQ(result.value.type_, JsNodeType::Integer);
    EXPECT_EQ(result.value.i, 7);
}

TEST(JsValueOpsTest, AddStringAndNumberTypeError) {
    GcHeap heap;
    JsValue lhs = JsValue::make_string(heap, "hi", 2);
    JsValue rhs = JsValue::make_integer(1);
    auto result = fiber::json::js_binary_op(JsBinaryOp::Add, lhs, rhs, &heap);
    EXPECT_EQ(result.error, JsOpError::TypeError);
}

TEST(JsValueOpsTest, UnaryLogicalNot) {
    JsValue value = JsValue::make_integer(0);
    auto result = fiber::json::js_unary_op(JsUnaryOp::LogicalNot, value);
    ASSERT_EQ(result.error, JsOpError::None);
    EXPECT_EQ(result.value.type_, JsNodeType::Boolean);
    EXPECT_TRUE(result.value.b);
}

TEST(JsValueOpsTest, LooseAndStrictEquality) {
    GcHeap heap;
    char one_bytes[] = {'1'};
    JsValue str = JsValue::make_native_string(one_bytes, sizeof(one_bytes));
    JsValue num = JsValue::make_integer(1);

    auto loose = fiber::json::js_binary_op(JsBinaryOp::Eq, str, num, &heap);
    ASSERT_EQ(loose.error, JsOpError::None);
    EXPECT_EQ(loose.value.type_, JsNodeType::Boolean);
    EXPECT_TRUE(loose.value.b);

    auto strict = fiber::json::js_binary_op(JsBinaryOp::StrictEq, str, num, &heap);
    ASSERT_EQ(strict.error, JsOpError::None);
    EXPECT_EQ(strict.value.type_, JsNodeType::Boolean);
    EXPECT_FALSE(strict.value.b);

    JsValue null_value = JsValue::make_null();
    JsValue undef_value = JsValue::make_undefined();
    auto loose_null = fiber::json::js_binary_op(JsBinaryOp::Eq, null_value, undef_value, &heap);
    ASSERT_EQ(loose_null.error, JsOpError::None);
    EXPECT_TRUE(loose_null.value.b);
    auto strict_null = fiber::json::js_binary_op(JsBinaryOp::StrictEq, null_value, undef_value, &heap);
    ASSERT_EQ(strict_null.error, JsOpError::None);
    EXPECT_FALSE(strict_null.value.b);
}

TEST(JsValueOpsTest, CompareHeapByteStrings) {
    GcHeap heap;
    JsValue lhs = JsValue::make_string(heap, "ab", 2);
    JsValue rhs = JsValue::make_string(heap, "aba", 3);

    auto lt = fiber::json::js_binary_op(JsBinaryOp::Lt, lhs, rhs, nullptr);
    ASSERT_EQ(lt.error, JsOpError::None);
    EXPECT_TRUE(lt.value.b);

    auto gt = fiber::json::js_binary_op(JsBinaryOp::Gt, lhs, rhs, nullptr);
    ASSERT_EQ(gt.error, JsOpError::None);
    EXPECT_FALSE(gt.value.b);
}

TEST(JsValueOpsTest, CompareHeapUtf16Strings) {
    GcHeap heap;
    char omega_bytes[] = {static_cast<char>(0xCE), static_cast<char>(0xA9)};
    char euro_bytes[] = {static_cast<char>(0xE2), static_cast<char>(0x82), static_cast<char>(0xAC)};
    JsValue omega = JsValue::make_string(heap, omega_bytes, sizeof(omega_bytes));
    JsValue euro = JsValue::make_string(heap, euro_bytes, sizeof(euro_bytes));

    auto lt = fiber::json::js_binary_op(JsBinaryOp::Lt, omega, euro, nullptr);
    ASSERT_EQ(lt.error, JsOpError::None);
    EXPECT_TRUE(lt.value.b);

    auto eq = fiber::json::js_binary_op(JsBinaryOp::Eq, euro, euro, nullptr);
    ASSERT_EQ(eq.error, JsOpError::None);
    EXPECT_TRUE(eq.value.b);
}

TEST(JsValueOpsTest, CompareHeapByteAndHeapUtf16) {
    GcHeap heap;
    JsValue ascii = JsValue::make_string(heap, "A", 1);
    char euro_bytes[] = {static_cast<char>(0xE2), static_cast<char>(0x82), static_cast<char>(0xAC)};
    JsValue euro = JsValue::make_string(heap, euro_bytes, sizeof(euro_bytes));

    auto lt = fiber::json::js_binary_op(JsBinaryOp::Lt, ascii, euro, nullptr);
    ASSERT_EQ(lt.error, JsOpError::None);
    EXPECT_TRUE(lt.value.b);
}

TEST(JsValueOpsTest, CompareHeapAndNativeByte) {
    GcHeap heap;
    char cafe_bytes[] = {'c', 'a', 'f', static_cast<char>(0xC3), static_cast<char>(0xA9)};
    JsValue heap_value = JsValue::make_string(heap, cafe_bytes, sizeof(cafe_bytes));
    JsValue native_value = JsValue::make_native_string(cafe_bytes, sizeof(cafe_bytes));

    auto eq = fiber::json::js_binary_op(JsBinaryOp::StrictEq, heap_value, native_value, nullptr);
    ASSERT_EQ(eq.error, JsOpError::None);
    EXPECT_TRUE(eq.value.b);
}

TEST(JsValueOpsTest, CompareNativeWithSurrogatePair) {
    GcHeap heap;
    char smile_bytes[] = {static_cast<char>(0xF0), static_cast<char>(0x9F), static_cast<char>(0x98),
                          static_cast<char>(0x80)};
    JsValue heap_smile = JsValue::make_string(heap, smile_bytes, sizeof(smile_bytes));
    JsValue native_smile = JsValue::make_native_string(smile_bytes, sizeof(smile_bytes));

    auto eq = fiber::json::js_binary_op(JsBinaryOp::Eq, heap_smile, native_smile, nullptr);
    ASSERT_EQ(eq.error, JsOpError::None);
    EXPECT_TRUE(eq.value.b);

    JsValue bang = JsValue::make_string(heap, "!", 1);
    auto gt = fiber::json::js_binary_op(JsBinaryOp::Gt, heap_smile, bang, nullptr);
    ASSERT_EQ(gt.error, JsOpError::None);
    EXPECT_TRUE(gt.value.b);
}

TEST(JsValueOpsTest, CompareInvalidUtf8) {
    char bad_bytes[] = {static_cast<char>(0xC3), static_cast<char>(0x28)};
    JsValue bad = JsValue::make_native_string(bad_bytes, sizeof(bad_bytes));
    JsValue good = JsValue::make_native_string(bad_bytes + 1, 1);

    auto eq = fiber::json::js_binary_op(JsBinaryOp::Eq, bad, good, nullptr);
    EXPECT_EQ(eq.error, JsOpError::InvalidUtf8);
}
