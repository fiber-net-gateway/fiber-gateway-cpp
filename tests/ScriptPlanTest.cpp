#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/json/JsGc.h"
#include "script/Library.h"
#include "script/Runtime.h"
#include "script/Script.h"
#include "script/ir/Compiler.h"
#include "script/parse/Parser.h"
#include "script/std/StdLibrary.h"

namespace {

using fiber::json::GcArray;
using fiber::json::GcBinary;
using fiber::json::GcObject;
using fiber::json::GcObjectEntry;
using fiber::json::GcString;
using fiber::json::JsNodeType;
using fiber::json::JsValue;
using FunctionResult = fiber::script::Library::FunctionResult;

using ScriptResult = fiber::script::ScriptSyncRun::Result;

std::string value_to_string(const JsValue &value) {
    if (value.type_ == JsNodeType::NativeString) {
        return std::string(value.ns.data, value.ns.len);
    }
    if (value.type_ == JsNodeType::HeapString) {
        std::string out;
        auto *str = reinterpret_cast<const GcString *>(value.gc);
        if (fiber::json::gc_string_to_utf8(str, out)) {
            return out;
        }
    }
    return {};
}

bool value_to_number(const JsValue &value, double &out) {
    if (value.type_ == JsNodeType::Integer) {
        out = static_cast<double>(value.i);
        return true;
    }
    if (value.type_ == JsNodeType::Float) {
        out = value.f;
        return true;
    }
    return false;
}

bool is_string_type(const JsValue &value) {
    return value.type_ == JsNodeType::NativeString || value.type_ == JsNodeType::HeapString;
}

bool is_binary_type(const JsValue &value) {
    return value.type_ == JsNodeType::NativeBinary || value.type_ == JsNodeType::HeapBinary;
}

const JsValue *object_value(const JsValue &obj, std::string_view key) {
    if (obj.type_ != JsNodeType::Object) {
        return nullptr;
    }
    auto *obj_ptr = reinterpret_cast<const GcObject *>(obj.gc);
    if (!obj_ptr) {
        return nullptr;
    }
    for (std::size_t i = 0; i < obj_ptr->size; ++i) {
        const GcObjectEntry *entry = fiber::json::gc_object_entry_at(obj_ptr, i);
        if (!entry || !entry->occupied || !entry->key) {
            continue;
        }
        std::string entry_key;
        if (!fiber::json::gc_string_to_utf8(entry->key, entry_key)) {
            continue;
        }
        if (entry_key == key) {
            return &entry->value;
        }
    }
    return nullptr;
}

const JsValue &object_value_or_default(const JsValue &obj, std::string_view key) {
    const JsValue *value = object_value(obj, key);
    if (!value) {
        ADD_FAILURE() << "missing object key: " << key;
        static JsValue missing = JsValue::make_undefined();
        return missing;
    }
    return *value;
}

const JsValue *array_value(const JsValue &arr, std::size_t index) {
    if (arr.type_ != JsNodeType::Array) {
        return nullptr;
    }
    auto *arr_ptr = reinterpret_cast<const GcArray *>(arr.gc);
    if (!arr_ptr) {
        return nullptr;
    }
    return fiber::json::gc_array_get(arr_ptr, index);
}

const JsValue &array_value_or_default(const JsValue &arr, std::size_t index) {
    const JsValue *value = array_value(arr, index);
    if (!value) {
        ADD_FAILURE() << "missing array index: " << index;
        static JsValue missing = JsValue::make_undefined();
        return missing;
    }
    return *value;
}

JsValue make_heap_string(fiber::script::ScriptRuntime &runtime, std::string_view text) {
    GcString *str = runtime.alloc_with_gc(text.size(), [&]() {
        return fiber::json::gc_new_string(&runtime.heap(), text.data(), text.size());
    });
    if (!str) {
        return JsValue::make_undefined();
    }
    JsValue value;
    value.type_ = JsNodeType::HeapString;
    value.gc = &str->hdr;
    return value;
}

class AddFunc final : public fiber::script::Library::Function {
public:
    FunctionResult call(fiber::script::ExecutionContext &context) override {
        double sum = 0.0;
        bool any_float = false;
        for (std::size_t i = 0; i < context.arg_count(); ++i) {
            const JsValue &arg = context.arg_value(i);
            if (arg.type_ == JsNodeType::Integer) {
                sum += static_cast<double>(arg.i);
                continue;
            }
            if (arg.type_ == JsNodeType::Float) {
                sum += arg.f;
                any_float = true;
                continue;
            }
            static char msg[] = "add arg must be number";
            return std::unexpected(JsValue::make_native_string(msg, sizeof(msg) - 1));
        }
        if (any_float) {
            return JsValue::make_float(sum);
        }
        return JsValue::make_integer(static_cast<std::int64_t>(sum));
    }
};

class ReqReadBinaryFunc final : public fiber::script::Library::Function {
public:
    FunctionResult call(fiber::script::ExecutionContext &context) override {
        (void)context;
        static std::uint8_t data[] = {0x01, 0x02, 0x03};
        return JsValue::make_native_binary(data, sizeof(data));
    }
};

class DemoCreateUserFunc final : public fiber::script::Library::Function {
public:
    FunctionResult call(fiber::script::ExecutionContext &context) override {
        std::string arg;
        if (context.arg_count() > 0) {
            arg = value_to_string(context.arg_value(0));
        }
        std::string out = "user:";
        out += arg;
        JsValue value = make_heap_string(context.runtime(), out);
        if (value.type_ == JsNodeType::Undefined) {
            static char msg[] = "out of memory";
            return std::unexpected(JsValue::make_native_string(msg, sizeof(msg) - 1));
        }
        return value;
    }
};

class RandRandomStub final : public fiber::script::Library::Function {
public:
    FunctionResult call(fiber::script::ExecutionContext &context) override {
        (void)context;
        return JsValue::make_integer(7);
    }
};

class RandCanaryStub final : public fiber::script::Library::Function {
public:
    FunctionResult call(fiber::script::ExecutionContext &context) override {
        (void)context;
        return JsValue::make_integer(42);
    }
};

class TimeFormatStub final : public fiber::script::Library::Function {
public:
    FunctionResult call(fiber::script::ExecutionContext &context) override {
        (void)context;
        JsValue value = make_heap_string(context.runtime(), "2023-11-14");
        if (value.type_ == JsNodeType::Undefined) {
            static char msg[] = "out of memory";
            return std::unexpected(JsValue::make_native_string(msg, sizeof(msg) - 1));
        }
        return value;
    }
};

class UrlAliasFunc final : public fiber::script::Library::Function {
public:
    explicit UrlAliasFunc(std::string target) : target_(std::move(target)) {}

    FunctionResult call(fiber::script::ExecutionContext &context) override {
        auto *func = fiber::script::std_lib::StdLibrary::instance().find_func(target_);
        if (!func) {
            static char msg[] = "url function not found";
            return std::unexpected(JsValue::make_native_string(msg, sizeof(msg) - 1));
        }
        return func->call(context);
    }

private:
    std::string target_;
};

class DemoServiceDirective final : public fiber::script::Library::DirectiveDef {
public:
    explicit DemoServiceDirective(fiber::script::Library::Function *create_user)
        : create_user_(create_user) {}

    fiber::script::Library::Function *find_func(std::string_view directive,
                                                std::string_view function) override {
        if (directive == "demoService" && function == "createUser") {
            return create_user_;
        }
        return nullptr;
    }

    fiber::script::Library::AsyncFunction *find_async_func(std::string_view directive,
                                                           std::string_view function) override {
        (void)directive;
        (void)function;
        return nullptr;
    }

private:
    fiber::script::Library::Function *create_user_ = nullptr;
};

class StubLibrary final : public fiber::script::Library {
public:
    explicit StubLibrary(fiber::script::Library &fallback)
        : fallback_(fallback),
          url_parse_query_("URL.parseQuery"),
          url_build_query_("URL.buildQuery"),
          url_encode_component_("URL.encodeComponent"),
          url_decode_component_("URL.decodeComponent"),
          directive_(&demo_create_user_) {
        register_func("add", &add_);
        register_func("req.readBinary", &read_binary_);
        register_func("rand.random", &rand_random_);
        register_func("rand.canary", &rand_canary_);
        register_func("time.format", &time_format_);
        register_func("url.parseQuery", &url_parse_query_);
        register_func("url.buildQuery", &url_build_query_);
        register_func("url.encodeComponent", &url_encode_component_);
        register_func("url.decodeComponent", &url_decode_component_);
    }

    void mark_root_prop(std::string_view prop_name) override {
        fallback_.mark_root_prop(prop_name);
    }

    Function *find_func(std::string_view name) override {
        auto it = functions_.find(std::string(name));
        if (it != functions_.end()) {
            return it->second;
        }
        return fallback_.find_func(name);
    }

    AsyncFunction *find_async_func(std::string_view name) override {
        auto it = async_functions_.find(std::string(name));
        if (it != async_functions_.end()) {
            return it->second;
        }
        return fallback_.find_async_func(name);
    }

    Constant *find_constant(std::string_view namespace_name, std::string_view key) override {
        std::string full(namespace_name);
        full.append(".");
        full.append(key);
        auto it = constants_.find(full);
        if (it != constants_.end()) {
            return it->second;
        }
        return fallback_.find_constant(namespace_name, key);
    }

    AsyncConstant *find_async_constant(std::string_view namespace_name, std::string_view key) override {
        std::string full(namespace_name);
        full.append(".");
        full.append(key);
        auto it = async_constants_.find(full);
        if (it != async_constants_.end()) {
            return it->second;
        }
        return fallback_.find_async_constant(namespace_name, key);
    }

    DirectiveDef *find_directive_def(std::string_view type,
                                     std::string_view name,
                                     const std::vector<fiber::json::JsValue> &literals) override {
        (void)literals;
        if (type == "dubbo" && name == "demoService") {
            return &directive_;
        }
        return fallback_.find_directive_def(type, name, literals);
    }

    void register_func(std::string name, Function *func) {
        functions_.emplace(std::move(name), func);
    }

private:
    fiber::script::Library &fallback_;
    std::unordered_map<std::string, Function *> functions_;
    std::unordered_map<std::string, AsyncFunction *> async_functions_;
    std::unordered_map<std::string, Constant *> constants_;
    std::unordered_map<std::string, AsyncConstant *> async_constants_;

    AddFunc add_;
    ReqReadBinaryFunc read_binary_;
    DemoCreateUserFunc demo_create_user_;
    RandRandomStub rand_random_;
    RandCanaryStub rand_canary_;
    TimeFormatStub time_format_;
    UrlAliasFunc url_parse_query_;
    UrlAliasFunc url_build_query_;
    UrlAliasFunc url_encode_component_;
    UrlAliasFunc url_decode_component_;
    DemoServiceDirective directive_;
};

bool compile_script(std::string_view script, fiber::script::Library &library, fiber::script::ir::Compiled &out) {
    fiber::script::parse::Parser parser(library, true);
    auto parsed = parser.parse_script(script);
    if (!parsed) {
        ADD_FAILURE() << parsed.error().message;
        return false;
    }
    out = fiber::script::ir::Compiler::compile(*parsed.value());
    return true;
}

ScriptResult run_script(std::string_view script,
                        fiber::script::Library &library,
                        fiber::script::ScriptRuntime &runtime) {
    fiber::script::ir::Compiled compiled;
    if (!compile_script(script, library, compiled)) {
        return std::unexpected(JsValue::make_undefined());
    }
    auto compiled_ptr = std::make_shared<fiber::script::ir::Compiled>(std::move(compiled));
    fiber::script::Script script_obj(compiled_ptr);
    auto run = script_obj.exec_sync(JsValue::make_undefined(), nullptr, runtime);
    return run();
}

struct TestEnv {
    fiber::json::GcHeap heap;
    fiber::json::GcRootSet roots;
    fiber::script::ScriptRuntime runtime;
    StubLibrary library;

    TestEnv()
        : runtime(heap, roots),
          library(fiber::script::std_lib::StdLibrary::instance()) {}
};

} // namespace

TEST(ScriptPlanTest, LiteralsAndTypeof) {
    TestEnv env;
    auto result = run_script(
        "let num = 1;\n"
        "let txt = \"this is string\";\n"
        "let bin = req.readBinary();\n"
        "let boo = true;\n"
        "let nul = null;\n"
        "let obj = {n:num};\n"
        "let mis = obj.cc;\n"
        "let arr = [1,2,num];\n"
        "let result = {num, txt, bin, nul, obj, boo, mis, arr};\n"
        "let types = {};\n"
        "for (let k, v of result) { types[k] = typeof v; }\n"
        "return {types, result};\n",
        env.library,
        env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(value.type_, JsNodeType::Object);

    const JsValue *types = object_value(value, "types");
    ASSERT_NE(types, nullptr);
    ASSERT_EQ(types->type_, JsNodeType::Object);
    const JsValue *res = object_value(value, "result");
    ASSERT_NE(res, nullptr);
    ASSERT_EQ(res->type_, JsNodeType::Object);

    EXPECT_EQ(value_to_string(object_value_or_default(*types, "num")), "number");
    EXPECT_EQ(value_to_string(object_value_or_default(*types, "txt")), "string");
    EXPECT_EQ(value_to_string(object_value_or_default(*types, "bin")), "binary");
    EXPECT_EQ(value_to_string(object_value_or_default(*types, "nul")), "null");
    EXPECT_EQ(value_to_string(object_value_or_default(*types, "obj")), "object");
    EXPECT_EQ(value_to_string(object_value_or_default(*types, "boo")), "boolean");
    EXPECT_EQ(value_to_string(object_value_or_default(*types, "mis")), "undefined");
    EXPECT_EQ(value_to_string(object_value_or_default(*types, "arr")), "array");

    const JsValue *bin = object_value(*res, "bin");
    ASSERT_NE(bin, nullptr);
    EXPECT_TRUE(is_binary_type(*bin));

    const JsValue *mis = object_value(*res, "mis");
    ASSERT_NE(mis, nullptr);
    EXPECT_EQ(mis->type_, JsNodeType::Undefined);
}

TEST(ScriptPlanTest, ArithmeticPrecedence) {
    TestEnv env;
    auto result = run_script("return 1 + 2 * 3 - 4 / 2 + (5 % 2);", env.library, env.runtime);
    ASSERT_TRUE(result.has_value());
    double number = 0.0;
    ASSERT_TRUE(value_to_number(result.value(), number));
    EXPECT_EQ(number, 6.0);
}

TEST(ScriptPlanTest, StringConcat) {
    TestEnv env;
    auto result = run_script("return strings.toString(1) + \"a\" + strings.toString(2);",
                             env.library,
                             env.runtime);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(value_to_string(result.value()), "1a2");
}

TEST(ScriptPlanTest, LogicalShortCircuit) {
    TestEnv env;
    auto result = run_script(
        "let v = 0;\n"
        "let a = v && (v = 2);\n"
        "let b = v || (v = 3);\n"
        "return {a, b, v};\n",
        env.library,
        env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(value.type_, JsNodeType::Object);
    const JsValue *a = object_value(value, "a");
    const JsValue *b = object_value(value, "b");
    const JsValue *v = object_value(value, "v");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(a->type_, JsNodeType::Integer);
    EXPECT_EQ(a->i, 0);
    EXPECT_EQ(b->type_, JsNodeType::Integer);
    EXPECT_EQ(b->i, 3);
    EXPECT_EQ(v->type_, JsNodeType::Integer);
    EXPECT_EQ(v->i, 3);
}

TEST(ScriptPlanTest, ComparisonsAndEquality) {
    TestEnv env;
    auto result = run_script(
        "return {\n"
        "  a: 1 == \"1\",\n"
        "  b: 1 === \"1\",\n"
        "  c: 1 != \"1\",\n"
        "  d: 1 !== \"1\"\n"
        "};\n",
        env.library,
        env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(value.type_, JsNodeType::Object);
    EXPECT_TRUE(object_value_or_default(value, "a").b);
    EXPECT_FALSE(object_value_or_default(value, "b").b);
    EXPECT_FALSE(object_value_or_default(value, "c").b);
    EXPECT_TRUE(object_value_or_default(value, "d").b);
}

TEST(ScriptPlanTest, InOperator) {
    TestEnv env;
    auto result = run_script(
        "let obj = {n:1};\n"
        "return {t: \"n\" in obj, f: \"x\" in obj};\n",
        env.library,
        env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(value.type_, JsNodeType::Object);
    EXPECT_TRUE(object_value_or_default(value, "t").b);
    EXPECT_FALSE(object_value_or_default(value, "f").b);
}

TEST(ScriptPlanTest, UnaryOps) {
    TestEnv env;
    auto result = run_script("return {a:+3, b:-(2), c:!0, d:typeof null};",
                             env.library,
                             env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(value.type_, JsNodeType::Object);
    double a_num = 0.0;
    double b_num = 0.0;
    ASSERT_TRUE(value_to_number(object_value_or_default(value, "a"), a_num));
    ASSERT_TRUE(value_to_number(object_value_or_default(value, "b"), b_num));
    EXPECT_EQ(a_num, 3.0);
    EXPECT_EQ(b_num, -2.0);
    EXPECT_TRUE(object_value_or_default(value, "c").b);
    EXPECT_EQ(value_to_string(object_value_or_default(value, "d")), "null");
}

TEST(ScriptPlanTest, TernaryOperator) {
    TestEnv env;
    auto result = run_script("return (1 > 2) ? \"no\" : \"yes\";", env.library, env.runtime);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(value_to_string(result.value()), "yes");
}

TEST(ScriptPlanTest, AccessAndAssignment) {
    TestEnv env;
    auto result = run_script(
        "let o = {a:1};\n"
        "let a = [o.a, 2];\n"
        "o.a = 3;\n"
        "a[1] = 4;\n"
        "return {o, a};\n",
        env.library,
        env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(value.type_, JsNodeType::Object);
    const JsValue *o = object_value(value, "o");
    const JsValue *a = object_value(value, "a");
    ASSERT_NE(o, nullptr);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(object_value_or_default(*o, "a").i, 3);
    ASSERT_EQ(a->type_, JsNodeType::Array);
    EXPECT_EQ(array_value_or_default(*a, 0).i, 1);
    EXPECT_EQ(array_value_or_default(*a, 1).i, 4);
}

TEST(ScriptPlanTest, SpreadInArrayObjectAndCall) {
    TestEnv env;
    auto result = run_script(
        "let a = [1,2];\n"
        "let b = [0, ...a, 3];\n"
        "let o = {a:1};\n"
        "let p = {z:0, ...o, b:2};\n"
        "return {b, p, sum: add(...b)};\n",
        env.library,
        env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(value.type_, JsNodeType::Object);
    const JsValue *b = object_value(value, "b");
    const JsValue *p = object_value(value, "p");
    const JsValue *sum = object_value(value, "sum");
    ASSERT_NE(b, nullptr);
    ASSERT_NE(p, nullptr);
    ASSERT_NE(sum, nullptr);
    ASSERT_EQ(b->type_, JsNodeType::Array);
    EXPECT_EQ(array_value_or_default(*b, 0).i, 0);
    EXPECT_EQ(array_value_or_default(*b, 1).i, 1);
    EXPECT_EQ(array_value_or_default(*b, 2).i, 2);
    EXPECT_EQ(array_value_or_default(*b, 3).i, 3);
    EXPECT_EQ(object_value_or_default(*p, "z").i, 0);
    EXPECT_EQ(object_value_or_default(*p, "a").i, 1);
    EXPECT_EQ(object_value_or_default(*p, "b").i, 2);
    EXPECT_EQ(sum->i, 6);
}

TEST(ScriptPlanTest, IfElseReturn) {
    TestEnv env;
    auto result = run_script(
        "let v = 2;\n"
        "if (v > 1) { return \"big\"; }\n"
        "return \"small\";\n",
        env.library,
        env.runtime);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(value_to_string(result.value()), "big");
}

TEST(ScriptPlanTest, ForOfArrayWithBreakContinue) {
    TestEnv env;
    auto result = run_script(
        "let arr = [10, 20, 30];\n"
        "let out = [];\n"
        "for (let i, v of arr) {\n"
        "  if (i == 0) { continue; }\n"
        "  array.push(out, v);\n"
        "  break;\n"
        "}\n"
        "return out;\n",
        env.library,
        env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(value.type_, JsNodeType::Array);
    EXPECT_EQ(array_value_or_default(value, 0).i, 20);
}

TEST(ScriptPlanTest, ForOfObjectKeysValues) {
    TestEnv env;
    auto result = run_script(
        "let obj = {a:1, b:2};\n"
        "let out = {};\n"
        "for (let k, v of obj) { out[k] = v + 1; }\n"
        "return out;\n",
        env.library,
        env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(value.type_, JsNodeType::Object);
    EXPECT_EQ(object_value_or_default(value, "a").i, 2);
    EXPECT_EQ(object_value_or_default(value, "b").i, 3);
}

TEST(ScriptPlanTest, TryCatchThrowString) {
    TestEnv env;
    auto result = run_script("try { throw \"err\"; } catch (e) { return e; }",
                             env.library,
                             env.runtime);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(value_to_string(result.value()), "err");
}

TEST(ScriptPlanTest, TryCatchThrowObject) {
    TestEnv env;
    auto result = run_script(
        "let obj = {a:1};\n"
        "try { throw obj; } catch (e) { return e === obj; }\n",
        env.library,
        env.runtime);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().type_, JsNodeType::Boolean);
    EXPECT_TRUE(result.value().b);
}

TEST(ScriptPlanTest, DirectiveCall) {
    TestEnv env;
    auto result = run_script(
        "directive demoService from dubbo \"com.test.dubbo.DemoService\";\n"
        "return demoService.createUser(\"name\");\n",
        env.library,
        env.runtime);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(value_to_string(result.value()), "user:name");
}

TEST(ScriptPlanTest, LengthAndIncludes) {
    TestEnv env;
    auto result = run_script(
        "return {\n"
        "  a: length(\"abc\") === 3,\n"
        "  b: length({a:1,b:2}) === 2,\n"
        "  c: length([1,2,3]) === 3,\n"
        "  d: length(1) === 0,\n"
        "  e: includes(\"abcabc\", \"cab\") === true,\n"
        "  f: includes([\"aa\",\"bb\",\"cc\"], \"aa\") === true,\n"
        "  g: includes({a:1}, \"a\") === false\n"
        "};\n",
        env.library,
        env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(value.type_, JsNodeType::Object);
    EXPECT_TRUE(object_value_or_default(value, "a").b);
    EXPECT_TRUE(object_value_or_default(value, "b").b);
    EXPECT_TRUE(object_value_or_default(value, "c").b);
    EXPECT_TRUE(object_value_or_default(value, "d").b);
    EXPECT_TRUE(object_value_or_default(value, "e").b);
    EXPECT_TRUE(object_value_or_default(value, "f").b);
    EXPECT_TRUE(object_value_or_default(value, "g").b);
}

TEST(ScriptPlanTest, ArrayPushPopJoin) {
    TestEnv env;
    auto result = run_script(
        "let a = [1,2];\n"
        "let b = array.push(a, 3, 4);\n"
        "let c = array.pop(a);\n"
        "return {same: a === b, c, join: array.join(a, \"-\"), len: length(a)};\n",
        env.library,
        env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(value.type_, JsNodeType::Object);
    EXPECT_TRUE(object_value_or_default(value, "same").b);
    EXPECT_EQ(object_value_or_default(value, "c").i, 4);
    EXPECT_EQ(value_to_string(object_value_or_default(value, "join")), "1-2-3");
    EXPECT_EQ(object_value_or_default(value, "len").i, 3);
}

TEST(ScriptPlanTest, ObjectAssignKeysValuesDelete) {
    TestEnv env;
    auto result = run_script(
        "let a = {a:1,b:2};\n"
        "Object.assign(a, {c:3});\n"
        "let keys = Object.keys(a);\n"
        "let values = Object.values(a);\n"
        "Object.deleteProperties(a, \"a\", \"x\");\n"
        "return {len:length(a), a:a.a, keys, values};\n",
        env.library,
        env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(value.type_, JsNodeType::Object);
    EXPECT_EQ(object_value_or_default(value, "len").i, 2);
    const JsValue *a_prop = object_value(value, "a");
    ASSERT_NE(a_prop, nullptr);
    EXPECT_EQ(a_prop->type_, JsNodeType::Undefined);

    const JsValue *keys = object_value(value, "keys");
    const JsValue *values = object_value(value, "values");
    ASSERT_NE(keys, nullptr);
    ASSERT_NE(values, nullptr);
    ASSERT_EQ(keys->type_, JsNodeType::Array);
    ASSERT_EQ(values->type_, JsNodeType::Array);
    auto *keys_arr = reinterpret_cast<const GcArray *>(keys->gc);
    auto *values_arr = reinterpret_cast<const GcArray *>(values->gc);
    ASSERT_NE(keys_arr, nullptr);
    ASSERT_NE(values_arr, nullptr);
    EXPECT_EQ(keys_arr->size, 3u);
    EXPECT_EQ(values_arr->size, 3u);

    std::string key0 = value_to_string(array_value_or_default(*keys, 0));
    std::string key1 = value_to_string(array_value_or_default(*keys, 1));
    std::string key2 = value_to_string(array_value_or_default(*keys, 2));
    bool has_a = (key0 == "a" || key1 == "a" || key2 == "a");
    bool has_b = (key0 == "b" || key1 == "b" || key2 == "b");
    bool has_c = (key0 == "c" || key1 == "c" || key2 == "c");
    EXPECT_TRUE(has_a);
    EXPECT_TRUE(has_b);
    EXPECT_TRUE(has_c);

    std::int64_t val0 = array_value_or_default(*values, 0).i;
    std::int64_t val1 = array_value_or_default(*values, 1).i;
    std::int64_t val2 = array_value_or_default(*values, 2).i;
    bool has_1 = (val0 == 1 || val1 == 1 || val2 == 1);
    bool has_2 = (val0 == 2 || val1 == 2 || val2 == 2);
    bool has_3 = (val0 == 3 || val1 == 3 || val2 == 3);
    EXPECT_TRUE(has_1);
    EXPECT_TRUE(has_2);
    EXPECT_TRUE(has_3);
}

TEST(ScriptPlanTest, StringsCoreSet) {
    TestEnv env;
    auto result = run_script(
        "return {\n"
        "  prefix: strings.hasPrefix(\"abcdedf\", \"abc\"),\n"
        "  suffix: strings.hasSuffix(\"abcdedf\", \"edf\"),\n"
        "  lower: strings.toLower(\"AbC\") === \"abc\",\n"
        "  upper: strings.toUpper(\"AbC\") === \"ABC\",\n"
        "  trim: strings.trim(\"  \\tabc\\t \") === \"abc\",\n"
        "  split: strings.split(\"abcecdf\", \"c\")[1] === \"e\",\n"
        "  contains: strings.contains(\"abcd-effe-ssf-fd\", \"e-ssf\"),\n"
        "  index: strings.index(\"aabbcc\", \"bcc\") === 3,\n"
        "  last: strings.lastIndex(\"cabcd\", \"c\") === 3,\n"
        "  repeat: strings.repeat(\"acd\", 3) === \"acdacdacd\",\n"
        "  match: strings.match(\"aaabbbbccc\", \"a+b+c+\"),\n"
        "  substring: strings.substring(\"0123456789\", 3, 6) === \"345\"\n"
        "};\n",
        env.library,
        env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(value.type_, JsNodeType::Object);
    EXPECT_TRUE(object_value_or_default(value, "prefix").b);
    EXPECT_TRUE(object_value_or_default(value, "suffix").b);
    EXPECT_TRUE(object_value_or_default(value, "lower").b);
    EXPECT_TRUE(object_value_or_default(value, "upper").b);
    EXPECT_TRUE(object_value_or_default(value, "trim").b);
    EXPECT_TRUE(object_value_or_default(value, "split").b);
    EXPECT_TRUE(object_value_or_default(value, "contains").b);
    EXPECT_TRUE(object_value_or_default(value, "index").b);
    EXPECT_TRUE(object_value_or_default(value, "last").b);
    EXPECT_TRUE(object_value_or_default(value, "repeat").b);
    EXPECT_TRUE(object_value_or_default(value, "match").b);
    EXPECT_TRUE(object_value_or_default(value, "substring").b);
}

TEST(ScriptPlanTest, BinaryAndHash) {
    TestEnv env;
    auto result = run_script(
        "let bin = binary.base64Decode(\"AQID\");\n"
        "return {\n"
        "  b64: binary.base64Encode(bin) === \"AQID\",\n"
        "  hex: binary.hex(bin) === \"010203\",\n"
        "  crc: hash.crc32(\"abc\") === 891568578,\n"
        "  md5: hash.md5(\"abc\") === \"900150983cd24fb0d6963f7d28e17f72\",\n"
        "  sha1: hash.sha1(\"abc\") === \"a9993e364706816aba3e25717850c26c9cd0d89d\",\n"
        "  sha256: hash.sha256(\"abc\") ===\n"
        "    \"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"\n"
        "};\n",
        env.library,
        env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(value.type_, JsNodeType::Object);
    EXPECT_TRUE(object_value_or_default(value, "b64").b);
    EXPECT_TRUE(object_value_or_default(value, "hex").b);
    EXPECT_TRUE(object_value_or_default(value, "crc").b);
    EXPECT_TRUE(object_value_or_default(value, "md5").b);
    EXPECT_TRUE(object_value_or_default(value, "sha1").b);
    EXPECT_TRUE(object_value_or_default(value, "sha256").b);
}

TEST(ScriptPlanTest, JsonParseStringify) {
    TestEnv env;
    auto result = run_script(
        "let obj = JSON.parse(\"{\\\"a\\\":1,\\\"b\\\":[2,3]}\");\n"
        "return JSON.stringify(obj) === \"{\\\"a\\\":1,\\\"b\\\":[2,3]}\";\n",
        env.library,
        env.runtime);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().type_, JsNodeType::Boolean);
    EXPECT_TRUE(result.value().b);
}

TEST(ScriptPlanTest, MathHelpers) {
    TestEnv env;
    auto result = run_script(
        "return {a: math.floor(3.9) === 3, b: math.abs(-4) === 4};",
        env.library,
        env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(value.type_, JsNodeType::Object);
    EXPECT_TRUE(object_value_or_default(value, "a").b);
    EXPECT_TRUE(object_value_or_default(value, "b").b);
}

TEST(ScriptPlanTest, RandStubbed) {
    TestEnv env;
    auto result = run_script(
        "return {a: rand.canary(\"42\") === 42, b: rand.random() >= 0};",
        env.library,
        env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(value.type_, JsNodeType::Object);
    EXPECT_TRUE(object_value_or_default(value, "a").b);
    EXPECT_TRUE(object_value_or_default(value, "b").b);
}

TEST(ScriptPlanTest, TimeStubbed) {
    TestEnv env;
    auto result = run_script(
        "return time.format(1700000000, \"yyyy-MM-dd\") === \"2023-11-14\";",
        env.library,
        env.runtime);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().type_, JsNodeType::Boolean);
    EXPECT_TRUE(result.value().b);
}

TEST(ScriptPlanTest, UrlHelpers) {
    TestEnv env;
    auto result = run_script(
        "let q = url.parseQuery(\"a=1&b=2\");\n"
        "return (url.buildQuery(q) === \"a=1&b=2\" || url.buildQuery(q) === \"b=2&a=1\")\n"
        "  && url.encodeComponent(\"a b\") === \"a+b\"\n"
        "  && url.decodeComponent(\"a%20b\") === \"a b\";\n",
        env.library,
        env.runtime);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().type_, JsNodeType::Boolean);
    EXPECT_TRUE(result.value().b);
}

TEST(ScriptPlanTest, MissingTypeof) {
    TestEnv env;
    auto result = run_script(
        "let o = {};\n"
        "return typeof o.miss;\n",
        env.library,
        env.runtime);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(value_to_string(result.value()), "undefined");
}

TEST(ScriptPlanTest, BuiltinTypeMismatchThrows) {
    TestEnv env;
    auto result = run_script("array.push(1, 2);", env.library, env.runtime);
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(is_string_type(result.error()));
    EXPECT_FALSE(value_to_string(result.error()).empty());
}

TEST(ScriptPlanTest, SyntaxErrorPosition) {
    TestEnv env;
    fiber::script::parse::Parser parser(env.library, true);
    auto parsed = parser.parse_script("let a = [1, 2;");
    ASSERT_FALSE(parsed.has_value());
    EXPECT_FALSE(parsed.error().message.empty());
}
