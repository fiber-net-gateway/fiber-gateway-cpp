#ifndef FIBER_SCRIPT_IR_COMPILED_H
#define FIBER_SCRIPT_IR_COMPILED_H

#include <cstdint>
#include <vector>

#include "Code.h"

namespace fiber::script::ir {

struct Compiled {
    std::size_t stack_size = 0;
    std::size_t var_table_size = 0;
    std::vector<std::int64_t> positions;
    std::vector<std::int32_t> codes;
    std::vector<void *> operands;
    std::vector<std::int32_t> exception_table;

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

} // namespace fiber::script::ir

#endif // FIBER_SCRIPT_IR_COMPILED_H
