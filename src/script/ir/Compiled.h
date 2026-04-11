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
    };

    struct Operand {
        OperandKind kind = OperandKind::ConstValue;
        std::uint8_t reserved0 = 0;
        std::uint16_t reserved1 = 0;
        std::uint32_t aux = 0;
        std::uintptr_t payload = 0;
    };

    struct HostSymbol {
        Library::HostCallable::Kind kind = Library::HostCallable::Kind::SyncFunction;
        std::uint32_t flags = 0;
        const Library::HostCallable *callable = nullptr;
    };

    enum CallSiteFlags : std::uint16_t {
        CallSiteNone = 0,
        CallSiteSpreadArgs = 1u << 0,
    };

    struct CallSite {
        std::uint32_t host_symbol_index = 0;
        std::uint16_t argc = 0;
        std::uint16_t flags = CallSiteNone;
        std::int64_t position = -1;
    };

    std::size_t stack_size = 0;
    std::size_t var_table_size = 0;
    std::vector<std::int64_t> positions;
    std::vector<std::int32_t> codes;
    std::vector<Operand> operands;
    std::vector<HostSymbol> host_symbols;
    std::vector<CallSite> call_sites;
    std::vector<std::unique_ptr<ConstValue>> const_pool;
    std::vector<std::unique_ptr<std::string>> string_pool;
    std::vector<std::int32_t> exception_table;

    const Operand &operand_at(std::size_t index) const;
    const ConstValue *operand_const(std::size_t index) const;
    const std::string *operand_string(std::size_t index) const;
    const HostSymbol &host_symbol_at(std::size_t index) const;
    const CallSite &call_site_at(std::size_t index) const;
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
