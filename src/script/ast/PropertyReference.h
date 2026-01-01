#ifndef FIBER_SCRIPT_AST_PROPERTY_REFERENCE_H
#define FIBER_SCRIPT_AST_PROPERTY_REFERENCE_H

#include <memory>
#include <string>

#include "MaybeLValue.h"

namespace fiber::script::ast {

class PropertyReference : public MaybeLValue {
public:
    PropertyReference(std::int32_t start,
                      std::int32_t end,
                      std::string name,
                      std::unique_ptr<Expression> parent)
        : MaybeLValue(start, end), name_(std::move(name)), parent_(std::move(parent)) {
    }

    const std::string &name() const {
        return name_;
    }

    const Expression *parent() const {
        return parent_.get();
    }

    std::unique_ptr<Expression> take_parent() {
        return std::move(parent_);
    }

private:
    std::string name_;
    std::unique_ptr<Expression> parent_;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_PROPERTY_REFERENCE_H
