#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include <fiber/script/ScriptCompiler.h>
#include <fiber/script/gc/GcInternal.h>
#include <fiber/script/std/StdLibrary.h>
#include "ScriptTestHelpers.h"

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

void expect_script_int(std::string_view source, std::int64_t expected) {
    GcHeap heap;
    auto result = run_script(source, heap);
    ASSERT_TRUE(result.is_success()) << "script did not succeed";
    ASSERT_EQ(js_value_type(result.value()), JsNodeType::Integer);
    EXPECT_EQ(fiber::script::js_value_int64(result.value()), expected);
}

} // namespace

TEST(ObjectsFuncsTest, AssignMergesSources) {
    expect_script_string("let t = {a: 1}; Object.assign(t, {b: 2}, {c: 3}); return array.join(Object.keys(t), \",\");",
                         "a,b,c");
}

TEST(ObjectsFuncsTest, AssignOverwritesInPlace) {
    // b is overwritten in place (keeps its position); c is appended.
    expect_script_string(
            "let t = {a: 1, b: 2}; Object.assign(t, {b: 3, c: 4}); return array.join(Object.keys(t), \",\");", "a,b,c");
    expect_script_int("let t = {a: 1, b: 2}; Object.assign(t, {b: 3, c: 4}); return t.b;", 3);
}

TEST(ObjectsFuncsTest, AssignReturnsTarget) {
    expect_script_string("let t = {a: 1}; let r = Object.assign(t, {b: 2}); return array.join(Object.keys(r), \",\");",
                         "a,b");
}

TEST(ObjectsFuncsTest, AssignSkipsNonObjectSources) {
    // Only the {b: 2} source contributes; integer, string, and null sources are skipped.
    expect_script_string(
            "let t = {a: 1}; Object.assign(t, 123, \"x\", {b: 2}, null); return array.join(Object.keys(t), \",\");",
            "a,b");
}

TEST(ObjectsFuncsTest, KeysInInsertionOrder) {
    expect_script_string("return array.join(Object.keys({b: 2, a: 1, c: 3}), \",\");", "b,a,c");
}

TEST(ObjectsFuncsTest, KeysEmptyObject) { expect_script_string("return array.join(Object.keys({}), \",\");", ""); }

TEST(ObjectsFuncsTest, ValuesInInsertionOrder) {
    expect_script_string("return array.join(Object.values({a: 10, b: 20}), \",\");", "10,20");
}

TEST(ObjectsFuncsTest, ValuesIndexableAndSum) {
    expect_script_int("let v = Object.values({a: 5, b: 7}); return v[0] + v[1];", 12);
}

TEST(ObjectsFuncsTest, ValuesPreserveScalarTextForm) {
    expect_script_string("return array.join(Object.values({a: true, b: \"x\"}), \",\");", "true,x");
}

TEST(ObjectsFuncsTest, DeletePropertiesRemovesKey) {
    expect_script_string(
            "let t = {a: 1, b: 2, c: 3}; Object.deleteProperties(t, \"b\"); return array.join(Object.keys(t), \",\");",
            "a,c");
}

TEST(ObjectsFuncsTest, DeletePropertiesVariadic) {
    expect_script_string("let t = {a: 1, b: 2, c: 3, d: 4}; Object.deleteProperties(t, \"a\", \"c\"); return "
                         "array.join(Object.keys(t), \",\");",
                         "b,d");
}

TEST(ObjectsFuncsTest, DeletePropertiesSkipsNonStringKeys) {
    // 123 is not textual -> skipped; "a" is removed.
    expect_script_string(
            "let t = {a: 1, b: 2}; Object.deleteProperties(t, 123, \"a\"); return array.join(Object.keys(t), \",\");",
            "b");
}

TEST(ObjectsFuncsTest, DeletePropertiesMissingKeyIsNoOp) {
    expect_script_string(
            "let t = {a: 1, b: 2}; Object.deleteProperties(t, \"zzz\"); return array.join(Object.keys(t), \",\");",
            "a,b");
}

TEST(ObjectsFuncsTest, DeletePropertiesReturnsTarget) {
    expect_script_string("let t = {a: 1, b: 2}; let r = Object.deleteProperties(t, \"a\"); return "
                         "array.join(Object.keys(r), \",\");",
                         "b");
}

TEST(ObjectsFuncsTest, NonObjectTargetRaisesCatchableTypeError) {
    expect_script_string("try { Object.assign(123, {a: 1}); return \"no-throw\"; } catch (e) { return \"caught\"; }",
                         "caught");
    expect_script_string("try { Object.keys(123); return \"no-throw\"; } catch (e) { return \"caught\"; }", "caught");
    expect_script_string("try { Object.values(\"x\"); return \"no-throw\"; } catch (e) { return \"caught\"; }",
                         "caught");
    expect_script_string(
            "try { Object.deleteProperties(null, \"a\"); return \"no-throw\"; } catch (e) { return \"caught\"; }",
            "caught");
}
