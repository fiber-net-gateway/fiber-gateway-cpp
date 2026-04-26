#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
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
using fiber::json::JsHeapKind;
using fiber::json::JsNodeType;
using fiber::json::JsValue;
using FunctionResult = fiber::script::Library::FunctionResult;

using ScriptResult = fiber::script::ScriptSyncRun::Result;

fiber::script::Library::FunctionSignature exact_args(std::uint16_t argc) {
    fiber::script::Library::FunctionSignature signature;
    signature.required_argc = argc;
    signature.fixed_argc = argc;
    signature.variadic = false;
    return signature;
}

fiber::script::Library::FunctionSignature variadic_args(std::uint16_t fixed_argc) {
    fiber::script::Library::FunctionSignature signature;
    signature.required_argc = fixed_argc;
    signature.fixed_argc = fixed_argc;
    signature.variadic = true;
    return signature;
}

bool matches_signature(const fiber::script::Library::FunctionSignature &signature,
                       const fiber::script::Library::FunctionMatchRequest &request) {
    if (request.spread_argc_unknown && !signature.variadic) {
        return false;
    }
    if (signature.variadic) {
        return request.known_argc >= signature.fixed_argc;
    }
    return request.known_argc >= signature.required_argc && request.known_argc <= signature.fixed_argc;
}

std::string value_to_string(const JsValue &value) {
    if (js_value_type(value) != JsNodeType::String) {
        return {};
    }
    if (js_value_is_borrowed_string(value)) {
        return std::string(js_value_native_string(value).data, js_value_native_string(value).len);
    }
    std::string out;
    auto *str = js_value_heap_ptr<const GcString>(value);
    if (fiber::json::gc_string_to_utf8(str, out)) {
        return out;
    }
    return {};
}

bool value_to_number(const JsValue &value, double &out) {
    if (js_value_type(value) == JsNodeType::Integer) {
        out = static_cast<double>(js_value_int64(value));
        return true;
    }
    if (js_value_type(value) == JsNodeType::Float) {
        out = js_value_double(value);
        return true;
    }
    return false;
}

bool is_string_type(const JsValue &value) { return js_value_type(value) == JsNodeType::String; }

bool is_binary_type(const JsValue &value) { return js_value_type(value) == JsNodeType::Binary; }

const JsValue *object_value(const JsValue &obj, std::string_view key) {
    if (js_value_type(obj) != JsNodeType::Object) {
        return nullptr;
    }
    auto *obj_ptr = js_value_heap_ptr<const GcObject>(obj);
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
    if (js_value_type(arr) != JsNodeType::Array) {
        return nullptr;
    }
    auto *arr_ptr = js_value_heap_ptr<const GcArray>(arr);
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
    GcString *str = runtime.alloc_with_gc(
            text.size(), [&]() { return fiber::json::gc_new_string(&runtime.heap(), text.data(), text.size()); });
    if (!str) {
        return JsValue::make_undefined();
    }
    return js_make_heap_ref(&str->hdr, JsHeapKind::String);
}

class AddFunc final : public fiber::script::Library::Function {
public:
    FunctionResult call(fiber::script::ExecutionContext &context) override {
        double sum = 0.0;
        bool any_float = false;
        for (std::size_t i = 0; i < context.arg_count(); ++i) {
            const JsValue &arg = context.arg_value(i);
            if (js_value_type(arg) == JsNodeType::Integer) {
                sum += static_cast<double>(js_value_int64(arg));
                continue;
            }
            if (js_value_type(arg) == JsNodeType::Float) {
                sum += js_value_double(arg);
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
        (void) context;
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
        if (js_value_type(value) == JsNodeType::Undefined) {
            static char msg[] = "out of memory";
            return std::unexpected(JsValue::make_native_string(msg, sizeof(msg) - 1));
        }
        return value;
    }
};

class RandRandomStub final : public fiber::script::Library::Function {
public:
    FunctionResult call(fiber::script::ExecutionContext &context) override {
        (void) context;
        return JsValue::make_integer(7);
    }
};

class RandCanaryStub final : public fiber::script::Library::Function {
public:
    FunctionResult call(fiber::script::ExecutionContext &context) override {
        (void) context;
        return JsValue::make_integer(42);
    }
};

class TimeFormatStub final : public fiber::script::Library::Function {
public:
    FunctionResult call(fiber::script::ExecutionContext &context) override {
        (void) context;
        JsValue value = make_heap_string(context.runtime(), "2023-11-14");
        if (js_value_type(value) == JsNodeType::Undefined) {
            static char msg[] = "out of memory";
            return std::unexpected(JsValue::make_native_string(msg, sizeof(msg) - 1));
        }
        return value;
    }
};

class DemoServiceDirective final : public fiber::script::Library::DirectiveDef {
public:
    explicit DemoServiceDirective(fiber::script::Library::Function *create_user) : create_user_(create_user) {}

    fiber::script::Library::FunctionMatchResult
    find_func(std::string_view directive, std::string_view function,
              const fiber::script::Library::FunctionMatchRequest &request,
              const fiber::script::Library &library) override {
        if (directive == "demoService" && function == "createUser") {
            auto signature = exact_args(1);
            if (!matches_signature(signature, request)) {
                return fiber::script::Library::FunctionMatchResult::arity_mismatch();
            }
            return fiber::script::Library::FunctionMatchResult::found(library.host_callable_for(create_user_),
                                                                      signature, nullptr, 0);
        }
        return fiber::script::Library::FunctionMatchResult::not_found();
    }

    fiber::script::Library::FunctionMatchResult
    find_async_func(std::string_view directive, std::string_view function,
                    const fiber::script::Library::FunctionMatchRequest &request,
                    const fiber::script::Library &library) override {
        (void) directive;
        (void) function;
        (void) request;
        (void) library;
        return fiber::script::Library::FunctionMatchResult::not_found();
    }

private:
    fiber::script::Library::Function *create_user_ = nullptr;
};

class StubLibrary final : public fiber::script::Library {
public:
    explicit StubLibrary(fiber::script::Library &fallback) :
        fallback_(fallback), directive_(&demo_create_user_) {
        register_func("add", variadic_args(0), &add_);
        register_func("req.readBinary", exact_args(0), &read_binary_);
        register_func("rand.random", exact_args(0), &rand_random_);
        register_func("rand.canary", exact_args(1), &rand_canary_);
        register_func("time.format", exact_args(2), &time_format_);
        aliases_.emplace("url.parseQuery", "URL.parseQuery");
        aliases_.emplace("url.buildQuery", "URL.buildQuery");
        aliases_.emplace("url.encodeComponent", "URL.encodeComponent");
        aliases_.emplace("url.decodeComponent", "URL.decodeComponent");
    }

    void mark_root_prop(std::string_view prop_name) override { fallback_.mark_root_prop(prop_name); }

    FunctionMatchResult find_func(std::string_view name, const FunctionMatchRequest &request) override {
        auto it = functions_.find(std::string(name));
        if (it != functions_.end()) {
            if (!matches_signature(it->second.signature, request)) {
                return FunctionMatchResult::arity_mismatch();
            }
            return FunctionMatchResult::found(host_callable_for(it->second.func), it->second.signature, nullptr, 0);
        }
        auto alias = aliases_.find(std::string(name));
        if (alias != aliases_.end()) {
            return fallback_.find_func(alias->second, request);
        }
        return fallback_.find_func(name, request);
    }

    FunctionMatchResult find_async_func(std::string_view name, const FunctionMatchRequest &request) override {
        auto it = async_functions_.find(std::string(name));
        if (it != async_functions_.end()) {
            if (!matches_signature(it->second.signature, request)) {
                return FunctionMatchResult::arity_mismatch();
            }
            return FunctionMatchResult::found(host_callable_for(it->second.func), it->second.signature, nullptr, 0);
        }
        return fallback_.find_async_func(name, request);
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

    DirectiveDef *find_directive_def(std::string_view type, std::string_view name,
                                     const std::vector<fiber::json::JsValue> &literals) override {
        (void) literals;
        if (type == "dubbo" && name == "demoService") {
            return &directive_;
        }
        return fallback_.find_directive_def(type, name, literals);
    }

    void register_func(std::string name, FunctionSignature signature, Function *func) {
        FunctionEntry entry;
        entry.signature = signature;
        entry.func = func;
        functions_.emplace(std::move(name), entry);
    }

private:
    struct FunctionEntry {
        FunctionSignature signature{};
        Function *func = nullptr;
    };

    struct AsyncFunctionEntry {
        FunctionSignature signature{};
        AsyncFunction *func = nullptr;
    };

    fiber::script::Library &fallback_;
    std::unordered_map<std::string, FunctionEntry> functions_;
    std::unordered_map<std::string, AsyncFunctionEntry> async_functions_;
    std::unordered_map<std::string, std::string> aliases_;
    std::unordered_map<std::string, Constant *> constants_;
    std::unordered_map<std::string, AsyncConstant *> async_constants_;

    AddFunc add_;
    ReqReadBinaryFunc read_binary_;
    DemoCreateUserFunc demo_create_user_;
    RandRandomStub rand_random_;
    RandCanaryStub rand_canary_;
    TimeFormatStub time_format_;
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

ScriptResult run_script(std::string_view script, fiber::script::Library &library,
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

    TestEnv() : runtime(heap, roots), library(fiber::script::std_lib::StdLibrary::instance()) {}
};

} // namespace

TEST(ScriptPlanTest, LiteralsAndTypeof) {
    TestEnv env;
    auto result = run_script("let num = 1;\n"
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
                             env.library, env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(js_value_type(value), JsNodeType::Object);

    const JsValue *types = object_value(value, "types");
    ASSERT_NE(types, nullptr);
    ASSERT_EQ(js_value_type(*types), JsNodeType::Object);
    const JsValue *res = object_value(value, "result");
    ASSERT_NE(res, nullptr);
    ASSERT_EQ(js_value_type(*res), JsNodeType::Object);

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
    EXPECT_EQ(js_value_type(*mis), JsNodeType::Undefined);
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
    auto result = run_script("return strings.toString(1) + \"a\" + strings.toString(2);", env.library, env.runtime);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(value_to_string(result.value()), "1a2");
}

TEST(ScriptPlanTest, LogicalShortCircuit) {
    TestEnv env;
    auto result = run_script("let v = 0;\n"
                             "let a = v && (v = 2);\n"
                             "let b = v || (v = 3);\n"
                             "return {a, b, v};\n",
                             env.library, env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(js_value_type(value), JsNodeType::Object);
    const JsValue *a = object_value(value, "a");
    const JsValue *b = object_value(value, "b");
    const JsValue *v = object_value(value, "v");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(js_value_type(*a), JsNodeType::Integer);
    EXPECT_EQ(js_value_int64(*a), 0);
    EXPECT_EQ(js_value_type(*b), JsNodeType::Integer);
    EXPECT_EQ(js_value_int64(*b), 3);
    EXPECT_EQ(js_value_type(*v), JsNodeType::Integer);
    EXPECT_EQ(js_value_int64(*v), 3);
}

TEST(ScriptPlanTest, ComparisonsAndEquality) {
    TestEnv env;
    auto result = run_script("return {\n"
                             "  a: 1 == \"1\",\n"
                             "  b: 1 === \"1\",\n"
                             "  c: 1 != \"1\",\n"
                             "  d: 1 !== \"1\"\n"
                             "};\n",
                             env.library, env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(js_value_type(value), JsNodeType::Object);
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "a")));
    EXPECT_FALSE(js_value_bool(object_value_or_default(value, "b")));
    EXPECT_FALSE(js_value_bool(object_value_or_default(value, "c")));
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "d")));
}

TEST(ScriptPlanTest, InOperator) {
    TestEnv env;
    auto result = run_script("let obj = {n:1};\n"
                             "return {t: \"n\" in obj, f: \"x\" in obj};\n",
                             env.library, env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(js_value_type(value), JsNodeType::Object);
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "t")));
    EXPECT_FALSE(js_value_bool(object_value_or_default(value, "f")));
}

TEST(ScriptPlanTest, UnaryOps) {
    TestEnv env;
    auto result = run_script("return {a:+3, b:-(2), c:!0, d:typeof null};", env.library, env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(js_value_type(value), JsNodeType::Object);
    double a_num = 0.0;
    double b_num = 0.0;
    ASSERT_TRUE(value_to_number(object_value_or_default(value, "a"), a_num));
    ASSERT_TRUE(value_to_number(object_value_or_default(value, "b"), b_num));
    EXPECT_EQ(a_num, 3.0);
    EXPECT_EQ(b_num, -2.0);
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "c")));
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
    auto result = run_script("let o = {a:1};\n"
                             "let a = [o.a, 2];\n"
                             "o.a = 3;\n"
                             "a[1] = 4;\n"
                             "return {o, a};\n",
                             env.library, env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(js_value_type(value), JsNodeType::Object);
    const JsValue *o = object_value(value, "o");
    const JsValue *a = object_value(value, "a");
    ASSERT_NE(o, nullptr);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(js_value_int64(object_value_or_default(*o, "a")), 3);
    ASSERT_EQ(js_value_type(*a), JsNodeType::Array);
    EXPECT_EQ(js_value_int64(array_value_or_default(*a, 0)), 1);
    EXPECT_EQ(js_value_int64(array_value_or_default(*a, 1)), 4);
}

TEST(ScriptPlanTest, SpreadInArrayObjectAndCall) {
    TestEnv env;
    auto result = run_script("let a = [1,2];\n"
                             "let b = [0, ...a, 3];\n"
                             "let o = {a:1};\n"
                             "let p = {z:0, ...o, b:2};\n"
                             "return {b, p, sum: add(...b)};\n",
                             env.library, env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(js_value_type(value), JsNodeType::Object);
    const JsValue *b = object_value(value, "b");
    const JsValue *p = object_value(value, "p");
    const JsValue *sum = object_value(value, "sum");
    ASSERT_NE(b, nullptr);
    ASSERT_NE(p, nullptr);
    ASSERT_NE(sum, nullptr);
    ASSERT_EQ(js_value_type(*b), JsNodeType::Array);
    EXPECT_EQ(js_value_int64(array_value_or_default(*b, 0)), 0);
    EXPECT_EQ(js_value_int64(array_value_or_default(*b, 1)), 1);
    EXPECT_EQ(js_value_int64(array_value_or_default(*b, 2)), 2);
    EXPECT_EQ(js_value_int64(array_value_or_default(*b, 3)), 3);
    EXPECT_EQ(js_value_int64(object_value_or_default(*p, "z")), 0);
    EXPECT_EQ(js_value_int64(object_value_or_default(*p, "a")), 1);
    EXPECT_EQ(js_value_int64(object_value_or_default(*p, "b")), 2);
    EXPECT_EQ(js_value_int64(*sum), 6);
}

TEST(ScriptPlanTest, IfElseReturn) {
    TestEnv env;
    auto result = run_script("let v = 2;\n"
                             "if (v > 1) { return \"big\"; }\n"
                             "return \"small\";\n",
                             env.library, env.runtime);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(value_to_string(result.value()), "big");
}

TEST(ScriptPlanTest, ForOfArrayWithBreakContinue) {
    TestEnv env;
    auto result = run_script("let arr = [10, 20, 30];\n"
                             "let out = [];\n"
                             "for (let i, v of arr) {\n"
                             "  if (i == 0) { continue; }\n"
                             "  array.push(out, v);\n"
                             "  break;\n"
                             "}\n"
                             "return out;\n",
                             env.library, env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(js_value_type(value), JsNodeType::Array);
    EXPECT_EQ(js_value_int64(array_value_or_default(value, 0)), 20);
}

TEST(ScriptPlanTest, ForOfObjectKeysValues) {
    TestEnv env;
    auto result = run_script("let obj = {a:1, b:2};\n"
                             "let out = {};\n"
                             "for (let k, v of obj) { out[k] = v + 1; }\n"
                             "return out;\n",
                             env.library, env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(js_value_type(value), JsNodeType::Object);
    EXPECT_EQ(js_value_int64(object_value_or_default(value, "a")), 2);
    EXPECT_EQ(js_value_int64(object_value_or_default(value, "b")), 3);
}

TEST(ScriptPlanTest, TryCatchThrowString) {
    TestEnv env;
    auto result = run_script("try { throw \"err\"; } catch (e) { return e; }", env.library, env.runtime);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(value_to_string(result.value()), "err");
}

TEST(ScriptPlanTest, TryCatchThrowObject) {
    TestEnv env;
    auto result = run_script("let obj = {a:1};\n"
                             "try { throw obj; } catch (e) { return e === obj; }\n",
                             env.library, env.runtime);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(js_value_type(result.value()), JsNodeType::Boolean);
    EXPECT_TRUE(js_value_bool(result.value()));
}

TEST(ScriptPlanTest, DirectiveCall) {
    TestEnv env;
    auto result = run_script("directive demoService from dubbo \"com.test.dubbo.DemoService\";\n"
                             "return demoService.createUser(\"name\");\n",
                             env.library, env.runtime);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(value_to_string(result.value()), "user:name");
}

TEST(ScriptPlanTest, LengthAndIncludes) {
    TestEnv env;
    auto result = run_script("return {\n"
                             "  a: length(\"abc\") === 3,\n"
                             "  b: length({a:1,b:2}) === 2,\n"
                             "  c: length([1,2,3]) === 3,\n"
                             "  d: length(1) === 0,\n"
                             "  e: includes(\"abcabc\", \"cab\") === true,\n"
                             "  f: includes([\"aa\",\"bb\",\"cc\"], \"aa\") === true,\n"
                             "  g: includes({a:1}, \"a\") === false\n"
                             "};\n",
                             env.library, env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(js_value_type(value), JsNodeType::Object);
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "a")));
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "b")));
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "c")));
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "d")));
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "e")));
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "f")));
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "g")));
}

TEST(ScriptPlanTest, ArrayPushPopJoin) {
    TestEnv env;
    auto result = run_script("let a = [1,2];\n"
                             "let b = array.push(a, 3, 4);\n"
                             "let c = array.pop(a);\n"
                             "return {same: a === b, c, join: array.join(a, \"-\"), len: length(a)};\n",
                             env.library, env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(js_value_type(value), JsNodeType::Object);
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "same")));
    EXPECT_EQ(js_value_int64(object_value_or_default(value, "c")), 4);
    EXPECT_EQ(value_to_string(object_value_or_default(value, "join")), "1-2-3");
    EXPECT_EQ(js_value_int64(object_value_or_default(value, "len")), 3);
}

TEST(ScriptPlanTest, ObjectAssignKeysValuesDelete) {
    TestEnv env;
    auto result = run_script("let a = {a:1,b:2};\n"
                             "Object.assign(a, {c:3});\n"
                             "let keys = Object.keys(a);\n"
                             "let values = Object.values(a);\n"
                             "Object.deleteProperties(a, \"a\", \"x\");\n"
                             "return {len:length(a), a:a.a, keys, values};\n",
                             env.library, env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(js_value_type(value), JsNodeType::Object);
    EXPECT_EQ(js_value_int64(object_value_or_default(value, "len")), 2);
    const JsValue *a_prop = object_value(value, "a");
    ASSERT_NE(a_prop, nullptr);
    EXPECT_EQ(js_value_type(*a_prop), JsNodeType::Undefined);

    const JsValue *keys = object_value(value, "keys");
    const JsValue *values = object_value(value, "values");
    ASSERT_NE(keys, nullptr);
    ASSERT_NE(values, nullptr);
    ASSERT_EQ(js_value_type(*keys), JsNodeType::Array);
    ASSERT_EQ(js_value_type(*values), JsNodeType::Array);
    auto *keys_arr = js_value_heap_ptr<const GcArray>(*keys);
    auto *values_arr = js_value_heap_ptr<const GcArray>(*values);
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

    std::int64_t val0 = js_value_int64(array_value_or_default(*values, 0));
    std::int64_t val1 = js_value_int64(array_value_or_default(*values, 1));
    std::int64_t val2 = js_value_int64(array_value_or_default(*values, 2));
    bool has_1 = (val0 == 1 || val1 == 1 || val2 == 1);
    bool has_2 = (val0 == 2 || val1 == 2 || val2 == 2);
    bool has_3 = (val0 == 3 || val1 == 3 || val2 == 3);
    EXPECT_TRUE(has_1);
    EXPECT_TRUE(has_2);
    EXPECT_TRUE(has_3);
}

TEST(ScriptPlanTest, StringsCoreSet) {
    TestEnv env;
    auto result = run_script("return {\n"
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
                             env.library, env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(js_value_type(value), JsNodeType::Object);
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "prefix")));
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "suffix")));
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "lower")));
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "upper")));
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "trim")));
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "split")));
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "contains")));
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "index")));
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "last")));
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "repeat")));
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "match")));
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "substring")));
}

TEST(ScriptPlanTest, BinaryAndHash) {
    TestEnv env;
    auto result = run_script("let bin = binary.base64Decode(\"AQID\");\n"
                             "return {\n"
                             "  b64: binary.base64Encode(bin) === \"AQID\",\n"
                             "  hex: binary.hex(bin) === \"010203\",\n"
                             "  crc: hash.crc32(\"abc\") === 891568578,\n"
                             "  md5: hash.md5(\"abc\") === \"900150983cd24fb0d6963f7d28e17f72\",\n"
                             "  sha1: hash.sha1(\"abc\") === \"a9993e364706816aba3e25717850c26c9cd0d89d\",\n"
                             "  sha256: hash.sha256(\"abc\") ===\n"
                             "    \"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"\n"
                             "};\n",
                             env.library, env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(js_value_type(value), JsNodeType::Object);
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "b64")));
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "hex")));
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "crc")));
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "md5")));
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "sha1")));
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "sha256")));
}

TEST(ScriptPlanTest, JsonParseStringify) {
    TestEnv env;
    auto result = run_script("let obj = JSON.parse(\"{\\\"a\\\":1,\\\"b\\\":[2,3]}\");\n"
                             "return JSON.stringify(obj) === \"{\\\"a\\\":1,\\\"b\\\":[2,3]}\";\n",
                             env.library, env.runtime);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(js_value_type(result.value()), JsNodeType::Boolean);
    EXPECT_TRUE(js_value_bool(result.value()));
}

TEST(ScriptPlanTest, MathHelpers) {
    TestEnv env;
    auto result = run_script("return {a: math.floor(3.9) === 3, b: math.abs(-4) === 4};", env.library, env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(js_value_type(value), JsNodeType::Object);
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "a")));
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "b")));
}

TEST(ScriptPlanTest, RandStubbed) {
    TestEnv env;
    auto result =
            run_script("return {a: rand.canary(\"42\") === 42, b: rand.random() >= 0};", env.library, env.runtime);
    ASSERT_TRUE(result.has_value());
    const JsValue &value = result.value();
    ASSERT_EQ(js_value_type(value), JsNodeType::Object);
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "a")));
    EXPECT_TRUE(js_value_bool(object_value_or_default(value, "b")));
}

TEST(ScriptPlanTest, TimeStubbed) {
    TestEnv env;
    auto result =
            run_script("return time.format(1700000000, \"yyyy-MM-dd\") === \"2023-11-14\";", env.library, env.runtime);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(js_value_type(result.value()), JsNodeType::Boolean);
    EXPECT_TRUE(js_value_bool(result.value()));
}

TEST(ScriptPlanTest, UrlHelpers) {
    TestEnv env;
    auto result = run_script("let q = url.parseQuery(\"a=1&b=2\");\n"
                             "return (url.buildQuery(q) === \"a=1&b=2\" || url.buildQuery(q) === \"b=2&a=1\")\n"
                             "  && url.encodeComponent(\"a b\") === \"a+b\"\n"
                             "  && url.decodeComponent(\"a%20b\") === \"a b\";\n",
                             env.library, env.runtime);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(js_value_type(result.value()), JsNodeType::Boolean);
    EXPECT_TRUE(js_value_bool(result.value()));
}

TEST(ScriptPlanTest, MissingTypeof) {
    TestEnv env;
    auto result = run_script("let o = {};\n"
                             "return typeof o.miss;\n",
                             env.library, env.runtime);
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
