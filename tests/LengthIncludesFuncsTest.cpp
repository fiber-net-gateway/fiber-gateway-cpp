#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "script/ScriptCompiler.h"
#include "script/gc/GcInternal.h"
#include "script/std/StdLibrary.h"

using fiber::script::GcHeap;
using fiber::script::JsNodeType;
using fiber::script::JsValue;
using fiber::script::ScriptResult;
using fiber::script::std_lib::StdLibrary;

namespace {

ScriptResult run_script(std::string_view source, GcHeap &heap) {
    auto compiled = fiber::script::compile_script(StdLibrary::instance(), source);
    EXPECT_TRUE(compiled.has_value()) << (compiled ? "" : compiled.error().message);
    if (!compiled) {
        return ScriptResult::abort(fiber::script::ScriptAbortReason::Internal);
    }
    JsValue root = JsValue::make_undefined();
    return compiled->exec_sync(root, nullptr, heap);
}

void expect_script_int(std::string_view source, std::int64_t expected) {
    GcHeap heap;
    ScriptResult result = run_script(source, heap);
    ASSERT_TRUE(result.is_success()) << "script did not succeed";
    ASSERT_EQ(js_value_type(result.value()), JsNodeType::Integer);
    EXPECT_EQ(fiber::script::js_value_int64(result.value()), expected);
}

void expect_script_bool(std::string_view source, bool expected) {
    GcHeap heap;
    ScriptResult result = run_script(source, heap);
    ASSERT_TRUE(result.is_success()) << "script did not succeed";
    ASSERT_EQ(js_value_type(result.value()), JsNodeType::Boolean);
    EXPECT_EQ(fiber::script::js_value_bool(result.value()), expected);
}

} // namespace

// ---- length ----

TEST(LengthFuncTest, StringLengthIsUtf16CodeUnits) {
    expect_script_int("return length(\"abc\");", 3);
    expect_script_int("return length(\"héllo\");", 5); // é is one BMP code unit
    expect_script_int("return length(\"😀\");", 2); // supplementary -> surrogate pair
    expect_script_int("return length(\"😀\"[0]);", 1);
}

TEST(LengthFuncTest, SplitSurrogatesConcatBackToCanonicalString) {
    expect_script_bool("return \"😀\"[0] + \"😀\"[1] === \"😀\";", true);
}

TEST(LengthFuncTest, ContainerLength) {
    expect_script_int("return length({a:1, b:2});", 2);
    expect_script_int("return length([1, 2, 3]);", 3);
}

TEST(LengthFuncTest, ScalarAndNullLengthIsZero) {
    expect_script_int("return length(1);", 0);
    expect_script_int("return length(true);", 0);
    expect_script_int("return length(1.5);", 0);
    expect_script_int("return length(null);", 0);
    expect_script_int("return length();", 0); // default null parameter
}

// ---- includes ----

TEST(IncludesFuncTest, TextSubstring) {
    expect_script_bool("return includes(\"abcabc\", \"cab\");", true);
    expect_script_bool("return includes(\"abc\", \"d\");", false);
    expect_script_bool("return includes(\"abc\");", true); // no items -> vacuously true
}

TEST(IncludesFuncTest, TextItemMustBeString) { expect_script_bool("return includes(\"abc\", 1);", false); }

TEST(IncludesFuncTest, ArrayMembershipStrictEqual) {
    expect_script_bool("return includes([\"aa\", \"bb\", \"cc\"], \"aa\");", true);
    expect_script_bool("return includes([\"a\", 1], 1);", true);
    expect_script_bool("return includes([\"aa\", \"bb\"], \"a\");", false); // substring != element
    expect_script_bool("return includes([1, 2], 3);", false);
}

TEST(IncludesFuncTest, NonContainerIsFalse) {
    expect_script_bool("return includes({a:1}, \"a\");", false);
    expect_script_bool("return includes(123, 1);", false);
}
