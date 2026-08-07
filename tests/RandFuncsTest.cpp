#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

#include <fiber/script/ScriptCompiler.h>
#include <fiber/script/gc/GcInternal.h>
#include <fiber/script/std/StdLibrary.h>
#include "ScriptTestHelpers.h"

using fiber::script::AbiResult;
using fiber::script::GcHeap;
using fiber::script::GcString;
using fiber::script::JsNodeType;
using fiber::script::JsValue;
using fiber::script::ScriptAbortReason;
using fiber::script::ScriptResult;
using fiber::script::std_lib::StdLibrary;
using fiber::test::run_script;

namespace {

const GcString *as_string(const JsValue &value) { return js_value_heap_ptr<const GcString>(value); }

std::string string_to_utf8(const JsValue &value) {
    std::string out;
    if (fiber::script::js_value_is_borrowed_string(value)) {
        fiber::script::NativeStr native = fiber::script::js_value_native_string(value);
        out.assign(native.data, native.len);
        return out;
    }
    auto *str = as_string(value);
    if (str) {
        EXPECT_TRUE(fiber::script::gc_string_to_utf8(str, out));
    }
    return out;
}

void expect_script_int(std::string_view source, std::int64_t expected) {
    GcHeap heap;
    auto result = run_script(source, heap);
    ASSERT_TRUE(result.is_success()) << "script did not succeed";
    ASSERT_EQ(js_value_type(result.value()), JsNodeType::Integer);
    EXPECT_EQ(fiber::script::js_value_int64(result.value()), expected);
}

void expect_script_bool(std::string_view source, bool expected) {
    GcHeap heap;
    auto result = run_script(source, heap);
    ASSERT_TRUE(result.is_success()) << "script did not succeed";
    ASSERT_EQ(js_value_type(result.value()), JsNodeType::Boolean);
    EXPECT_EQ(fiber::script::js_value_bool(result.value()), expected);
}

void expect_caught(std::string_view source, std::string_view expected) {
    GcHeap heap;
    auto result = run_script(source, heap);
    ASSERT_TRUE(result.is_success()) << "script did not succeed";
    ASSERT_EQ(js_value_type(result.value()), JsNodeType::String);
    EXPECT_EQ(string_to_utf8(result.value()), expected);
}

void expect_int_in_range(std::string_view source, std::int64_t lo, std::int64_t hi) {
    GcHeap heap;
    auto result = run_script(source, heap);
    ASSERT_TRUE(result.is_success()) << "script did not succeed";
    ASSERT_EQ(js_value_type(result.value()), JsNodeType::Integer);
    std::int64_t v = fiber::script::js_value_int64(result.value());
    EXPECT_GE(v, lo);
    EXPECT_LT(v, hi);
}

} // namespace

// ---- rand.random ----

TEST(RandFuncsTest, RandomDefaultMaxIs1000) { expect_int_in_range("return rand.random();", 0, 1000); }

TEST(RandFuncsTest, RandomWithinBound) { expect_int_in_range("return rand.random(10);", 0, 10); }

TEST(RandFuncsTest, RandomBoundOneAlwaysZero) { expect_script_int("return rand.random(1);", 0); }

TEST(RandFuncsTest, RandomFloatMaxTruncatesTowardZero) {
    // 3.9 truncates to bound 3, so the result is in [0, 3).
    expect_int_in_range("return rand.random(3.9);", 0, 3);
}

TEST(RandFuncsTest, RandomNonNumberRaisesTypeError) {
    expect_caught("try { rand.random(\"x\"); return \"no-throw\"; } catch (e) { return \"caught\"; }", "caught");
    expect_caught("try { rand.random(null); return \"no-throw\"; } catch (e) { return \"caught\"; }", "caught");
}

TEST(RandFuncsTest, RandomNonPositiveBoundRaisesRangeError) {
    expect_caught("try { rand.random(0); return \"no-throw\"; } catch (e) { return \"caught\"; }", "caught");
    expect_caught("try { rand.random(-5); return \"no-throw\"; } catch (e) { return \"caught\"; }", "caught");
}

// ---- rand.canary ratio bounds & lenient ratio ----

TEST(RandFuncsTest, CanaryRatioBounds) {
    expect_script_bool("return rand.canary(100);", true);
    expect_script_bool("return rand.canary(200);", true);
    expect_script_bool("return rand.canary(0);", false);
    expect_script_bool("return rand.canary(-5);", false);
}

TEST(RandFuncsTest, CanaryLenientRatioNeverThrows) {
    // Non-numeric ratio collapses to 0 -> false, mirroring Jackson's asLong(0L).
    expect_script_bool("return rand.canary(\"bad\");", false);
    expect_script_bool("return rand.canary(null);", false);
}

TEST(RandFuncsTest, CanaryNoKeysReturnsBoolean) {
    GcHeap heap;
    auto result = run_script("return rand.canary(50);", heap);
    ASSERT_TRUE(result.is_success()) << "script did not succeed";
    EXPECT_EQ(js_value_type(result.value()), JsNodeType::Boolean);
}

// ---- rand.canary deterministic CRC bucket (java.util.zip.CRC32 parity) ----
// Ground-truth vectors: crc32("123456789") % 100 = 62, crc32("user-1") % 100 = 24,
// crc32("abc") % 100 = 78, crc32("null") % 100 = 91, crc32("") % 100 = 0.

TEST(RandFuncsTest, CanaryKeyBucket123456789) {
    expect_script_bool("return rand.canary(63, \"123456789\");", true); // 62 < 63
    expect_script_bool("return rand.canary(62, \"123456789\");", false); // 62 < 62 false
}

TEST(RandFuncsTest, CanaryKeyBucketUser1) {
    expect_script_bool("return rand.canary(25, \"user-1\");", true); // 24 < 25
    expect_script_bool("return rand.canary(24, \"user-1\");", false); // 24 < 24 false
}

TEST(RandFuncsTest, CanaryKeyBucketAbc) {
    expect_script_bool("return rand.canary(50, \"abc\");", false); // 78 < 50 false
    expect_script_bool("return rand.canary(79, \"abc\");", true); // 78 < 79
}

TEST(RandFuncsTest, CanaryNullKeyContributesNullText) {
    // null renders to "null" (4 bytes), not skipped: bucket 91, distinct from empty's 0.
    expect_script_bool("return rand.canary(50, \"\");", true); // empty skipped -> bucket 0 -> 0 < 50
    expect_script_bool("return rand.canary(50, null);", false); // "null" -> bucket 91 -> 91 < 50 false
    expect_script_bool("return rand.canary(92, null);", true); // 91 < 92
    expect_script_bool("return rand.canary(91, null);", false); // 91 < 91 false
}

TEST(RandFuncsTest, CanaryAllEmptyKeysHitsZeroBucket) {
    // No non-empty key text -> CRC of nothing == 0 -> 0 % 100 == 0 < 50 -> true.
    expect_script_bool("return rand.canary(50, \"\");", true);
    expect_script_bool("return rand.canary(50, \"\", \"\");", true);
}

TEST(RandFuncsTest, CanaryMultipleKeysAreCumulativeAndOrderSensitive) {
    // crc32("abcuser-1") % 100 = 40; crc32("user-1abc") % 100 = 66.
    expect_script_bool("return rand.canary(50, \"abc\", \"user-1\");", true); // 40 < 50
    expect_script_bool("return rand.canary(40, \"abc\", \"user-1\");", false); // 40 < 40 false
    expect_script_bool("return rand.canary(50, \"user-1\", \"abc\");", false); // 66 < 50 false (order matters)
    expect_script_bool("return rand.canary(67, \"user-1\", \"abc\");", true); // 66 < 67
}

TEST(RandFuncsTest, CanaryEmptyKeysSkippedBetweenNonEmpty) {
    // Empty key in the middle is skipped; crc == crc32("abcuser-1") % 100 = 40.
    expect_script_bool("return rand.canary(50, \"abc\", \"\", \"user-1\");", true);
    expect_script_bool("return rand.canary(40, \"abc\", \"\", \"user-1\");", false);
}
