#ifndef FIBER_SCRIPT_IR_COMPILED_H
#define FIBER_SCRIPT_IR_COMPILED_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "Code.h"
#include "../Library.h"

namespace fiber::script::ir {

struct Compiled {
    struct ConstValue {
        enum class Kind : std::uint8_t {
            Undefined,
            Null,
            Boolean,
            Integer,
            Float,
            String,
            Binary,
        };

        Kind kind = Kind::Undefined;
        bool bool_value = false;
        std::int64_t int_value = 0;
        double float_value = 0.0;
        std::string text;
        std::vector<std::uint8_t> bytes;
    };

    enum class OperandKind : std::uint8_t {
        ConstValue = 0,
        InternedString,
        Function,
        AsyncFunction,
        Constant,
        AsyncConstant,
    };

    struct Operand {
        OperandKind kind = OperandKind::ConstValue;
        std::uint8_t reserved0 = 0;
        std::uint16_t reserved1 = 0;
        std::uint32_t aux = 0;
        std::uintptr_t payload = 0;
    };

    std::size_t stack_size = 0;
    std::size_t var_table_size = 0;
    std::vector<std::int64_t> positions;
    std::vector<std::int32_t> codes;
    std::vector<Operand> operands;
    std::vector<std::unique_ptr<ConstValue>> const_pool;
    std::vector<std::unique_ptr<std::string>> string_pool;
    std::vector<std::int32_t> exception_table;

    const Operand &operand_at(std::size_t index) const;
    const ConstValue *operand_const(std::size_t index) const;
    const std::string *operand_string(std::size_t index) const;
    Library::Function *operand_function(std::size_t index) const;
    Library::AsyncFunction *operand_async_function(std::size_t index) const;
    Library::Constant *operand_constant(std::size_t index) const;
    Library::AsyncConstant *operand_async_constant(std::size_t index) const;
    bool validate_operands() const;

    bool contains_async() const {
        for (std::int32_t code : codes) {
            switch (code & 0xFF) {
                case Code::CALL_ASYNC_CONST:
                case Code::CALL_ASYNC_FUNC:
                case Code::CALL_ASYNC_FUNC_SPREAD:
                    return true;
                default:
                    break;
            }
        }
        return false;
    }
};

static_assert(std::is_trivially_copyable_v<Compiled::Operand>);

} // namespace fiber::script::ir

#endif // FIBER_SCRIPT_IR_COMPILED_H
