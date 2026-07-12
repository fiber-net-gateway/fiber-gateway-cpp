#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "script/JsGc.h"
#include "script/ScriptCompiler.h"
#include "script/gc/GcInternal.h"
#include "script/std/StdLibrary.h"

using fiber::script::AbiResult;
using fiber::script::ConstValueHandle;
using fiber::script::ExceptionKind;
using fiber::script::GcHeap;
using fiber::script::GcString;
using fiber::script::JsNodeType;
using fiber::script::JsValue;
using fiber::script::Library;
using fiber::script::ScriptAbortReason;
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
        return ScriptResult::abort(ScriptAbortReason::Internal);
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

void expect_script_bool(std::string_view source, bool expected) {
    GcHeap heap;
    auto result = run_script(source, heap);
    ASSERT_TRUE(result.is_success()) << "script did not succeed";
    ASSERT_EQ(js_value_type(result.value()), JsNodeType::Boolean);
    EXPECT_EQ(fiber::script::js_value_bool(result.value()), expected);
}

void expect_script_null(std::string_view source) {
    GcHeap heap;
    auto result = run_script(source, heap);
    ASSERT_TRUE(result.is_success()) << "script did not succeed";
    EXPECT_EQ(js_value_type(result.value()), JsNodeType::Null);
}

void expect_script_undefined(std::string_view source) {
    GcHeap heap;
    auto result = run_script(source, heap);
    ASSERT_TRUE(result.is_success()) << "script did not succeed";
    EXPECT_EQ(js_value_type(result.value()), JsNodeType::Undefined);
}

void expect_caught(std::string_view source) {
    GcHeap heap;
    auto result = run_script(source, heap);
    ASSERT_TRUE(result.is_success()) << "script did not succeed";
    ASSERT_EQ(js_value_type(result.value()), JsNodeType::String);
    EXPECT_EQ(string_to_utf8(result.value()), "caught");
}

// Resolves a host function and invokes it directly with one JsValue argument,
// mirroring how the interpreter dispatches host calls. The GcHeap is owned by the
// caller so heap-allocated results (e.g. stringify strings) stay valid for
// inspection. Mirrors BinaryFuncsTest's call_host.
AbiResult call_host_on(GcHeap &heap, const char *name, JsValue arg) {
    JsValue storage[1] = {arg};
    auto match = StdLibrary::instance().resolve_func(
            name, Library::FunctionMatchRequest{.known_argc = 1, .has_spread = false, .spread_argc_unknown = false});
    EXPECT_EQ(match.status, Library::FunctionMatchStatus::Found);
    if (match.status != Library::FunctionMatchStatus::Found || match.callable == nullptr ||
        match.callable->function == nullptr) {
        return AbiResult::abort(ScriptAbortReason::Internal);
    }
    Library::HostCallFrame frame(heap, JsValue::make_undefined(), nullptr);
    Library::Arguments args{ConstValueHandle(storage), 1};
    return match.callable->function(match.callable->userdata, frame, args);
}

void expect_exception_kind(const char *name, JsValue arg, ExceptionKind kind) {
    GcHeap heap;
    AbiResult result = call_host_on(heap, name, arg);
    ASSERT_TRUE(result.is_exception()) << name << " did not raise";
    EXPECT_EQ(fiber::script::js_value_exception_kind(result.exception()), kind) << name;
}

} // namespace

// ---- JSON.parse: values ----

TEST(JsonFuncsTest, ParseObjectNested) {
    expect_script_int(R"(let o = JSON.parse('{"a":1,"b":[2,3]}'); return o.a + o.b[0] + o.b[1];)", 6);
}

TEST(JsonFuncsTest, ParseInteger) { expect_script_int(R"(return JSON.parse("123");)", 123); }

TEST(JsonFuncsTest, ParseTrue) { expect_script_bool(R"(return JSON.parse("true");)", true); }

TEST(JsonFuncsTest, ParseFalse) { expect_script_bool(R"(return JSON.parse("false");)", false); }

TEST(JsonFuncsTest, ParseNullIsNull) { expect_script_null(R"(return JSON.parse("null");)"); }

TEST(JsonFuncsTest, ParseStringScalar) { expect_script_string(R"(return JSON.parse("\"str\"");)", "str"); }

TEST(JsonFuncsTest, ParseArrayLength) { expect_script_int(R"(return length(JSON.parse("[1,2,3]"));)", 3); }

TEST(JsonFuncsTest, ParseUnicodeString) {
    // Raw UTF-8 (U+00E9 = "é", two bytes) passes through the JSON decoder.
    expect_script_string(R"(return JSON.parse("\"é\"");)", "é");
}

// ---- JSON.parse: errors ----

TEST(JsonFuncsTest, ParseNonStringIsCatchable) {
    expect_caught(R"(try { JSON.parse(123); return "no"; } catch (e) { return "caught"; })");
}

TEST(JsonFuncsTest, ParseMalformedIsCatchable) {
    expect_caught(R"(try { JSON.parse("{bad"); return "no"; } catch (e) { return "caught"; })");
    expect_caught(R"(try { JSON.parse(""); return "no"; } catch (e) { return "caught"; })");
}

TEST(JsonFuncsTest, ParseNonStringRaisesTypeError) {
    expect_exception_kind("JSON.parse", JsValue::make_integer(123), ExceptionKind::TypeError);
    expect_exception_kind("JSON.parse", JsValue::make_null(), ExceptionKind::TypeError);
    expect_exception_kind("JSON.parse", JsValue::make_boolean(true), ExceptionKind::TypeError);
}

// ---- JSON.stringify: values ----

TEST(JsonFuncsTest, StringifyObject) { expect_script_string(R"(return JSON.stringify({a: 1});)", "{\"a\":1}"); }

TEST(JsonFuncsTest, StringifyArray) { expect_script_string(R"(return JSON.stringify([1, 2, 3]);)", "[1,2,3]"); }

TEST(JsonFuncsTest, StringifyString) { expect_script_string(R"(return JSON.stringify("x");)", "\"x\""); }

TEST(JsonFuncsTest, StringifyInteger) { expect_script_string(R"(return JSON.stringify(123);)", "123"); }

TEST(JsonFuncsTest, StringifyBoolean) { expect_script_string(R"(return JSON.stringify(true);)", "true"); }

TEST(JsonFuncsTest, StringifyNull) { expect_script_string(R"(return JSON.stringify(null);)", "null"); }

TEST(JsonFuncsTest, StringifyFloat) { expect_script_string(R"(return JSON.stringify(1.5);)", "1.5"); }

TEST(JsonFuncsTest, StringifyUndefinedReturnsUndefined) {
    // o.b is a missing property -> undefined; stringify(undefined) yields undefined.
    expect_script_undefined(R"(let o = {a: 1}; return JSON.stringify(o.b);)");
}

TEST(JsonFuncsTest, StringifyNaNSerializesAsNull) {
    GcHeap heap;
    AbiResult result =
            call_host_on(heap, "JSON.stringify", JsValue::make_float(std::numeric_limits<double>::quiet_NaN()));
    ASSERT_TRUE(result.is_success());
    ASSERT_EQ(js_value_type(result.value()), JsNodeType::String);
    EXPECT_EQ(string_to_utf8(result.value()), "null");
}

TEST(JsonFuncsTest, StringifyInfinitySerializesAsNull) {
    GcHeap heap;
    AbiResult result =
            call_host_on(heap, "JSON.stringify", JsValue::make_float(std::numeric_limits<double>::infinity()));
    ASSERT_TRUE(result.is_success());
    ASSERT_EQ(js_value_type(result.value()), JsNodeType::String);
    EXPECT_EQ(string_to_utf8(result.value()), "null");
}

// ---- round-trip ----

TEST(JsonFuncsTest, RoundTripObject) {
    expect_script_bool(R"(return JSON.stringify(JSON.parse('{"a":1,"b":[2,3]}')) === '{"a":1,"b":[2,3]}';)", true);
}
