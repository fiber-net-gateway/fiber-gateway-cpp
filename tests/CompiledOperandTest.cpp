#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string_view>
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

std::size_t find_first_opcode(const Compiled &compiled, std::uint8_t opcode) {
    for (std::size_t i = 0; i < compiled.codes.size(); ++i) {
        if (static_cast<std::uint8_t>(compiled.codes[i] & 0xFF) == opcode) {
            return i;
        }
    }
    return compiled.codes.size();
}

} // namespace

TEST(CompiledOperandTest, CompilerEmitsTypedOperands) {
    OperandLibrary library;
    Compiled compiled = compile_script("root.foo; syncFn(1); asyncFn(2); $env.value; $env.asyncValue;", library);

    const std::size_t prop_get = find_first_opcode(compiled, Code::PROP_GET);
    const std::size_t call_func = find_first_opcode(compiled, Code::CALL_FUNC);
    const std::size_t call_async_func = find_first_opcode(compiled, Code::CALL_ASYNC_FUNC);
    const std::size_t call_const = find_first_opcode(compiled, Code::CALL_CONST);
    const std::size_t call_async_const = find_first_opcode(compiled, Code::CALL_ASYNC_CONST);

    ASSERT_LT(prop_get, compiled.codes.size());
    ASSERT_LT(call_func, compiled.codes.size());
    ASSERT_LT(call_async_func, compiled.codes.size());
    ASSERT_LT(call_const, compiled.codes.size());
    ASSERT_LT(call_async_const, compiled.codes.size());

    EXPECT_EQ(compiled.operand_at(operand_index_for_code(compiled.codes[prop_get])).kind,
              Compiled::OperandKind::InternedString);
    EXPECT_EQ(
            compiled.host_symbol_at(
                            compiled.call_site_at(operand_index_for_code(compiled.codes[call_func])).host_symbol_index)
                    .kind,
            Library::HostCallable::Kind::SyncFunction);
    EXPECT_EQ(compiled.host_symbol_at(compiled.call_site_at(operand_index_for_code(compiled.codes[call_async_func]))
                                              .host_symbol_index)
                      .kind,
              Library::HostCallable::Kind::AsyncFunction);
    EXPECT_EQ(
            compiled.host_symbol_at(
                            compiled.call_site_at(operand_index_for_code(compiled.codes[call_const])).host_symbol_index)
                    .kind,
            Library::HostCallable::Kind::SyncConstant);
    EXPECT_EQ(compiled.host_symbol_at(compiled.call_site_at(operand_index_for_code(compiled.codes[call_async_const]))
                                              .host_symbol_index)
                      .kind,
              Library::HostCallable::Kind::AsyncConstant);
    EXPECT_TRUE(compiled.validate_operands());
}

TEST(CompiledOperandTest, ValidateOperandsRejectsKindMismatch) {
    OperandLibrary library;
    Compiled compiled = compile_script("root.foo;", library);

    const std::size_t prop_get = find_first_opcode(compiled, Code::PROP_GET);
    ASSERT_LT(prop_get, compiled.codes.size());

    const std::size_t operand_index = operand_index_for_code(compiled.codes[prop_get]);
    ASSERT_LT(operand_index, compiled.operands.size());
    compiled.operands[operand_index].kind = Compiled::OperandKind::ConstValue;

    EXPECT_FALSE(compiled.validate_operands());
}
