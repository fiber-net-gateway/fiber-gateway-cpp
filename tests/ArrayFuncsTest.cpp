#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "script/ScriptCompiler.h"
#include "script/gc/GcInternal.h"
#include "script/std/StdLibrary.h"

using fiber::script::AbiResult;
using fiber::script::ScriptResult;
using fiber::script::GcHeap;
using fiber::script::GcString;
using fiber::script::JsNodeType;
using fiber::script::JsValue;
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

void expect_script_null(std::string_view source) {
    GcHeap heap;
    auto result = run_script(source, heap);
    ASSERT_TRUE(result.is_success()) << "script did not succeed";
    EXPECT_EQ(js_value_type(result.value()), JsNodeType::Null);
}

void expect_script_int(std::string_view source, std::int64_t expected) {
    GcHeap heap;
    auto result = run_script(source, heap);
    ASSERT_TRUE(result.is_success()) << "script did not succeed";
    ASSERT_EQ(js_value_type(result.value()), JsNodeType::Integer);
    EXPECT_EQ(fiber::script::js_value_int64(result.value()), expected);
}

} // namespace

TEST(ArrayFuncsTest, JoinWithDelimiter) { expect_script_string("return array.join([1, 2, 3], \",\");", "1,2,3"); }

TEST(ArrayFuncsTest, JoinDefaultDelimiter) { expect_script_string("return array.join([\"a\", \"b\", \"c\"]);", "abc"); }

TEST(ArrayFuncsTest, JoinEmptyArray) { expect_script_string("return array.join([], \",\");", ""); }

TEST(ArrayFuncsTest, JoinNullElementCollapsesToEmpty) {
    expect_script_string("return array.join([1, null, 3], \"-\");", "1--3");
}

TEST(ArrayFuncsTest, JoinBooleanAndFloat) {
    expect_script_string("return array.join([true, false], \"/\");", "true/false");
    expect_script_string("return array.join([1.5, 2.5], \",\");", "1.5,2.5");
}

TEST(ArrayFuncsTest, PopReturnsLastAndMutates) {
    expect_script_int("let a = [1, 2, 3]; return array.pop(a);", 3);
    expect_script_string("let a = [1, 2, 3]; array.pop(a); return array.join(a, \",\");", "1,2");
}

TEST(ArrayFuncsTest, PopEmptyReturnsNull) { expect_script_null("return array.pop([]);"); }

TEST(ArrayFuncsTest, PushAppendsVariadicAndReturnsArray) {
    expect_script_string("let a = [1]; array.push(a, 2, 3); return array.join(a, \",\");", "1,2,3");
    expect_script_string("let a = []; let b = array.push(a, \"x\"); return array.join(b, \",\");", "x");
}

TEST(ArrayFuncsTest, NonArrayRaisesCatchableTypeError) {
    expect_script_string("try { array.join(123, \",\"); return \"no-throw\"; } catch (e) { return \"caught\"; }",
                         "caught");
    expect_script_string("try { array.pop(123); return \"no-throw\"; } catch (e) { return \"caught\"; }", "caught");
    expect_script_string("try { array.push(123, 1); return \"no-throw\"; } catch (e) { return \"caught\"; }", "caught");
}
