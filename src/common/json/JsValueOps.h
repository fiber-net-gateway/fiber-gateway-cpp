//
// Created by dear on 2025/12/30.
//

#ifndef FIBER_JSVALUEOPS_H
#define FIBER_JSVALUEOPS_H

#include <cstdint>

#include "JsNode.h"
#include "JsGc.h"

namespace fiber::json {

enum class JsUnaryOp : std::uint8_t {
    Plus,
    Negate,
    LogicalNot,
};

enum class JsBinaryOp : std::uint8_t {
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Eq,
    Ne,
    StrictEq,
    StrictNe,
    Lt,
    Le,
    Gt,
    Ge,
    LogicalAnd,
    LogicalOr,
};

enum class JsOpError : std::uint8_t {
    None,
    TypeError,
    DivisionByZero,
    HeapRequired,
    OutOfMemory,
    InvalidUtf8,
};

struct JsOpResult {
    JsValue value;
    JsOpError error = JsOpError::None;
};

[[nodiscard]] JsOpResult js_unary_op(JsUnaryOp op, const JsValue &value);
[[nodiscard]] JsOpResult js_binary_op(JsBinaryOp op, const JsValue &lhs, const JsValue &rhs, GcHeap *heap);

} // namespace fiber::json

#endif // FIBER_JSVALUEOPS_H
