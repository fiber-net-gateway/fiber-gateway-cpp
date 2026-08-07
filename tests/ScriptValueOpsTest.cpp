#include <gtest/gtest.h>

#include <limits>
#include <type_traits>

#include <fiber/script/ScriptResult.h>
#include <fiber/script/gc/GcInternal.h>
#include <fiber/script/run/Binaries.h>
#include <fiber/script/run/Compares.h>
#include <fiber/script/run/Unaries.h>

using fiber::script::CallResult;
using fiber::script::GcHeap;
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

TEST(ScriptValueOpsTest, ConcatKeepsCanonicalUtf8ForNativeUtf8) {
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
    EXPECT_EQ(str->utf16_len, 2u);
    EXPECT_EQ(fiber::script::gc_string_byte_len(str), 4u);

    const char expected_bytes[] = {
            static_cast<char>(0xC3),
            static_cast<char>(0xA9),
            static_cast<char>(0xC3),
            static_cast<char>(0x9F),
    };
    std::string expected(expected_bytes, sizeof(expected_bytes));
    EXPECT_EQ(string_to_utf8(result.value), expected);
}

TEST(ScriptValueOpsTest, ConcatKeepsCanonicalUtf8ForWideCharacters) {
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
    EXPECT_EQ(str->utf16_len, 2u);
    EXPECT_EQ(fiber::script::gc_string_byte_len(str), 4u);

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
    EXPECT_EQ(str->utf16_len, 4u);
    EXPECT_EQ(fiber::script::gc_string_byte_len(str), 4u);

    const char expected_bytes[] = {'h', 'i', '!', '!'};
    std::string expected(expected_bytes, sizeof(expected_bytes));
    EXPECT_EQ(string_to_utf8(result.value), expected);
}

TEST(ScriptValueOpsTest, ConcatCanonicalizesSurrogatePairBoundary) {
    GcHeap heap;
    GcString *high = nullptr;
    GcString *low = nullptr;
    GcString *emoji = nullptr;
    {
        GcHeap::NoGcScope no_gc(heap);
        const char16_t high_unit = static_cast<char16_t>(0xD83D);
        const char16_t low_unit = static_cast<char16_t>(0xDE00);
        high = fiber::script::gc_new_string_utf16(&heap, &high_unit, 1);
        low = fiber::script::gc_new_string_utf16(&heap, &low_unit, 1);
        emoji = fiber::script::gc_new_string(&heap, "\xF0\x9F\x98\x80", 4);
    }
    ASSERT_NE(high, nullptr);
    ASSERT_NE(low, nullptr);
    ASSERT_NE(emoji, nullptr);

    auto lhs = handle(heap, fiber::script::js_make_heap_ref(&high->hdr, fiber::script::GcHeapKind::String));
    auto rhs = handle(heap, fiber::script::js_make_heap_ref(&low->hdr, fiber::script::GcHeapKind::String));
    auto expected = handle(heap, fiber::script::js_make_heap_ref(&emoji->hdr, fiber::script::GcHeapKind::String));
    ResultPayload result;
    ASSERT_EQ(fiber::script::run::Binaries::plus(heap, lhs, rhs, result), CallResult::Success);
    EXPECT_EQ(as_string(result.value), as_string(*expected));
    EXPECT_EQ(fiber::script::gc_string_byte_len(as_string(result.value)), 4u);
    EXPECT_EQ(string_to_utf8(result.value), "\xF0\x9F\x98\x80");
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

TEST(ScriptValueOpsTest, CompareHeapAsciiStrings) {
    GcHeap heap;
    auto lhs = handle(heap, JsValue::make_string(heap, "ab", 2));
    auto rhs = handle(heap, JsValue::make_string(heap, "aba", 3));

    EXPECT_TRUE(fiber::script::run::Compares::lt(lhs, rhs));
    EXPECT_FALSE(fiber::script::run::Compares::gt(lhs, rhs));
}

TEST(ScriptValueOpsTest, CompareHeapNonAsciiStrings) {
    GcHeap heap;
    char omega_bytes[] = {static_cast<char>(0xCE), static_cast<char>(0xA9)};
    char euro_bytes[] = {static_cast<char>(0xE2), static_cast<char>(0x82), static_cast<char>(0xAC)};
    auto omega = handle(heap, JsValue::make_string(heap, omega_bytes, sizeof(omega_bytes)));
    auto euro = handle(heap, JsValue::make_string(heap, euro_bytes, sizeof(euro_bytes)));

    EXPECT_TRUE(fiber::script::run::Compares::lt(omega, euro));
    EXPECT_TRUE(fiber::script::run::Compares::eq(euro, euro));
}

TEST(ScriptValueOpsTest, CompareHeapAsciiAndNonAscii) {
    GcHeap heap;
    auto ascii = handle(heap, JsValue::make_string(heap, "A", 1));
    char euro_bytes[] = {static_cast<char>(0xE2), static_cast<char>(0x82), static_cast<char>(0xAC)};
    auto euro = handle(heap, JsValue::make_string(heap, euro_bytes, sizeof(euro_bytes)));

    EXPECT_TRUE(fiber::script::run::Compares::lt(ascii, euro));
}

TEST(ScriptValueOpsTest, CompareHeapAndNativeUtf8) {
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

TEST(ScriptValueOpsTest, ComparisonUsesUtf16CodeUnitOrder) {
    GcHeap heap;
    auto supplementary = handle(heap, JsValue::make_string(heap, "\xF0\x90\x80\x80", 4)); // U+10000
    auto bmp_private = handle(heap, JsValue::make_string(heap, "\xEE\x80\x80", 3)); // U+E000

    EXPECT_TRUE(fiber::script::run::Compares::lt(supplementary, bmp_private));
    EXPECT_FALSE(fiber::script::run::Compares::gt(supplementary, bmp_private));
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
