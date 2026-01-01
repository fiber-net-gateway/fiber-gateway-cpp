#ifndef FIBER_SCRIPT_AST_LITERAL_H
#define FIBER_SCRIPT_AST_LITERAL_H

#include <cstdint>
#include <string>

#include "Expression.h"

namespace fiber::script::ast {

class Literal : public Expression {
public:
    enum class Kind : std::uint8_t {
        NullValue,
        Boolean,
        Integer,
        Float,
        String,
    };

    Literal() = default;

    static Literal make_null(std::int32_t start, std::int32_t end) {
        Literal literal;
        literal.start_ = start;
        literal.end_ = end;
        literal.kind_ = Kind::NullValue;
        return literal;
    }

    Literal(std::int32_t start, std::int32_t end, bool value)
        : Expression(start, end), kind_(Kind::Boolean), bool_value_(value) {
    }

    Literal(std::int32_t start, std::int32_t end, std::int64_t value)
        : Expression(start, end), kind_(Kind::Integer), int_value_(value) {
    }

    Literal(std::int32_t start, std::int32_t end, double value)
        : Expression(start, end), kind_(Kind::Float), float_value_(value) {
    }

    Literal(std::int32_t start, std::int32_t end, std::string value)
        : Expression(start, end), kind_(Kind::String), string_value_(std::move(value)) {
    }

    Kind kind() const {
        return kind_;
    }

    bool bool_value() const {
        return bool_value_;
    }

    std::int64_t int_value() const {
        return int_value_;
    }

    double float_value() const {
        return float_value_;
    }

    const std::string &string_value() const {
        return string_value_;
    }

private:
    Kind kind_ = Kind::NullValue;
    bool bool_value_ = false;
    std::int64_t int_value_ = 0;
    double float_value_ = 0.0;
    std::string string_value_;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_LITERAL_H
