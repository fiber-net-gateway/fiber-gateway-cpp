#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include <fiber/script/ScriptCompiler.h>
#include <fiber/script/gc/GcInternal.h>
#include <fiber/script/std/StdLibrary.h>
#include "ScriptTestHelpers.h"
#include "support/AllocFailure.h"

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
    const auto *str = as_string(value);
    if (str) {
        EXPECT_TRUE(fiber::script::gc_string_to_utf8(str, out));
    }
    return out;
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

void expect_script_bool(std::string_view source, bool expected) {
    GcHeap heap;
    auto result = run_script(source, heap);
    ASSERT_TRUE(result.is_success()) << "script did not succeed";
    ASSERT_EQ(js_value_type(result.value()), JsNodeType::Boolean);
    EXPECT_EQ(fiber::script::js_value_bool(result.value()), expected);
}

} // namespace

// ---- URL.encodeComponent ----

TEST(UrlFuncsTest, EncodeComponentSpacesAndUnreserved) {
    expect_script_string("return URL.encodeComponent('a b');", "a+b");
    expect_script_string("return URL.encodeComponent('a*b-c.d_e');", "a*b-c.d_e");
    expect_script_string("return URL.encodeComponent('100%');", "100%25");
}

TEST(UrlFuncsTest, EncodeComponentEmpty) { expect_script_string("return URL.encodeComponent('');", ""); }

TEST(UrlFuncsTest, EncodeComponentEncodesNonAsciiAsUppercaseHex) {
    expect_script_string("return URL.encodeComponent('\xc3\xa9');", "%C3%A9");
}

TEST(UrlFuncsTest, EncodeComponentNonStringThrows) {
    expect_script_string(R"(try { URL.encodeComponent(123); return "no"; } catch (e) { return "caught"; })", "caught");
}

// ---- URL.decodeComponent ----

TEST(UrlFuncsTest, DecodeComponentRoundTrip) {
    expect_script_bool("return URL.decodeComponent(URL.encodeComponent('a b')) === 'a b';", true);
    expect_script_string("return URL.decodeComponent('a+b');", "a b");
    expect_script_string("return URL.decodeComponent('%C3%A9');", "\xc3\xa9");
    expect_script_string("return URL.decodeComponent('100%25');", "100%");
    expect_script_string("return URL.decodeComponent('');", "");
}

TEST(UrlFuncsTest, DecodeComponentMalformedEscapeThrows) {
    expect_script_string(R"(try { URL.decodeComponent('%zz'); return "no"; } catch (e) { return "caught"; })",
                         "caught");
    expect_script_string(R"(try { URL.decodeComponent('%1'); return "no"; } catch (e) { return "caught"; })", "caught");
    expect_script_string(R"(try { URL.decodeComponent('%'); return "no"; } catch (e) { return "caught"; })", "caught");
}

// Every allocation on the UTF-8 repair path must reach the host function's oom_guard: a
// noexcept helper under the guard terminates the process instead of aborting the script.
TEST(UrlFuncsTest, DecodeComponentRepairOomAbortsScript) {
    if constexpr (!fiber::test::kAllocFailureAvailable) {
        GTEST_SKIP() << "no operator new replacement in this build";
    }
    std::string source = "return URL.decodeComponent('";
    for (int i = 0; i < 64; ++i) {
        source += "%FF";
    }
    source += "');";
    auto compiled = fiber::script::compile_script(StdLibrary::instance(), source);
    ASSERT_TRUE(compiled.has_value());

    std::size_t aborts = 0;
    for (std::size_t successes = 0;; ++successes) {
        GcHeap heap;
        JsValue root = JsValue::make_undefined();
        ScriptResult result;
        {
            fiber::test::ScopedAllocFailure out_of_memory(successes);
            result = compiled->exec_sync(root, nullptr, heap);
        }
        if (result.is_success()) {
            break;
        }
        ASSERT_TRUE(result.is_abort());
        EXPECT_EQ(result.abort().reason, ScriptAbortReason::OutOfMemory);
        ++aborts;
    }
    EXPECT_GE(aborts, 2U); // the decode buffer and the repair buffer each failed once
}

TEST(UrlFuncsTest, DecodeComponentRepairsMalformedUtf8) {
    expect_script_string("return URL.decodeComponent('a%FFb');", "a\xef\xbf\xbd"
                                                                 "b");
}

// ---- URL.parseQuery ----

TEST(UrlFuncsTest, ParseQueryRepairsMalformedUtf8) {
    expect_script_string("return URL.parseQuery('a=%FF').a;", "\xef\xbf\xbd");
    expect_script_string("return URL.parseQuery('%C3=1')['\xef\xbf\xbd'];", "1");
}

// parseQuery has no oom_guard: its decode buffers come from a pool, so nothing may throw.
TEST(UrlFuncsTest, ParseQueryMakesNoThrowingAllocation) {
    if constexpr (!fiber::test::kAllocFailureAvailable) {
        GTEST_SKIP() << "no operator new replacement in this build";
    }
    auto compiled = fiber::script::compile_script(StdLibrary::instance(), "return URL.parseQuery('a=%FF&%FE=b').a;");
    ASSERT_TRUE(compiled.has_value());
    GcHeap heap;
    JsValue root = JsValue::make_undefined();
    ScriptResult result;
    {
        // An allocation here would throw through the noexcept host function and terminate.
        fiber::test::ScopedAllocFailure no_allocations(0);
        result = compiled->exec_sync(root, nullptr, heap);
    }
    ASSERT_TRUE(result.is_success());
    EXPECT_EQ(string_to_utf8(result.value()), "\xef\xbf\xbd");
}

TEST(UrlFuncsTest, ParseQueryRepeatedKeyAggregates) {
    expect_script_bool("let q = URL.parseQuery('a=1&a=2&b=x'); return q.a[0] === '1' && q.a[1] === '2' && q.b === 'x';",
                       true);
}

TEST(UrlFuncsTest, ParseQueryEmptyObject) {
    expect_script_bool("return length(URL.parseQuery('')) === 0;", true);
    expect_script_bool("return length(Object.keys(URL.parseQuery(''))) === 0;", true);
}

TEST(UrlFuncsTest, ParseQueryNoValueKeyIsEmptyString) {
    expect_script_bool("let q = URL.parseQuery('a'); return length(q) === 1 && q.a === '';", true);
}

TEST(UrlFuncsTest, ParseQueryEqualsInValue) {
    expect_script_bool("let q = URL.parseQuery('a=b=c'); return q.a === 'b=c';", true);
}

TEST(UrlFuncsTest, ParseQueryEmptySegmentsSkipped) {
    expect_script_bool("let q = URL.parseQuery('a=1&&b=2'); return length(q) === 2 && q.a === '1' && q.b === '2';",
                       true);
}

TEST(UrlFuncsTest, ParseQueryDecodesKeysAndValues) {
    expect_script_bool("let q = URL.parseQuery('%C3%A9=x&k=a+b'); return q['\xc3\xa9'] === 'x' && q.k === 'a b';",
                       true);
}

TEST(UrlFuncsTest, ParseQueryMalformedEscapeThrows) {
    expect_script_string(R"(try { URL.parseQuery('%zz=1'); return "no"; } catch (e) { return "caught"; })", "caught");
}

// ---- URL.buildQuery ----

TEST(UrlFuncsTest, BuildQueryBasic) { expect_script_string("return URL.buildQuery({a: 1, b: 'x'});", "a=1&b=x"); }

TEST(UrlFuncsTest, BuildQueryEmpty) { expect_script_string("return URL.buildQuery({});", ""); }

TEST(UrlFuncsTest, BuildQueryNullPassthrough) { expect_script_null("return URL.buildQuery(null);"); }

TEST(UrlFuncsTest, BuildQueryUndefinedPassthrough) {
    // buildQuery() with no arg uses the undefined default, which is returned as-is.
    GcHeap heap;
    auto result = run_script("return URL.buildQuery();", heap);
    ASSERT_TRUE(result.is_success());
    EXPECT_EQ(js_value_type(result.value()), JsNodeType::Undefined);
}

TEST(UrlFuncsTest, BuildQueryArrayValuesExpand) {
    expect_script_string("return URL.buildQuery({a: [1, 2], b: 'x'});", "a=1&a=2&b=x");
}

TEST(UrlFuncsTest, BuildQueryEncodesKeysAndValues) {
    expect_script_string("return URL.buildQuery({'a b': 'c d'});", "a+b=c+d");
}

TEST(UrlFuncsTest, BuildQueryNullValueRendersAsNull) {
    expect_script_string("return URL.buildQuery({a: null});", "a=null");
}

TEST(UrlFuncsTest, BuildQueryNonObjectThrows) {
    expect_script_string(R"(try { URL.buildQuery(123); return "no"; } catch (e) { return "caught"; })", "caught");
    expect_script_string(R"(try { URL.buildQuery('x'); return "no"; } catch (e) { return "caught"; })", "caught");
}
