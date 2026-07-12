#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "script/ScriptCompiler.h"
#include "script/gc/GcInternal.h"
#include "script/std/StdLibrary.h"

using fiber::script::AbiResult;
using fiber::script::GcHeap;
using fiber::script::GcString;
using fiber::script::JsNodeType;
using fiber::script::JsValue;
using fiber::script::ScriptResult;
using fiber::script::std_lib::StdLibrary;

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

ScriptResult run_script(std::string_view source, GcHeap &heap) {
    auto compiled = fiber::script::compile_script(StdLibrary::instance(), source);
    EXPECT_TRUE(compiled.has_value()) << (compiled ? "" : compiled.error().message);
    if (!compiled) {
        return ScriptResult::abort(fiber::script::ScriptAbortReason::Internal);
    }
    JsValue root = JsValue::make_undefined();
    return compiled->exec_sync(root, nullptr, heap);
}

void expect_script_string(std::string_view source, std::string_view expected) {
    GcHeap heap;
    auto result = run_script(source, heap);
    ASSERT_TRUE(result.is_success()) << "script did not succeed";
    ASSERT_EQ(js_value_type(result.value()), JsNodeType::String);
    EXPECT_EQ(string_to_utf8(result.value()), expected);
}

void expect_script_int(std::string_view source, std::int64_t expected) {
    GcHeap heap;
    auto result = run_script(source, heap);
    ASSERT_TRUE(result.is_success()) << "script did not succeed";
    ASSERT_EQ(js_value_type(result.value()), JsNodeType::Integer);
    EXPECT_EQ(fiber::script::js_value_int64(result.value()), expected);
}

void expect_script_double(std::string_view source, double expected) {
    GcHeap heap;
    auto result = run_script(source, heap);
    ASSERT_TRUE(result.is_success()) << "script did not succeed";
    ASSERT_EQ(js_value_type(result.value()), JsNodeType::Float);
    EXPECT_DOUBLE_EQ(fiber::script::js_value_double(result.value()), expected);
}

} // namespace

TEST(MathFuncsTest, FloorIntegerPassthrough) { expect_script_int("return math.floor(5);", 5); }

TEST(MathFuncsTest, FloorPositiveFloatRoundsDown) { expect_script_int("return math.floor(2.9);", 2); }

TEST(MathFuncsTest, FloorNegativeFloatTowardNegativeInf) { expect_script_int("return math.floor(-2.1);", -3); }

TEST(MathFuncsTest, FloorWholeFloatYieldsInteger) { expect_script_int("return math.floor(7.0);", 7); }

TEST(MathFuncsTest, AbsPositiveInteger) { expect_script_int("return math.abs(5);", 5); }

TEST(MathFuncsTest, AbsNegativeInteger) { expect_script_int("return math.abs(-5);", 5); }

TEST(MathFuncsTest, AbsNegativeFloat) { expect_script_double("return math.abs(-2.5);", 2.5); }

TEST(MathFuncsTest, AbsPositiveFloat) { expect_script_double("return math.abs(2.5);", 2.5); }

TEST(MathFuncsTest, NonNumericRaisesCatchableTypeError) {
    expect_script_string("try { math.floor(\"x\"); return \"no-throw\"; } catch (e) { return \"caught\"; }", "caught");
    expect_script_string("try { math.abs(null); return \"no-throw\"; } catch (e) { return \"caught\"; }", "caught");
}
