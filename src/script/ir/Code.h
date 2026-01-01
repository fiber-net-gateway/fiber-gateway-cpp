#ifndef FIBER_SCRIPT_IR_CODE_H
#define FIBER_SCRIPT_IR_CODE_H

#include <cstdint>

namespace fiber::script::ir {

struct Code {
    static constexpr std::uint8_t NOOP = 1;
    static constexpr std::uint8_t LOAD_CONST = 3;
    static constexpr std::uint8_t LOAD_ROOT = 4;
    static constexpr std::uint8_t DUMP = 5;
    static constexpr std::uint8_t POP = 6;
    static constexpr std::uint8_t LOAD_VAR = 7;
    static constexpr std::uint8_t STORE_VAR = 8;

    static constexpr std::uint8_t NEW_OBJECT = 10;
    static constexpr std::uint8_t NEW_ARRAY = 11;
    static constexpr std::uint8_t EXP_OBJECT = 12;
    static constexpr std::uint8_t EXP_ARRAY = 13;
    static constexpr std::uint8_t PUSH_ARRAY = 14;

    static constexpr std::uint8_t IDX_GET = 15;
    static constexpr std::uint8_t IDX_SET = 16;
    static constexpr std::uint8_t IDX_SET_1 = 17;
    static constexpr std::uint8_t PROP_GET = 18;
    static constexpr std::uint8_t PROP_SET = 19;
    static constexpr std::uint8_t PROP_SET_1 = 20;

    static constexpr std::uint8_t BOP_PLUS = 25;
    static constexpr std::uint8_t BOP_MINUS = 26;
    static constexpr std::uint8_t BOP_MULTIPLY = 27;
    static constexpr std::uint8_t BOP_DIVIDE = 28;
    static constexpr std::uint8_t BOP_MOD = 29;
    static constexpr std::uint8_t BOP_MATCH = 30;

    static constexpr std::uint8_t BOP_LT = 31;
    static constexpr std::uint8_t BOP_LTE = 32;
    static constexpr std::uint8_t BOP_GT = 33;
    static constexpr std::uint8_t BOP_GTE = 34;
    static constexpr std::uint8_t BOP_EQ = 35;
    static constexpr std::uint8_t BOP_SEQ = 36;
    static constexpr std::uint8_t BOP_NE = 37;
    static constexpr std::uint8_t BOP_SNE = 38;
    static constexpr std::uint8_t BOP_IN = 39;

    static constexpr std::uint8_t UNARY_PLUS = 43;
    static constexpr std::uint8_t UNARY_MINUS = 44;
    static constexpr std::uint8_t UNARY_NEG = 45;
    static constexpr std::uint8_t UNARY_TYPEOF = 46;

    static constexpr std::uint8_t CALL_FUNC = 50;
    static constexpr std::uint8_t CALL_FUNC_SPREAD = 51;
    static constexpr std::uint8_t CALL_ASYNC_FUNC = 52;
    static constexpr std::uint8_t CALL_ASYNC_FUNC_SPREAD = 53;

    static constexpr std::uint8_t CALL_CONST = 56;
    static constexpr std::uint8_t CALL_ASYNC_CONST = 57;

    static constexpr std::uint8_t JUMP = 60;
    static constexpr std::uint8_t JUMP_IF_FALSE = 61;
    static constexpr std::uint8_t JUMP_IF_TRUE = 62;

    static constexpr std::uint8_t ITERATE_INTO = 65;
    static constexpr std::uint8_t ITERATE_NEXT = 66;
    static constexpr std::uint8_t ITERATE_KEY = 67;
    static constexpr std::uint8_t ITERATE_VALUE = 68;

    static constexpr std::uint8_t INTO_CATCH = 69;

    static constexpr std::uint8_t THROW_EXP = 75;
    static constexpr std::uint8_t END_RETURN = 76;
};

} // namespace fiber::script::ir

#endif // FIBER_SCRIPT_IR_CODE_H
