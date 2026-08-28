#ifndef FIBER_SCRIPT_RUN_COMPARES_H
#define FIBER_SCRIPT_RUN_COMPARES_H

#include <fiber/script/JsGc.h>

namespace fiber::script::run {

class Compares {
public:
    static bool neg(ConstValueHandle value) noexcept;
    static bool logic(ConstValueHandle value) noexcept;

    static bool eq(ConstValueHandle a, ConstValueHandle b) noexcept;
    static bool seq(ConstValueHandle a, ConstValueHandle b) noexcept;
    static bool ne(ConstValueHandle a, ConstValueHandle b) noexcept;
    static bool sne(ConstValueHandle a, ConstValueHandle b) noexcept;

    static bool lt(ConstValueHandle a, ConstValueHandle b) noexcept;
    static bool lte(ConstValueHandle a, ConstValueHandle b) noexcept;
    static bool gt(ConstValueHandle a, ConstValueHandle b) noexcept;
    static bool gte(ConstValueHandle a, ConstValueHandle b) noexcept;

    static bool matches(ConstValueHandle a, ConstValueHandle b) noexcept;
    static bool in(ConstValueHandle a, ConstValueHandle b) noexcept;
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_COMPARES_H
