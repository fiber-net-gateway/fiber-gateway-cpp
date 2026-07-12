#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "script/JsGc.h"
#include "script/ScriptCompiler.h"
#include "script/gc/GcInternal.h"
#include "script/std/StdLibrary.h"

using fiber::script::AbiResult;
using fiber::script::GcHeap;
using fiber::script::GcString;
using fiber::script::JsNodeType;
using fiber::script::JsValue;
using fiber::script::ScriptAbortReason;
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

AbiResult run_script(std::string_view source, GcHeap &heap) {
    auto compiled = fiber::script::compile_script(StdLibrary::instance(), source);
    EXPECT_TRUE(compiled.has_value()) << (compiled ? "" : compiled.error().message);
    if (!compiled) {
        return AbiResult::abort(ScriptAbortReason::Internal);
    }
    JsValue root = JsValue::make_undefined();
    return compiled->exec_sync(root, nullptr, heap);
}

void expect_script_bool(std::string_view source, bool expected) {
    GcHeap heap;
    AbiResult result = run_script(source, heap);
    ASSERT_TRUE(result.is_success()) << "script did not succeed";
    ASSERT_EQ(js_value_type(result.value()), JsNodeType::Boolean);
    EXPECT_EQ(fiber::script::js_value_bool(result.value()), expected);
}

void expect_script_int(std::string_view source, std::int64_t expected) {
    GcHeap heap;
    AbiResult result = run_script(source, heap);
    ASSERT_TRUE(result.is_success()) << "script did not succeed";
    ASSERT_EQ(js_value_type(result.value()), JsNodeType::Integer);
    EXPECT_EQ(fiber::script::js_value_int64(result.value()), expected);
}

void expect_script_string(std::string_view source, std::string_view expected) {
    GcHeap heap;
    AbiResult result = run_script(source, heap);
    ASSERT_TRUE(result.is_success()) << "script did not succeed";
    ASSERT_EQ(js_value_type(result.value()), JsNodeType::String);
    EXPECT_EQ(string_to_utf8(result.value()), expected);
}

void expect_script_null(std::string_view source) {
    GcHeap heap;
    AbiResult result = run_script(source, heap);
    ASSERT_TRUE(result.is_success()) << "script did not succeed";
    EXPECT_EQ(js_value_type(result.value()), JsNodeType::Null);
}

// U+1D11E (MUSICAL SYMBOL G CLEF): a 4-byte UTF-8 sequence (F0 9D 84 9E) that
// occupies two UTF-16 code units, used to exercise the UTF-16 index model.
const std::string kSupp = "\xF0\x9D\x84\x9E"; // "𝄞"
const std::string kSuppText = std::string("a") + kSupp + "b"; // "a𝄞b" (u16 len 4)

std::string quote(const std::string &s) { return "\"" + s + "\""; }

} // namespace

// ---- Java StdTest oracles (non-regex) ----

TEST(StringsFuncsTest, HasPrefix) {
    expect_script_bool(R"(return strings.hasPrefix("abcdedf", "abc") === true;)", true);
    expect_script_bool(R"(return strings.hasPrefix("abcdedf", "xyz") === false;)", true);
}

TEST(StringsFuncsTest, HasSuffix) {
    expect_script_bool(R"(return strings.hasSuffix("abcdedf", "edf") === true;)", true);
    expect_script_bool(R"(return strings.hasSuffix("abcdedf", "abc") === false;)", true);
}

TEST(StringsFuncsTest, ToLower) {
    expect_script_bool(R"(return strings.toLower("abc123Abc") === "abc123abc";)", true);
    expect_script_string(R"(return strings.toLower("ABC");)", "abc");
}

TEST(StringsFuncsTest, ToUpper) {
    expect_script_bool(R"(return strings.toUpper("abc123Abc") === "ABC123ABC";)", true);
    expect_script_string(R"(return strings.toUpper("abc");)", "ABC");
}

TEST(StringsFuncsTest, Trim) {
    expect_script_bool(R"(return strings.trim(" \tabc \t ") === "abc";)", true);
    expect_script_bool(R"(return strings.trim("aaabc a", "a") === "bc ";)", true);
}

TEST(StringsFuncsTest, TrimLeft) {
    expect_script_bool(R"(return strings.trimLeft(" bc a ") === "bc a ";)", true);
    expect_script_bool(R"(return strings.trimLeft("aa bc a", "a") === " bc a";)", true);
}

TEST(StringsFuncsTest, TrimRight) {
    expect_script_bool(R"(return strings.trimRight(" bc a ") === " bc a";)", true);
    expect_script_bool(R"(return strings.trimRight(" bc a aa", "a") === " bc a ";)", true);
}

TEST(StringsFuncsTest, Split) {
    expect_script_bool(R"(let arr = strings.split("abcecdf", "c"); )"
                       R"(return length(arr) === 3 && arr[0] === "ab" && arr[1] === "e" && arr[2] === "df";)",
                       true);
}

TEST(StringsFuncsTest, Contains) {
    expect_script_bool(R"(return strings.contains("abcd-effe-ssf-fd", "e-ssf") === true;)", true);
    expect_script_bool(R"(return strings.contains("abcd-effe-ssf-fd", "zzz") === false;)", true);
}

TEST(StringsFuncsTest, ContainsAny) {
    expect_script_bool(R"(return strings.contains_any("abcd-effe-ssf-fd", "ccddeezzz") === true;)", true);
    expect_script_bool(R"(return strings.contains_any("abcd", "xyz") === false;)", true);
}

TEST(StringsFuncsTest, Index) {
    expect_script_bool(R"(return strings.index("aabbcc", "bcc") === 3;)", true);
    expect_script_int(R"(return strings.index("aabbcc", "bcc");)", 3);
}

TEST(StringsFuncsTest, IndexAny) { expect_script_bool(R"(return strings.indexAny("acsdfds", "rss") === 2;)", true); }

TEST(StringsFuncsTest, LastIndex) { expect_script_bool(R"(return strings.lastIndex("cabcd", "c") === 3;)", true); }

TEST(StringsFuncsTest, LastIndexAny) {
    expect_script_bool(R"(return strings.lastIndexAny("cabcd", "dcz") === 4;)", true);
    expect_script_bool(R"(return strings.lastIndexAny("abc", "xyz") === -1;)", true);
}

TEST(StringsFuncsTest, Repeat) { expect_script_bool(R"(return strings.repeat("acd", 3) === "acdacdacd";)", true); }

TEST(StringsFuncsTest, Substring) {
    expect_script_bool(R"(return strings.substring("0123456789", 3) === "3456789" )"
                       R"(&& strings.substring("0123456789", 3, 6) === "345";)",
                       true);
}

TEST(StringsFuncsTest, ToString) {
    expect_script_bool(R"(return strings.toString(null) === "null" )"
                       R"(&& strings.toString({}) === "<ObjectNode>" && strings.toString(3.5) === "3.5";)",
                       true);
}

// ---- edge cases ----

TEST(StringsFuncsTest, SplitNoSeparatorReturnsSingleElement) {
    expect_script_bool(R"(let a = strings.split("abc"); return length(a) === 1 && a[0] === "abc";)", true);
}

TEST(StringsFuncsTest, SplitEmptyTextIsEmptyArray) {
    expect_script_bool(R"(return length(strings.split("", ",")) === 0;)", true);
}

TEST(StringsFuncsTest, SplitSeparatorIsCodepointSet) {
    // The separator is a SET of code points: ',' and ';' both split, adjacent
    // separators collapse (no empty tokens), trailing separator yields no token.
    expect_script_bool(R"(let a = strings.split("a,b;c,", ",;"); )"
                       R"(return length(a) === 3 && a[0] === "a" && a[1] === "b" && a[2] === "c";)",
                       true);
    expect_script_bool(R"(let a = strings.split("a--b", "-"); )"
                       R"(return length(a) === 2 && a[0] === "a" && a[1] === "b";)",
                       true);
}

TEST(StringsFuncsTest, NonTextInputSoftFailure) {
    // hasPrefix returns false; toLower/index return null (Java parity).
    expect_script_bool(R"(return strings.hasPrefix(123, "1") === false;)", true);
    expect_script_bool(R"(return strings.hasSuffix(123, "1") === false;)", true);
    expect_script_null(R"(return strings.toLower(123);)");
    expect_script_null(R"(return strings.toUpper(123);)");
    expect_script_null(R"(return strings.trim(123);)");
    expect_script_null(R"(return strings.split(123, ",");)");
    expect_script_null(R"(return strings.index(123, "x");)");
    expect_script_null(R"(return strings.contains(123, "x");)");
    expect_script_null(R"(return strings.substring(123, 0);)");
}

TEST(StringsFuncsTest, EmptyNeedle) {
    expect_script_bool(R"(return strings.contains("abc", "") === true;)", true);
    expect_script_int(R"(return strings.index("abc", "");)", 0);
    expect_script_int(R"(return strings.lastIndex("abc", "");)", 3);
    expect_script_bool(R"(return strings.hasPrefix("abc", "") === true;)", true);
}

TEST(StringsFuncsTest, IndexNotFound) {
    expect_script_int(R"(return strings.index("abc", "z");)", -1);
    expect_script_int(R"(return strings.lastIndex("abc", "z");)", -1);
    expect_script_int(R"(return strings.indexAny("abc", "xyz");)", -1);
}

TEST(StringsFuncsTest, RepeatEdges) {
    expect_script_string(R"(return strings.repeat("abc", 0);)", "");
    expect_script_string(R"(return strings.repeat("abc", 1);)", "abc");
    expect_script_string(R"(return strings.repeat("", 5);)", "");
    expect_script_null(R"(return strings.repeat("abc", -1);)");
    expect_script_null(R"(return strings.repeat("abc", "x");)");
}

TEST(StringsFuncsTest, SubstringEdges) {
    expect_script_string(R"(return strings.substring("abc", 10);)", "");
    expect_script_string(R"(return strings.substring("abc", 2, 2);)", "");
    expect_script_string(R"(return strings.substring("abc", -1, 2);)", "ab"); // start clamped to 0
    expect_script_string(R"(return strings.substring("abc", 0);)", "abc"); // j>=len, i==0 -> original
    expect_script_string(R"(return strings.substring("abc", 1);)", "bc");
}

TEST(StringsFuncsTest, ToStringVariants) {
    expect_script_string(R"(return strings.toString();)", "");
    expect_script_string(R"(return strings.toString(42);)", "42");
    expect_script_string(R"(return strings.toString(true);)", "true");
    expect_script_string(R"(return strings.toString(false);)", "false");
    expect_script_string(R"(return strings.toString([1, 2]);)", "<ArrayNode>");
}

// ---- UTF-16 index model (supplementary code points) ----

TEST(StringsFuncsTest, Utf16LengthAndIndex) {
    // "a𝄞b": a(1) + 𝄞(2 u16 units) + b(1) = 4 UTF-16 units.
    expect_script_int("return length(" + quote(kSuppText) + ");", 4);
    // index of "𝄞" is the UTF-16 index of its high surrogate = 1.
    expect_script_int("return strings.index(" + quote(kSuppText) + ", " + quote(kSupp) + ");", 1);
}

TEST(StringsFuncsTest, Utf16Substring) {
    expect_script_string("return strings.substring(" + quote(kSuppText) + ", 0, 1);", "a");
    expect_script_string("return strings.substring(" + quote(kSuppText) + ", 1, 3);", kSupp);
    expect_script_string("return strings.substring(" + quote(kSuppText) + ", 0, 3);", std::string("a") + kSupp);
    expect_script_string("return strings.substring(" + quote(kSuppText) + ", 3);", "b");
}

TEST(StringsFuncsTest, Utf16IndexAny) {
    // 'b' sits at UTF-16 index 3 (after the 2-unit supplementary).
    expect_script_int("return strings.indexAny(" + quote(kSuppText) + ", \"b\");", 3);
    expect_script_int("return strings.lastIndexAny(" + quote(kSuppText) + ", \"b\");", 3);
}
