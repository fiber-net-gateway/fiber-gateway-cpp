#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "script/ScriptCompiler.h"
#include "script/gc/GcInternal.h"
#include "script/std/StdLibrary.h"

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

// ---- compile_template_string: compile a template-literal BODY (no surrounding backticks) ----

ScriptResult run_template(std::string_view body, GcHeap &heap) {
    auto compiled = fiber::script::compile_template_string(StdLibrary::instance(), body);
    EXPECT_TRUE(compiled.has_value()) << (compiled ? "" : compiled.error().message);
    if (!compiled) {
        return ScriptResult::abort(fiber::script::ScriptAbortReason::Internal);
    }
    JsValue root = JsValue::make_undefined();
    return compiled->exec_sync(root, nullptr, heap);
}

void expect_template_string(std::string_view body, std::string_view expected) {
    GcHeap heap;
    ScriptResult result = run_template(body, heap);
    ASSERT_TRUE(result.is_success());
    ASSERT_EQ(js_value_type(result.value()), JsNodeType::String);
    EXPECT_EQ(string_to_utf8(result.value()), expected);
}

TEST(ScriptTemplateLiteralTest, TemplateValueLiteralBody) {
    // No ${...}: body compiles to a constant string.
    expect_template_string("abc", "abc");
    expect_template_string("", "");
}

TEST(ScriptTemplateLiteralTest, TemplateValueInterpolation) {
    expect_template_string("pre-${1 + 2}-post", "pre-3-post");
    expect_template_string("${1 + 2}", "3");
    expect_template_string("v=${true}", "v=true");
    expect_template_string("v=${null}", "v=null");
}

TEST(ScriptTemplateLiteralTest, TemplateValueNestedExpressions) {
    expect_template_string("${{a: 1}.a}", "1");
    expect_template_string("outer-${`x${1 + 1}`}-end", "outer-x2-end");
}

TEST(ScriptTemplateLiteralTest, TemplateValueIsSynchronous) {
    auto compiled = fiber::script::compile_template_string(StdLibrary::instance(), "${1 + 2}");
    ASSERT_TRUE(compiled.has_value());
    EXPECT_FALSE(compiled->contains_async());
}

TEST(ScriptTemplateLiteralTest, TemplateValueMalformedBodyFails) {
    // Unterminated ${ : the backtick-wrapped body is an invalid template literal.
    auto compiled = fiber::script::compile_template_string(StdLibrary::instance(), "pre-${unclosed");
    EXPECT_FALSE(compiled.has_value());
}

TEST(ScriptTemplateLiteralTest, TemplateValueRejectsAssignmentByDefault) {
    // allow_assign defaults to false; a bare assignment in an interpolation is a compile error.
    auto compiled = fiber::script::compile_template_string(StdLibrary::instance(), "${(a = 1)}");
    EXPECT_FALSE(compiled.has_value());
}
