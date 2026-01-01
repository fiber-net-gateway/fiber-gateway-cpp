#ifndef FIBER_SCRIPT_AST_INLINE_OBJECT_H
#define FIBER_SCRIPT_AST_INLINE_OBJECT_H

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Expression.h"

namespace fiber::script::ast {

class InlineObject : public Expression {
public:
    enum class KeyKind : std::uint8_t {
        String,
        Expression,
        Expand,
    };

    struct Key {
        KeyKind kind = KeyKind::String;
        std::string string_key;
        std::unique_ptr<Expression> expr_key;
    };

    struct Entry {
        Key key;
        std::unique_ptr<Expression> value;
    };

    InlineObject(std::int32_t start, std::int32_t end, std::vector<Entry> entries)
        : Expression(start, end), entries_(std::move(entries)) {
    }

    const std::vector<Entry> &entries() const {
        return entries_;
    }

private:
    std::vector<Entry> entries_;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_INLINE_OBJECT_H
