#include <gtest/gtest.h>

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

namespace {

class TestFunction final : public fiber::script::Library::Function {
public:
    fiber::script::Library::FunctionResult call(fiber::script::ExecutionContext &context) override {
        (void) context;
        return fiber::json::JsValue::make_integer(7);
    }
};

class ThrowFunction final : public fiber::script::Library::Function {
public:
    fiber::script::Library::FunctionResult call(fiber::script::ExecutionContext &context) override {
        (void) context;
        static char msg[] = "boom";
        return std::unexpected(fiber::json::JsValue::make_native_string(msg, 4));
    }
};

class TestConstant final : public fiber::script::Library::Constant {
public:
    fiber::script::Library::FunctionResult get(fiber::script::ExecutionContext &context) override {
        (void) context;
        return fiber::json::JsValue::make_integer(41);
    }
};

class DelayedAsyncFunction final : public fiber::script::Library::AsyncFunction {
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

class TestLibrary final : public fiber::script::Library {
public:
    explicit TestLibrary(TestFunction *func, ThrowFunction *boom, TestConstant *constant,
                         DelayedAsyncFunction *async_func = nullptr) :
        func_(func), boom_(boom), constant_(constant), async_func_(async_func) {}

    Function *find_func(std::string_view name) override {
        if (name == "func") {
            return func_;
        }
        if (name == "boom") {
            return boom_;
        }
        return nullptr;
    }

    AsyncFunction *find_async_func(std::string_view name) override {
        if (name == "asyncFunc") {
            return async_func_;
        }
        return nullptr;
    }

    Constant *find_constant(std::string_view namespace_name, std::string_view key) override {
        if (namespace_name == "$test" && key == "answer") {
            return constant_;
        }
        return nullptr;
    }

    AsyncConstant *find_async_constant(std::string_view namespace_name, std::string_view key) override {
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

    fiber::script::run::VmResult out = fiber::json::JsValue::make_undefined();
    auto state = vm.iterate(out);
    ASSERT_EQ(state, fiber::script::run::InterpreterVm::VmState::Suspend);

    async_func.complete_with(fiber::json::JsValue::make_integer(9));

    state = vm.iterate(out);
    ASSERT_EQ(state, fiber::script::run::InterpreterVm::VmState::Success);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(js_value_type(out.value()), fiber::json::JsNodeType::Integer);
    EXPECT_EQ(js_value_int64(out.value()), 9);
}
