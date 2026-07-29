#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "ScriptTestHelpers.h"
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
    expect_script_string("try { URL.encodeComponent(123); return \"no\"; } catch (e) { return \"caught\"; }", "caught");
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
    expect_script_string("try { URL.decodeComponent('%zz'); return \"no\"; } catch (e) { return \"caught\"; }",
                         "caught");
    expect_script_string("try { URL.decodeComponent('%1'); return \"no\"; } catch (e) { return \"caught\"; }",
                         "caught");
    expect_script_string("try { URL.decodeComponent('%'); return \"no\"; } catch (e) { return \"caught\"; }", "caught");
}

// ---- URL.parseQuery ----

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
    expect_script_string("try { URL.parseQuery('%zz=1'); return \"no\"; } catch (e) { return \"caught\"; }", "caught");
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
    expect_script_string("try { URL.buildQuery(123); return \"no\"; } catch (e) { return \"caught\"; }", "caught");
    expect_script_string("try { URL.buildQuery('x'); return \"no\"; } catch (e) { return \"caught\"; }", "caught");
}
