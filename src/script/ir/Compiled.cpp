#include "Compiled.h"

#include <cstdint>

#include "../../common/Assert.h"

namespace fiber::script::ir {

namespace {

std::size_t operand_index_for_code(std::uint8_t op, std::uint32_t raw_code) {
    switch (op) {
        case Code::CALL_FUNC:
        case Code::CALL_ASYNC_FUNC:
            return static_cast<std::size_t>(raw_code >> 16);
        case Code::LOAD_CONST:
        case Code::PROP_GET:
        case Code::PROP_SET:
        case Code::PROP_SET_1:
        case Code::CALL_FUNC_SPREAD:
        case Code::CALL_ASYNC_FUNC_SPREAD:
        case Code::CALL_CONST:
        case Code::CALL_ASYNC_CONST:
            return static_cast<std::size_t>(raw_code >> 8);
        default:
            return 0;
    }
}

Compiled::OperandKind expected_operand_kind(std::uint8_t op) {
    switch (op) {
        case Code::LOAD_CONST:
            return Compiled::OperandKind::ConstValue;
        case Code::PROP_GET:
        case Code::PROP_SET:
        case Code::PROP_SET_1:
            return Compiled::OperandKind::InternedString;
        case Code::CALL_FUNC:
        case Code::CALL_FUNC_SPREAD:
            return Compiled::OperandKind::Function;
        case Code::CALL_ASYNC_FUNC:
        case Code::CALL_ASYNC_FUNC_SPREAD:
            return Compiled::OperandKind::AsyncFunction;
        case Code::CALL_CONST:
            return Compiled::OperandKind::Constant;
        case Code::CALL_ASYNC_CONST:
            return Compiled::OperandKind::AsyncConstant;
        default:
            return Compiled::OperandKind::ConstValue;
    }
}

bool code_uses_operand(std::uint8_t op) {
    switch (op) {
        case Code::LOAD_CONST:
        case Code::PROP_GET:
        case Code::PROP_SET:
        case Code::PROP_SET_1:
        case Code::CALL_FUNC:
        case Code::CALL_FUNC_SPREAD:
        case Code::CALL_ASYNC_FUNC:
        case Code::CALL_ASYNC_FUNC_SPREAD:
        case Code::CALL_CONST:
        case Code::CALL_ASYNC_CONST:
            return true;
        default:
            return false;
    }
}

template <typename T>
T *operand_host_ptr(const Compiled::Operand &operand, Compiled::OperandKind expected_kind) {
    FIBER_ASSERT(operand.kind == expected_kind);
    return reinterpret_cast<T *>(operand.payload);
}

} // namespace

const Compiled::Operand &Compiled::operand_at(std::size_t index) const {
    FIBER_ASSERT(index < operands.size());
    return operands[index];
}

const Compiled::ConstValue *Compiled::operand_const(std::size_t index) const {
    const Operand &operand = operand_at(index);
    FIBER_ASSERT(operand.kind == OperandKind::ConstValue);
    const std::size_t pool_index = static_cast<std::size_t>(operand.payload);
    FIBER_ASSERT(pool_index < const_pool.size());
    return const_pool[pool_index].get();
}

const std::string *Compiled::operand_string(std::size_t index) const {
    const Operand &operand = operand_at(index);
    FIBER_ASSERT(operand.kind == OperandKind::InternedString);
    const std::size_t pool_index = static_cast<std::size_t>(operand.payload);
    FIBER_ASSERT(pool_index < string_pool.size());
    return string_pool[pool_index].get();
}

Library::Function *Compiled::operand_function(std::size_t index) const {
    return operand_host_ptr<Library::Function>(operand_at(index), OperandKind::Function);
}

Library::AsyncFunction *Compiled::operand_async_function(std::size_t index) const {
    return operand_host_ptr<Library::AsyncFunction>(operand_at(index), OperandKind::AsyncFunction);
}

Library::Constant *Compiled::operand_constant(std::size_t index) const {
    return operand_host_ptr<Library::Constant>(operand_at(index), OperandKind::Constant);
}

Library::AsyncConstant *Compiled::operand_async_constant(std::size_t index) const {
    return operand_host_ptr<Library::AsyncConstant>(operand_at(index), OperandKind::AsyncConstant);
}

bool Compiled::validate_operands() const {
    for (const Operand &operand : operands) {
        const std::size_t payload_index = static_cast<std::size_t>(operand.payload);
        switch (operand.kind) {
            case OperandKind::ConstValue:
                if (payload_index >= const_pool.size()) {
                    return false;
                }
                break;
            case OperandKind::InternedString:
                if (payload_index >= string_pool.size()) {
                    return false;
                }
                break;
            case OperandKind::Function:
            case OperandKind::AsyncFunction:
            case OperandKind::Constant:
            case OperandKind::AsyncConstant:
                if (operand.payload == 0) {
                    return false;
                }
                break;
        }
    }

    for (std::int32_t code : codes) {
        const std::uint32_t raw_code = static_cast<std::uint32_t>(code);
        const std::uint8_t op = static_cast<std::uint8_t>(raw_code & 0xFF);
        if (!code_uses_operand(op)) {
            continue;
        }
        const std::size_t operand_index = operand_index_for_code(op, raw_code);
        if (operand_index >= operands.size()) {
            return false;
        }
        if (operands[operand_index].kind != expected_operand_kind(op)) {
            return false;
        }
    }
    return true;
}

} // namespace fiber::script::ir
