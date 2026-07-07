#include <gtest/gtest.h>

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "script/JsGc.h"
#include "script/Library.h"
#include "script/Script.h"
#include "script/ir/Compiler.h"
#include "script/parse/Parser.h"
#include "script/run/InterpreterVm.h"

namespace {

using fiber::script::JsValue;
using fiber::script::Library;
using fiber::script::ScriptResult;
using fiber::script::ScriptStatus;
using fiber::script::ValueHandle;

ScriptStatus test_function(void *userdata, const Library::HostCallFrame &frame, const Library::Arguments &arguments,
                           ValueHandle out) noexcept {
    (void) userdata;
    (void) frame;
    (void) arguments;
    *out = JsValue::make_integer(7);
    return ScriptStatus::success();
}

ScriptStatus throw_function(void *userdata, const Library::HostCallFrame &frame, const Library::Arguments &arguments,
                            ValueHandle out) noexcept {
    (void) userdata;
    (void) frame;
    (void) arguments;
    static char msg[] = "boom";
    *out = JsValue::make_native_string(msg, 4);
    return ScriptStatus::exception();
}

ScriptStatus test_constant(void *userdata, const Library::HostCallFrame &frame, ValueHandle out) noexcept {
    (void) userdata;
    (void) frame;
    *out = JsValue::make_integer(41);
    return ScriptStatus::success();
}

class DelayedAsyncFunction final {
public:
    struct Awaiter {
        DelayedAsyncFunction *self = nullptr;

        bool await_ready() const noexcept { return self && self->ready_; }

        void await_suspend(std::coroutine_handle<> handle) noexcept {
            if (self) {
                self->continuation_ = handle;
            }
        }

        JsValue await_resume() const noexcept { return self ? self->value_ : JsValue::make_undefined(); }
    };

    void complete_with(const fiber::script::JsValue &value) {
        value_ = value;
        ready_ = true;
        std::coroutine_handle<> continuation = continuation_;
        continuation_ = nullptr;
        if (continuation) {
            continuation.resume();
        }
    }

    Awaiter awaiter() noexcept { return Awaiter{this}; }

private:
    JsValue value_ = JsValue::make_undefined();
    std::coroutine_handle<> continuation_ = nullptr;
    bool ready_ = false;
};

fiber::script::AsyncTask delayed_async_function(void *userdata, const Library::HostCallFrame &frame,
                                                const Library::Arguments &arguments, ValueHandle out) noexcept {
    (void) frame;
    (void) arguments;
    auto *func = static_cast<DelayedAsyncFunction *>(userdata);
    JsValue value = co_await func->awaiter();
    *out = value;
    co_return ScriptStatus::success();
}

struct AddDefaultFunction {
    std::size_t observed_argc = 0;
};

ScriptStatus add_default_function(void *userdata, const Library::HostCallFrame &frame,
                                  const Library::Arguments &arguments, ValueHandle out) noexcept {
    (void) frame;
    auto *func = static_cast<AddDefaultFunction *>(userdata);
    if (func) {
        func->observed_argc = arguments.argc;
    }
    std::int64_t a = arguments.argc > 0 ? js_value_int64(arguments.args[0]) : 0;
    std::int64_t b = arguments.argc > 1 ? js_value_int64(arguments.args[1]) : 0;
    *out = JsValue::make_integer(a + b);
    return ScriptStatus::success();
}

Library::HostCallable make_sync_function(Library::Function function, void *userdata = nullptr) noexcept {
    Library::HostCallable callable;
    callable.kind = Library::HostCallable::Kind::SyncFunction;
    callable.userdata = userdata;
    callable.function = function;
    return callable;
}

Library::HostCallable make_async_function(Library::AsyncFunction function, void *userdata = nullptr) noexcept {
    Library::HostCallable callable;
    callable.kind = Library::HostCallable::Kind::AsyncFunction;
    callable.userdata = userdata;
    callable.async_function = function;
    return callable;
}

Library::HostCallable make_sync_constant(Library::Constant constant, void *userdata = nullptr) noexcept {
    Library::HostCallable callable;
    callable.kind = Library::HostCallable::Kind::SyncConstant;
    callable.userdata = userdata;
    callable.constant = constant;
    return callable;
}

class TestLibrary final : public Library {
public:
    explicit TestLibrary(DelayedAsyncFunction *async_func = nullptr) :
        func_(make_sync_function(&test_function)), boom_(make_sync_function(&throw_function)),
        constant_(make_sync_constant(&test_constant)),
        async_func_(make_async_function(&delayed_async_function, async_func)) {}

    FunctionMatchResult resolve_func(std::string_view name, const FunctionMatchRequest &request) const override {
        const HostCallable *func = nullptr;
        std::uint16_t argc = 0;
        if (name == "func") {
            func = &func_;
        } else if (name == "boom") {
            func = &boom_;
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
        return FunctionMatchResult::found(func, signature, nullptr, 0);
    }

    FunctionMatchResult resolve_async_func(std::string_view name, const FunctionMatchRequest &request) const override {
        if (name == "asyncFunc") {
            if (request.has_spread || request.known_argc != 1) {
                return FunctionMatchResult::arity_mismatch();
            }
            FunctionSignature signature;
            signature.required_argc = 1;
            signature.fixed_argc = 1;
            signature.variadic = false;
            return FunctionMatchResult::found(&async_func_, signature, nullptr, 0);
        }
        return FunctionMatchResult::not_found();
    }

    const HostCallable *resolve_constant(std::string_view namespace_name, std::string_view key) const override {
        if (namespace_name == "$test" && key == "answer") {
            return &constant_;
        }
        return nullptr;
    }

    const HostCallable *resolve_async_constant(std::string_view namespace_name, std::string_view key) const override {
        (void) namespace_name;
        (void) key;
        return nullptr;
    }

    DirectiveDef *resolve_directive_def(std::string_view type, std::string_view name,
                                        const std::vector<fiber::script::JsValue> &literals) const override {
        (void) type;
        (void) name;
        (void) literals;
        return nullptr;
    }

private:
    HostCallable func_{};
    HostCallable boom_{};
    HostCallable constant_{};
    HostCallable async_func_{};
};

class SignatureTestLibrary final : public Library {
public:
    explicit SignatureTestLibrary(AddDefaultFunction *func) : func_(make_sync_function(&add_default_function, func)) {}

    FunctionMatchResult resolve_func(std::string_view name, const FunctionMatchRequest &request) const override {
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
        const fiber::script::JsValue *defaults = default_count == 0 ? nullptr : defaults_;
        return FunctionMatchResult::found(&func_, signature, defaults, default_count);
    }

    FunctionMatchResult resolve_async_func(std::string_view name, const FunctionMatchRequest &request) const override {
        (void) name;
        (void) request;
        return FunctionMatchResult::not_found();
    }

    const HostCallable *resolve_constant(std::string_view namespace_name, std::string_view key) const override {
        (void) namespace_name;
        (void) key;
        return nullptr;
    }

    const HostCallable *resolve_async_constant(std::string_view namespace_name, std::string_view key) const override {
        (void) namespace_name;
        (void) key;
        return nullptr;
    }

    DirectiveDef *resolve_directive_def(std::string_view type, std::string_view name,
                                        const std::vector<fiber::script::JsValue> &literals) const override {
        (void) type;
        (void) name;
        (void) literals;
        return nullptr;
    }

private:
    HostCallable func_{};
    fiber::script::JsValue defaults_[1] = {fiber::script::JsValue::make_integer(1)};
};

fiber::script::ir::Compiled compile_script(std::string_view script, fiber::script::Library &library) {
    fiber::script::parse::Parser parser(library, true);
    auto parsed = parser.parse_script(script);
    EXPECT_TRUE(parsed.has_value()) << parsed.error().message;
    auto compiled = fiber::script::ir::Compiler::compile(*parsed.value());
    if (!compiled) {
        ADD_FAILURE() << (compiled.error().message ? compiled.error().message : "compile failed");
        return {};
    }
    return std::move(compiled.value());
}

std::string value_to_string(const fiber::script::JsValue &value) {
    if (js_value_type(value) != fiber::script::JsNodeType::String) {
        return {};
    }
    if (js_value_is_borrowed_string(value)) {
        return std::string(js_value_native_string(value).data, js_value_native_string(value).len);
    }
    std::string out;
    auto *str = js_value_heap_ptr<const fiber::script::GcString>(value);
    if (fiber::script::gc_string_to_utf8(str, out)) {
        return out;
    }
    return {};
}

} // namespace

TEST(ScriptExecutionTest, RunSimpleReturn) {
    TestLibrary library;

    auto compiled = compile_script("return 1 + 2 * 3;", library);
    auto compiled_ptr = std::make_shared<fiber::script::ir::Compiled>(std::move(compiled));
    fiber::script::Script script(compiled_ptr);

    fiber::script::GcHeap heap;
    auto run = script.exec_sync(fiber::script::JsValue::make_undefined(), nullptr, heap);
    auto result = run();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(js_value_type(result.value()), fiber::script::JsNodeType::Integer);
    EXPECT_EQ(js_value_int64(result.value()), 7);
}

TEST(ScriptExecutionTest, RunThrowLiteral) {
    TestLibrary library;

    auto compiled = compile_script("throw \"oops\";", library);
    auto compiled_ptr = std::make_shared<fiber::script::ir::Compiled>(std::move(compiled));
    fiber::script::Script script(compiled_ptr);

    fiber::script::GcHeap heap;
    auto run = script.exec_sync(fiber::script::JsValue::make_undefined(), nullptr, heap);
    auto result = run();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(value_to_string(result.error()), "oops");
}

TEST(ScriptExecutionTest, RunFunctionThrowCaught) {
    TestLibrary library;

    auto compiled = compile_script("try { boom(); return 0; } catch (e) { return e; }", library);
    auto compiled_ptr = std::make_shared<fiber::script::ir::Compiled>(std::move(compiled));
    fiber::script::Script script(compiled_ptr);

    fiber::script::GcHeap heap;
    auto run = script.exec_sync(fiber::script::JsValue::make_undefined(), nullptr, heap);
    auto result = run();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(value_to_string(result.value()), "boom");
}

TEST(ScriptExecutionTest, RethrowFromNestedCatchReachesOuterCatch) {
    TestLibrary library;

    auto compiled = compile_script("try { try { throw \"inner\"; } catch (e) { throw \"outer\"; } } "
                                   "catch (e) { return e; }",
                                   library);
    auto compiled_ptr = std::make_shared<fiber::script::ir::Compiled>(std::move(compiled));
    fiber::script::Script script(compiled_ptr);

    fiber::script::GcHeap heap;
    auto run = script.exec_sync(fiber::script::JsValue::make_undefined(), nullptr, heap);
    auto result = run();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(value_to_string(result.value()), "outer");
}

TEST(ScriptExecutionTest, AsyncFunctionSuspendsAndResumes) {
    DelayedAsyncFunction async_func;
    TestLibrary library(&async_func);

    auto compiled = compile_script("return asyncFunc(1);", library);

    fiber::script::GcHeap heap;
    fiber::script::run::InterpreterVm vm(compiled, fiber::script::JsValue::make_undefined(), nullptr, heap);

    vm.iterate();
    ASSERT_FALSE(vm.done());
    ASSERT_TRUE(vm.async_task().valid());
    std::coroutine_handle<> handle = vm.async_task().swap_coroutine_handle(std::noop_coroutine());
    handle.resume();

    async_func.complete_with(fiber::script::JsValue::make_integer(9));

    vm.iterate();
    ASSERT_TRUE(vm.done());
    ASSERT_TRUE(vm.result().has_value());
    EXPECT_EQ(js_value_type(vm.result().value()), fiber::script::JsNodeType::Integer);
    EXPECT_EQ(js_value_int64(vm.result().value()), 9);
}

TEST(ScriptExecutionTest, FunctionDefaultArgumentIsAppendedBeforeHostCall) {
    AddDefaultFunction func;
    SignatureTestLibrary library(&func);

    auto compiled = compile_script("return addDefault(3);", library);
    auto compiled_ptr = std::make_shared<fiber::script::ir::Compiled>(std::move(compiled));
    fiber::script::Script script(compiled_ptr);

    fiber::script::GcHeap heap;
    auto run = script.exec_sync(fiber::script::JsValue::make_undefined(), nullptr, heap);
    auto result = run();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(js_value_int64(result.value()), 4);
    EXPECT_EQ(func.observed_argc, 2u);
}
