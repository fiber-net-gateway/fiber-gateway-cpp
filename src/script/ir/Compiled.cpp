#include "Compiled.h"

#include <cstdint>

#include "../../common/Assert.h"

namespace fiber::script::ir {

namespace {

std::size_t operand_index_for_code(std::uint8_t op, std::uint32_t raw_code) {
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
            return true;
        default:
            return false;
    }
}

bool code_uses_call_site(std::uint8_t op) {
    switch (op) {
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

const Compiled::HostSymbol &Compiled::host_symbol_at(std::size_t index) const {
    FIBER_ASSERT(index < host_symbols.size());
    return host_symbols[index];
}

const Compiled::CallSite &Compiled::call_site_at(std::size_t index) const {
    FIBER_ASSERT(index < call_sites.size());
    return call_sites[index];
}

bool Compiled::validate_operands() const {
    for (const Operand &operand: operands) {
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
        }
    }

    for (const HostSymbol &symbol: host_symbols) {
        if (!symbol.callable) {
            return false;
        }
        if (symbol.kind != symbol.callable->kind) {
            return false;
        }
        switch (symbol.kind) {
            case Library::HostCallable::Kind::SyncFunction:
            case Library::HostCallable::Kind::SyncConstant:
                if (!symbol.callable->sync) {
                    return false;
                }
                break;
            case Library::HostCallable::Kind::AsyncFunction:
            case Library::HostCallable::Kind::AsyncConstant:
                if (!symbol.callable->async) {
                    return false;
                }
                break;
        }
    }

    for (const CallSite &site: call_sites) {
        if (site.host_symbol_index >= host_symbols.size()) {
            return false;
        }
        if ((site.flags & CallSiteSpreadArgs) != 0 && site.argc != 0) {
            return false;
        }
    }

    for (std::int32_t code: codes) {
        const std::uint32_t raw_code = static_cast<std::uint32_t>(code);
        const std::uint8_t op = static_cast<std::uint8_t>(raw_code & 0xFF);
        if (code_uses_operand(op)) {
            const std::size_t operand_index = operand_index_for_code(op, raw_code);
            if (operand_index >= operands.size()) {
                return false;
            }
            if (operands[operand_index].kind != expected_operand_kind(op)) {
                return false;
            }
        }
        if (code_uses_call_site(op)) {
            const std::size_t call_site_index = operand_index_for_code(op, raw_code);
            if (call_site_index >= call_sites.size()) {
                return false;
            }
            const CallSite &site = call_sites[call_site_index];
            if (site.host_symbol_index >= host_symbols.size()) {
                return false;
            }
        }
    }
    return true;
}

} // namespace fiber::script::ir
