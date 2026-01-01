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

namespace {

class TestFunction final : public fiber::script::Library::Function {
public:
    fiber::script::Library::FunctionResult call(fiber::script::ExecutionContext &context) override {
        (void)context;
        return fiber::json::JsValue::make_integer(7);
    }
};

class ThrowFunction final : public fiber::script::Library::Function {
public:
    fiber::script::Library::FunctionResult call(fiber::script::ExecutionContext &context) override {
        (void)context;
        static char msg[] = "boom";
        return std::unexpected(fiber::json::JsValue::make_native_string(msg, 4));
    }
};

class TestConstant final : public fiber::script::Library::Constant {
public:
    fiber::script::Library::FunctionResult get(fiber::script::ExecutionContext &context) override {
        (void)context;
        return fiber::json::JsValue::make_integer(41);
    }
};

class TestLibrary final : public fiber::script::Library {
public:
    explicit TestLibrary(TestFunction *func, ThrowFunction *boom, TestConstant *constant)
        : func_(func), boom_(boom), constant_(constant) {}

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
        (void)name;
        return nullptr;
    }

    Constant *find_constant(std::string_view namespace_name, std::string_view key) override {
        if (namespace_name == "$test" && key == "answer") {
            return constant_;
        }
        return nullptr;
    }

    AsyncConstant *find_async_constant(std::string_view namespace_name, std::string_view key) override {
        (void)namespace_name;
        (void)key;
        return nullptr;
    }

    DirectiveDef *find_directive_def(std::string_view type,
                                     std::string_view name,
                                     const std::vector<fiber::json::JsValue> &literals) override {
        (void)type;
        (void)name;
        (void)literals;
        return nullptr;
    }

private:
    TestFunction *func_ = nullptr;
    ThrowFunction *boom_ = nullptr;
    TestConstant *constant_ = nullptr;
};

fiber::script::ir::Compiled compile_script(std::string_view script, fiber::script::Library &library) {
    fiber::script::parse::Parser parser(library, true);
    auto parsed = parser.parse_script(script);
    EXPECT_TRUE(parsed.has_value()) << parsed.error().message;
    return fiber::script::ir::Compiler::compile(*parsed.value());
}

std::string value_to_string(const fiber::json::JsValue &value) {
    if (value.type_ == fiber::json::JsNodeType::NativeString) {
        return std::string(value.ns.data, value.ns.len);
    }
    if (value.type_ == fiber::json::JsNodeType::HeapString) {
        std::string out;
        auto *str = reinterpret_cast<const fiber::json::GcString *>(value.gc);
        if (fiber::json::gc_string_to_utf8(str, out)) {
            return out;
        }
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
    EXPECT_EQ(result.value().type_, fiber::json::JsNodeType::Integer);
    EXPECT_EQ(result.value().i, 7);
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
