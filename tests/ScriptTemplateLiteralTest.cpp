#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "common/json/JsGc.h"
#include "script/ScriptCompiler.h"
#include "script/std/StdLibrary.h"

using fiber::json::GcHeap;
using fiber::json::GcString;
using fiber::json::JsNodeType;
using fiber::json::JsValue;
using fiber::script::ScriptResult;
using fiber::script::std_lib::StdLibrary;

namespace {

const GcString *as_string(const JsValue &value) { return js_value_heap_ptr<const GcString>(value); }

std::string string_to_utf8(const JsValue &value) {
    std::string out;
    if (fiber::json::js_value_is_borrowed_string(value)) {
        fiber::json::NativeStr native = fiber::json::js_value_native_string(value);
        out.assign(native.data, native.len);
        return out;
    }
    auto *str = as_string(value);
    if (str) {
        EXPECT_TRUE(fiber::json::gc_string_to_utf8(str, out));
    }
    return out;
}

ScriptResult run_script(std::string_view source, GcHeap &heap) {
    auto compiled = fiber::script::compile_script(StdLibrary::instance(), source);
    EXPECT_TRUE(compiled.has_value()) << (compiled ? "" : compiled.error().message);
    if (!compiled) {
        return ScriptResult::abort(fiber::script::ScriptAbortReason::InvalidArgument);
    }
    JsValue root = JsValue::make_undefined();
    auto run = compiled->exec_sync(root, nullptr, heap);
    return run();
}

void expect_script_string(std::string_view source, std::string_view expected) {
    GcHeap heap;
    ScriptResult result = run_script(source, heap);
    ASSERT_TRUE(result.is_success());
    ASSERT_EQ(js_value_type(result.value()), JsNodeType::String);
    EXPECT_EQ(string_to_utf8(result.value()), expected);
}

} // namespace

TEST(ScriptTemplateLiteralTest, PlainTemplateLiteral) { expect_script_string("return `abc`;", "abc"); }

TEST(ScriptTemplateLiteralTest, MultilineTemplateLiteral) { expect_script_string("return `a\nb`;", "a\nb"); }

TEST(ScriptTemplateLiteralTest, InterpolatesPrimitiveValues) {
    expect_script_string("let a = 1; return `1 + ${a}`;", "1 + 1");
    expect_script_string("let a = true; return `v=${a}`;", "v=true");
    expect_script_string("let a = null; return `v=${a}`;", "v=null");
}

TEST(ScriptTemplateLiteralTest, ExpressionOnlyTemplateForcesString) {
    expect_script_string("let a = 1; return `${a}`;", "1");
}

TEST(ScriptTemplateLiteralTest, InterpolatesExpressionBetweenChunks) {
    expect_script_string("let a = 1; return `x${a + 2}y`;", "x3y");
}

TEST(ScriptTemplateLiteralTest, HandlesNestedBracesAndTemplatesInExpression) {
    expect_script_string("return `${{a: 1}.a}`;", "1");
    expect_script_string("let a = 1; return `${`x${a}`}`;", "x1");
}

TEST(ScriptTemplateLiteralTest, EscapesBacktickAndInterpolationStart) {
    expect_script_string("return `\\`${1}\\${2}`;", "`1${2}");
}
