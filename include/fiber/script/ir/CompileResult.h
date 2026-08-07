#ifndef FIBER_SCRIPT_IR_COMPILE_RESULT_H
#define FIBER_SCRIPT_IR_COMPILE_RESULT_H

#include <cstdint>
#include <expected>

namespace fiber::script::ir {

enum class CompileErrorReason : std::uint8_t {
    None,
    OutOfMemory,
    ProgramTooLarge,
    OperandOutOfRange,
    JumpTargetOutOfRange,
    TooManyArguments,
    TooManyFunctions,
    TooManyConstants,
    PayloadTooLarge,
    UnsupportedConstant,
    InvalidHostCallable,
    Internal,
};

struct CompileError {
    CompileErrorReason reason = CompileErrorReason::None;
    std::int64_t position = -1;
    const char *message = nullptr;
};

template<typename T>
using CompileResult = std::expected<T, CompileError>;

} // namespace fiber::script::ir

#endif // FIBER_SCRIPT_IR_COMPILE_RESULT_H
