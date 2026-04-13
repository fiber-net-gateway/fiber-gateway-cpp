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
using fiber::script::AsyncExecutionContext;
using fiber::script::ExecutionContext;
using fiber::script::Library;
using fiber::script::ir::Code;
using fiber::script::ir::Compiled;

class DummyFunction final : public Library::Function {
public:
    Library::FunctionResult call(ExecutionContext &context) override {
        (void) context;
        return JsValue::make_undefined();
    }
};

class DummyAsyncFunction final : public Library::AsyncFunction {
public:
    void call(AsyncExecutionContext &context) override { context.return_value(JsValue::make_undefined()); }
};

class DummyConstant final : public Library::Constant {
public:
    Library::FunctionResult get(ExecutionContext &context) override {
        (void) context;
        return JsValue::make_undefined();
    }
};

class DummyAsyncConstant final : public Library::AsyncConstant {
public:
    void get(AsyncExecutionContext &context) override { context.return_value(JsValue::make_undefined()); }
};

class OperandLibrary final : public Library {
public:
    Function *find_func(std::string_view name) override {
        if (name == "syncFn") {
            return &sync_fn_;
        }
        return nullptr;
    }

    AsyncFunction *find_async_func(std::string_view name) override {
        if (name == "asyncFn") {
            return &async_fn_;
        }
        return nullptr;
    }

    Constant *find_constant(std::string_view namespace_name, std::string_view key) override {
        if (namespace_name == "$env" && key == "value") {
            return &constant_;
        }
        return nullptr;
    }

    AsyncConstant *find_async_constant(std::string_view namespace_name, std::string_view key) override {
        if (namespace_name == "$env" && key == "asyncValue") {
            return &async_constant_;
        }
        return nullptr;
    }

    DirectiveDef *find_directive_def(std::string_view type, std::string_view name,
                                     const std::vector<JsValue> &literals) override {
        (void) type;
        (void) name;
        (void) literals;
        return nullptr;
    }

private:
    DummyFunction sync_fn_;
    DummyAsyncFunction async_fn_;
    DummyConstant constant_;
    DummyAsyncConstant async_constant_;
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
