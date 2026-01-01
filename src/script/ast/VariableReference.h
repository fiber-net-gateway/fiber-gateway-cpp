#ifndef FIBER_SCRIPT_AST_VARIABLE_REFERENCE_H
#define FIBER_SCRIPT_AST_VARIABLE_REFERENCE_H

#include <string>

#include "MaybeLValue.h"

namespace fiber::script::ast {

class VariableReference : public MaybeLValue {
public:
    VariableReference(std::int32_t start, std::int32_t end, std::string name)
        : MaybeLValue(start, end), name_(std::move(name)) {
    }

    const std::string &name() const {
        return name_;
    }

    bool is_root() const {
        return name_ == "$";
    }

    void mark_ref_const() {
        ref_const_ = true;
    }

    bool ref_const() const {
        return ref_const_;
    }

private:
    std::string name_;
    bool ref_const_ = false;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_VARIABLE_REFERENCE_H
