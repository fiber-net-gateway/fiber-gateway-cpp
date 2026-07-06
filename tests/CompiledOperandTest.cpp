#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include "script/Library.h"
#include "script/ir/Code.h"
#include "script/ir/Compiler.h"
#include "script/parse/Parser.h"

namespace {

using fiber::json::JsValue;
using fiber::script::Library;
using fiber::script::ScriptResult;
using fiber::script::ScriptStatus;
using fiber::script::ValueHandle;
using fiber::script::ir::Code;
using fiber::script::ir::Compiled;

ScriptStatus dummy_function(void *userdata, const Library::HostCallFrame &frame, const Library::Arguments &arguments,
                            ValueHandle out) noexcept {
    (void) userdata;
    (void) frame;
    (void) arguments;
    *out = JsValue::make_undefined();
    return ScriptStatus::success();
}

fiber::script::AsyncTask dummy_async_function(void *userdata, const Library::HostCallFrame &frame,
                                              const Library::Arguments &arguments, ValueHandle out) noexcept {
    (void) userdata;
    (void) frame;
    (void) arguments;
    *out = JsValue::make_undefined();
    co_return ScriptStatus::success();
}

ScriptStatus dummy_constant(void *userdata, const Library::HostCallFrame &frame, ValueHandle out) noexcept {
    (void) userdata;
    (void) frame;
    *out = JsValue::make_undefined();
    return ScriptStatus::success();
}

fiber::script::AsyncTask dummy_async_constant(void *userdata, const Library::HostCallFrame &frame,
                                              ValueHandle out) noexcept {
    (void) userdata;
    (void) frame;
    *out = JsValue::make_undefined();
    co_return ScriptStatus::success();
}

class OperandLibrary final : public Library {
public:
    OperandLibrary() {
        sync_fn_.kind = HostCallable::Kind::SyncFunction;
        sync_fn_.function = &dummy_function;
        async_fn_.kind = HostCallable::Kind::AsyncFunction;
        async_fn_.async_function = &dummy_async_function;
        constant_.kind = HostCallable::Kind::SyncConstant;
        constant_.constant = &dummy_constant;
        async_constant_.kind = HostCallable::Kind::AsyncConstant;
        async_constant_.async_constant = &dummy_async_constant;
    }

    FunctionMatchResult resolve_func(std::string_view name, const FunctionMatchRequest &request) const override {
        if (name == "syncFn") {
            FunctionSignature signature;
            signature.required_argc = 1;
            signature.fixed_argc = 1;
            signature.variadic = false;
            if (request.known_argc != 1 || request.has_spread) {
                return FunctionMatchResult::arity_mismatch();
            }
            return FunctionMatchResult::found(&sync_fn_, signature, nullptr, 0);
        }
        return FunctionMatchResult::not_found();
    }

    FunctionMatchResult resolve_async_func(std::string_view name, const FunctionMatchRequest &request) const override {
        if (name == "asyncFn") {
            FunctionSignature signature;
            signature.required_argc = 1;
            signature.fixed_argc = 1;
            signature.variadic = false;
            if (request.known_argc != 1 || request.has_spread) {
                return FunctionMatchResult::arity_mismatch();
            }
            return FunctionMatchResult::found(&async_fn_, signature, nullptr, 0);
        }
        return FunctionMatchResult::not_found();
    }

    const HostCallable *resolve_constant(std::string_view namespace_name, std::string_view key) const override {
        if (namespace_name == "$env" && key == "value") {
            return &constant_;
        }
        return nullptr;
    }

    const HostCallable *resolve_async_constant(std::string_view namespace_name, std::string_view key) const override {
        if (namespace_name == "$env" && key == "asyncValue") {
            return &async_constant_;
        }
        return nullptr;
    }

    DirectiveDef *resolve_directive_def(std::string_view type, std::string_view name,
                                        const std::vector<JsValue> &literals) const override {
        (void) type;
        (void) name;
        (void) literals;
        return nullptr;
    }

private:
    HostCallable sync_fn_{};
    HostCallable async_fn_{};
    HostCallable constant_{};
    HostCallable async_constant_{};
};

Compiled compile_script(std::string_view script, Library &library) {
    fiber::script::parse::Parser parser(library, true);
    auto parsed = parser.parse_script(script);
    EXPECT_TRUE(parsed.has_value()) << parsed.error().message;
    return fiber::script::ir::Compiler::compile(*parsed.value());
}

std::size_t operand_index_for_code(std::int32_t code) {
    const auto raw = static_cast<std::uint32_t>(code);
    return static_cast<std::size_t>(raw >> 8);
}

std::size_t func_index_for_call(std::int32_t code) {
    const auto raw = static_cast<std::uint32_t>(code);
    std::uint8_t op = static_cast<std::uint8_t>(raw & 0xFFu);
    if (op == Code::CALL_FUNC || op == Code::CALL_ASYNC_FUNC) {
        return static_cast<std::size_t>(raw >> 16u);
    }
    return static_cast<std::size_t>(raw >> 8u);
}

std::uint32_t argc_for_call(std::int32_t code) {
    const auto raw = static_cast<std::uint32_t>(code);
    return (raw >> 8u) & 0xFFu;
}

std::size_t find_first_opcode(const Compiled &compiled, std::uint8_t opcode) {
    for (std::size_t i = 0; i < compiled.code_size(); ++i) {
        if (static_cast<std::uint8_t>(compiled.codes()[i] & 0xFF) == opcode) {
            return i;
        }
    }
    return compiled.code_size();
}

} // namespace

TEST(CompiledOperandTest, CompilerEmitsDirectConstantsAndFuncConsts) {
    OperandLibrary library;
    Compiled compiled = compile_script("root.foo; syncFn(1); asyncFn(2); $env.value; $env.asyncValue;", library);

    const std::size_t prop_get = find_first_opcode(compiled, Code::PROP_GET);
    const std::size_t call_func = find_first_opcode(compiled, Code::CALL_FUNC);
    const std::size_t call_async_func = find_first_opcode(compiled, Code::CALL_ASYNC_FUNC);
    const std::size_t call_const = find_first_opcode(compiled, Code::CALL_CONST);
    const std::size_t call_async_const = find_first_opcode(compiled, Code::CALL_ASYNC_CONST);

    ASSERT_LT(prop_get, compiled.code_size());
    ASSERT_LT(call_func, compiled.code_size());
    ASSERT_LT(call_async_func, compiled.code_size());
    ASSERT_LT(call_const, compiled.code_size());
    ASSERT_LT(call_async_const, compiled.code_size());

    const JsValue &prop_name = compiled.constant(operand_index_for_code(compiled.codes()[prop_get]));
    ASSERT_EQ(fiber::json::js_value_type(prop_name), fiber::json::JsNodeType::String);
    fiber::json::NativeStr name = fiber::json::js_value_native_string(prop_name);
    EXPECT_EQ(std::string_view(name.data, name.len), "foo");

    std::int32_t sync_call_code = compiled.codes()[call_func];
    EXPECT_EQ(argc_for_call(sync_call_code), 1u);
    EXPECT_EQ(compiled.func_const(func_index_for_call(sync_call_code)).sync_func, &dummy_function);

    std::int32_t async_call_code = compiled.codes()[call_async_func];
    EXPECT_EQ(argc_for_call(async_call_code), 1u);
    EXPECT_EQ(compiled.func_const(func_index_for_call(async_call_code)).async_func, &dummy_async_function);

    EXPECT_EQ(compiled.func_const(func_index_for_call(compiled.codes()[call_const])).sync_ct, &dummy_constant);
    EXPECT_EQ(compiled.func_const(func_index_for_call(compiled.codes()[call_async_const])).async_ct,
              &dummy_async_constant);
}

TEST(CompiledOperandTest, MoveKeepsBorrowedConstantPayloadStable) {
    OperandLibrary library;
    Compiled compiled = compile_script("root.foo;", library);
    Compiled moved = std::move(compiled);

    const std::size_t prop_get = find_first_opcode(moved, Code::PROP_GET);
    ASSERT_LT(prop_get, moved.code_size());

    const JsValue &prop_name = moved.constant(operand_index_for_code(moved.codes()[prop_get]));
    ASSERT_EQ(fiber::json::js_value_type(prop_name), fiber::json::JsNodeType::String);
    fiber::json::NativeStr name = fiber::json::js_value_native_string(prop_name);
    EXPECT_EQ(std::string_view(name.data, name.len), "foo");
}
