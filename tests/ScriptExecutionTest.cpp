#include <gtest/gtest.h>

#include <cstddef>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "common/json/JsGc.h"
#include "script/Library.h"
#include "script/Runtime.h"
#include "script/Script.h"
#include "script/ir/Compiler.h"
#include "script/parse/Parser.h"
#include "script/run/InterpreterVm.h"
#include "script/std/StdLibrary.h"

namespace {

class TestFunction final : public fiber::script::Library::LegacyFunction {
public:
    fiber::script::Library::FunctionResult call(fiber::script::ExecutionContext &context) override {
        (void) context;
        return fiber::json::JsValue::make_integer(7);
    }
};

class ThrowFunction final : public fiber::script::Library::LegacyFunction {
public:
    fiber::script::Library::FunctionResult call(fiber::script::ExecutionContext &context) override {
        (void) context;
        static char msg[] = "boom";
        return std::unexpected(fiber::json::JsValue::make_native_string(msg, 4));
    }
};

class TestConstant final : public fiber::script::Library::LegacyConstant {
public:
    fiber::script::Library::FunctionResult get(fiber::script::ExecutionContext &context) override {
        (void) context;
        return fiber::json::JsValue::make_integer(41);
    }
};

class DelayedAsyncFunction final : public fiber::script::Library::LegacyAsyncFunction {
public:
    void call(fiber::script::AsyncExecutionContext &context) override { context_ = &context; }

    void complete_with(const fiber::json::JsValue &value) {
        if (!context_) {
            return;
        }
        auto *context = context_;
        context_ = nullptr;
        context->return_value(value);
    }

private:
    fiber::script::AsyncExecutionContext *context_ = nullptr;
};

class AddDefaultFunction final : public fiber::script::Library::LegacyFunction {
public:
    fiber::script::Library::FunctionResult call(fiber::script::ExecutionContext &context) override {
        observed_argc = context.arg_count();
        std::int64_t a = js_value_int64(context.arg_value(0));
        std::int64_t b = js_value_int64(context.arg_value(1));
        return fiber::json::JsValue::make_integer(a + b);
    }

    std::size_t observed_argc = 0;
};

class TestLibrary final : public fiber::script::Library {
public:
    explicit TestLibrary(TestFunction *func, ThrowFunction *boom, TestConstant *constant,
                         DelayedAsyncFunction *async_func = nullptr) :
        func_(func), boom_(boom), constant_(constant), async_func_(async_func) {}

    FunctionMatchResult find_func(std::string_view name, const FunctionMatchRequest &request) override {
        LegacyFunction *func = nullptr;
        std::uint16_t argc = 0;
        if (name == "func") {
            func = func_;
        } else if (name == "boom") {
            func = boom_;
        } else {
            return FunctionMatchResult::not_found();
        }
        if (request.has_spread || request.known_argc != argc) {
            return FunctionMatchResult::arity_mismatch();
        }
        FunctionSignature signature;
        signature.required_argc = argc;
        signature.fixed_argc = argc;
        signature.variadic = false;
        return FunctionMatchResult::found(host_callable_for(func), signature, nullptr, 0);
    }

    FunctionMatchResult find_async_func(std::string_view name, const FunctionMatchRequest &request) override {
        if (name == "asyncFunc") {
            if (request.has_spread || request.known_argc != 1) {
                return FunctionMatchResult::arity_mismatch();
            }
            FunctionSignature signature;
            signature.required_argc = 1;
            signature.fixed_argc = 1;
            signature.variadic = false;
            return FunctionMatchResult::found(host_callable_for(async_func_), signature, nullptr, 0);
        }
        return FunctionMatchResult::not_found();
    }

    LegacyConstant *find_constant(std::string_view namespace_name, std::string_view key) override {
        if (namespace_name == "$test" && key == "answer") {
            return constant_;
        }
        return nullptr;
    }

    LegacyAsyncConstant *find_async_constant(std::string_view namespace_name, std::string_view key) override {
        (void) namespace_name;
        (void) key;
        return nullptr;
    }

    DirectiveDef *find_directive_def(std::string_view type, std::string_view name,
                                     const std::vector<fiber::json::JsValue> &literals) override {
        (void) type;
        (void) name;
        (void) literals;
        return nullptr;
    }

private:
    TestFunction *func_ = nullptr;
    ThrowFunction *boom_ = nullptr;
    TestConstant *constant_ = nullptr;
    DelayedAsyncFunction *async_func_ = nullptr;
};

class SignatureTestLibrary final : public fiber::script::Library {
public:
    explicit SignatureTestLibrary(AddDefaultFunction *func) : func_(func) {}

    FunctionMatchResult find_func(std::string_view name, const FunctionMatchRequest &request) override {
        if (name != "addDefault") {
            return FunctionMatchResult::not_found();
        }
        if (request.has_spread) {
            return FunctionMatchResult::arity_mismatch();
        }
        if (request.known_argc < 1 || request.known_argc > 2) {
            return FunctionMatchResult::arity_mismatch();
        }
        FunctionSignature signature;
        signature.required_argc = 1;
        signature.fixed_argc = 2;
        signature.variadic = false;
        signature.default_count = 1;
        signature.defaults = defaults_;
        const std::uint16_t default_count = static_cast<std::uint16_t>(2 - request.known_argc);
        const fiber::json::JsValue *defaults = default_count == 0 ? nullptr : defaults_;
        return FunctionMatchResult::found(host_callable_for(func_), signature, defaults, default_count);
    }

    FunctionMatchResult find_async_func(std::string_view name, const FunctionMatchRequest &request) override {
        (void) name;
        (void) request;
        return FunctionMatchResult::not_found();
    }

    LegacyConstant *find_constant(std::string_view namespace_name, std::string_view key) override {
        (void) namespace_name;
        (void) key;
        return nullptr;
    }

    LegacyAsyncConstant *find_async_constant(std::string_view namespace_name, std::string_view key) override {
        (void) namespace_name;
        (void) key;
        return nullptr;
    }

    DirectiveDef *find_directive_def(std::string_view type, std::string_view name,
                                     const std::vector<fiber::json::JsValue> &literals) override {
        (void) type;
        (void) name;
        (void) literals;
        return nullptr;
    }

private:
    AddDefaultFunction *func_ = nullptr;
    fiber::json::JsValue defaults_[1] = {fiber::json::JsValue::make_integer(1)};
};

fiber::script::ir::Compiled compile_script(std::string_view script, fiber::script::Library &library) {
    fiber::script::parse::Parser parser(library, true);
    auto parsed = parser.parse_script(script);
    EXPECT_TRUE(parsed.has_value()) << parsed.error().message;
    return fiber::script::ir::Compiler::compile(*parsed.value());
}

std::string value_to_string(const fiber::json::JsValue &value) {
    if (js_value_type(value) != fiber::json::JsNodeType::String) {
        return {};
    }
    if (js_value_is_borrowed_string(value)) {
        return std::string(js_value_native_string(value).data, js_value_native_string(value).len);
    }
    std::string out;
    auto *str = js_value_heap_ptr<const fiber::json::GcString>(value);
    if (fiber::json::gc_string_to_utf8(str, out)) {
        return out;
    }
    return {};
}

} // namespace

TEST(ScriptExecutionTest, RunSimpleReturn) {
    TestFunction func;
    ThrowFunction boom;
    TestConstant constant;
    TestLibrary library(&func, &boom, &constant);

    auto compiled = compile_script("return 1 + 2 * 3;", library);
    auto compiled_ptr = std::make_shared<fiber::script::ir::Compiled>(std::move(compiled));
    fiber::script::Script script(compiled_ptr);

    fiber::json::GcHeap heap;
    fiber::json::GcRootSet roots;
    fiber::script::ScriptRuntime runtime(heap, roots);
    auto run = script.exec_sync(fiber::json::JsValue::make_undefined(), nullptr, runtime);
    auto result = run();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(js_value_type(result.value()), fiber::json::JsNodeType::Integer);
    EXPECT_EQ(js_value_int64(result.value()), 7);
}

TEST(ScriptExecutionTest, RunThrowLiteral) {
    TestFunction func;
    ThrowFunction boom;
    TestConstant constant;
    TestLibrary library(&func, &boom, &constant);

    auto compiled = compile_script("throw \"oops\";", library);
    auto compiled_ptr = std::make_shared<fiber::script::ir::Compiled>(std::move(compiled));
    fiber::script::Script script(compiled_ptr);

    fiber::json::GcHeap heap;
    fiber::json::GcRootSet roots;
    fiber::script::ScriptRuntime runtime(heap, roots);
    auto run = script.exec_sync(fiber::json::JsValue::make_undefined(), nullptr, runtime);
    auto result = run();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(value_to_string(result.error()), "oops");
}

TEST(ScriptExecutionTest, RunFunctionThrowCaught) {
    TestFunction func;
    ThrowFunction boom;
    TestConstant constant;
    TestLibrary library(&func, &boom, &constant);

    auto compiled = compile_script("try { boom(); return 0; } catch (e) { return e; }", library);
    auto compiled_ptr = std::make_shared<fiber::script::ir::Compiled>(std::move(compiled));
    fiber::script::Script script(compiled_ptr);

    fiber::json::GcHeap heap;
    fiber::json::GcRootSet roots;
    fiber::script::ScriptRuntime runtime(heap, roots);
    auto run = script.exec_sync(fiber::json::JsValue::make_undefined(), nullptr, runtime);
    auto result = run();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(value_to_string(result.value()), "boom");
}

TEST(ScriptExecutionTest, LegacyAsyncFunctionSuspendsAndResumes) {
    TestFunction func;
    ThrowFunction boom;
    TestConstant constant;
    DelayedAsyncFunction async_func;
    TestLibrary library(&func, &boom, &constant, &async_func);

    auto compiled = compile_script("return asyncFunc(1);", library);

    fiber::json::GcHeap heap;
    fiber::json::GcRootSet roots;
    fiber::script::ScriptRuntime runtime(heap, roots);
    fiber::script::run::InterpreterVm vm(compiled, fiber::json::JsValue::make_undefined(), nullptr, runtime);

    vm.iterate();
    ASSERT_FALSE(vm.done());
    ASSERT_TRUE(vm.async_task().valid());
    std::coroutine_handle<> handle = vm.async_task().swap_coroutine_handle(std::noop_coroutine());
    handle.resume();

    async_func.complete_with(fiber::json::JsValue::make_integer(9));

    vm.iterate();
    ASSERT_TRUE(vm.done());
    ASSERT_TRUE(vm.result().has_value());
    EXPECT_EQ(js_value_type(vm.result().value()), fiber::json::JsNodeType::Integer);
    EXPECT_EQ(js_value_int64(vm.result().value()), 9);
}

TEST(ScriptExecutionTest, FunctionDefaultArgumentIsAppendedBeforeHostCall) {
    AddDefaultFunction func;
    SignatureTestLibrary library(&func);

    auto compiled = compile_script("return addDefault(3);", library);
    auto compiled_ptr = std::make_shared<fiber::script::ir::Compiled>(std::move(compiled));
    fiber::script::Script script(compiled_ptr);

    fiber::json::GcHeap heap;
    fiber::json::GcRootSet roots;
    fiber::script::ScriptRuntime runtime(heap, roots);
    auto run = script.exec_sync(fiber::json::JsValue::make_undefined(), nullptr, runtime);
    auto result = run();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(js_value_int64(result.value()), 4);
    EXPECT_EQ(func.observed_argc, 2u);
}

TEST(ScriptExecutionTest, StdArrayPushAcceptsVariadicSpreadArguments) {
    auto &library = fiber::script::std_lib::StdLibrary::instance();
    auto compiled = compile_script("let arr = [0];\n"
                                   "array.push(arr, 1, 2, 3);\n"
                                   "let others = [4, 5];\n"
                                   "array.push(arr, ...others);\n"
                                   "return length(arr);\n",
                                   library);
    auto compiled_ptr = std::make_shared<fiber::script::ir::Compiled>(std::move(compiled));
    fiber::script::Script script(compiled_ptr);

    fiber::json::GcHeap heap;
    fiber::json::GcRootSet roots;
    fiber::script::ScriptRuntime runtime(heap, roots);
    auto run = script.exec_sync(fiber::json::JsValue::make_undefined(), nullptr, runtime);
    auto result = run();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(js_value_int64(result.value()), 6);
}

TEST(ScriptExecutionTest, StdFunctionArityMismatchFailsParsing) {
    auto &library = fiber::script::std_lib::StdLibrary::instance();
    fiber::script::parse::Parser parser(library, true);
    auto parsed = parser.parse_script("return length();");
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().message, "function argument count mismatch");
}

TEST(ScriptExecutionTest, StdFunctionTooManyArgumentsFailsParsing) {
    auto &library = fiber::script::std_lib::StdLibrary::instance();
    fiber::script::parse::Parser parser(library, true);
    auto parsed = parser.parse_script("return length(\"abc\", \"extra\");");
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().message, "function argument count mismatch");
}

TEST(ScriptExecutionTest, StdFunctionDefaultArgumentIsAppended) {
    auto &library = fiber::script::std_lib::StdLibrary::instance();
    auto compiled = compile_script("return array.join([1, 2]);", library);
    auto compiled_ptr = std::make_shared<fiber::script::ir::Compiled>(std::move(compiled));
    fiber::script::Script script(compiled_ptr);

    fiber::json::GcHeap heap;
    fiber::json::GcRootSet roots;
    fiber::script::ScriptRuntime runtime(heap, roots);
    auto run = script.exec_sync(fiber::json::JsValue::make_undefined(), nullptr, runtime);
    auto result = run();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(value_to_string(result.value()), "12");
}
