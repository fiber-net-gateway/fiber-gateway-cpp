#include "MathFuncs.h"

#include "StdLibrary.h"

#include "../JsValue.h"
#include "../Library.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace fiber::script::std_lib {

namespace {

// The runtime collapses Jackson's numeric node kinds onto two JsNodeType tags:
// Short/Int/Long/BigInteger -> Integer (int64), Float/Double/BigDecimal ->
// Float (double). Java's math.floor therefore maps integers to themselves and
// any floating value to a single int64 (Java narrowed float->int32 and
// double->int64; int64 is the only integral width here, so no int32 path).
// math.abs likewise has only the integer/double cases. Non-numeric input
// raises a catchable TypeError, mirroring Java's ScriptExecException.

ScriptResult type_error() noexcept {
    return ScriptResult::exception(JsValue::make_exception(ExceptionKind::TypeError));
}

ScriptResult floor_fn(void * /*userdata*/, const Library::HostCallFrame & /*frame*/, Library::Arguments args) noexcept {
    if (!args.args || args.argc < 1) {
        return type_error();
    }
    const JsValue &value = args.args[0];
    switch (js_value_type(value)) {
        case JsNodeType::Integer:
            // Integral input is returned unchanged, as in Java.
            return ScriptResult::success(value);
        case JsNodeType::Float:
            return ScriptResult::success(
                    JsValue::make_integer(static_cast<std::int64_t>(std::floor(js_value_double(value)))));
        default:
            return type_error();
    }
}

ScriptResult abs_fn(void * /*userdata*/, const Library::HostCallFrame & /*frame*/, Library::Arguments args) noexcept {
    if (!args.args || args.argc < 1) {
        return type_error();
    }
    const JsValue &value = args.args[0];
    switch (js_value_type(value)) {
        case JsNodeType::Integer: {
            const std::int64_t v = js_value_int64(value);
            // std::abs(INT64_MIN) is undefined; Java's Math.abs(Long.MIN_VALUE)
            // returns Long.MIN_VALUE unchanged, so preserve that behavior.
            if (v == std::numeric_limits<std::int64_t>::min()) {
                return ScriptResult::success(value);
            }
            return ScriptResult::success(JsValue::make_integer(std::abs(v)));
        }
        case JsNodeType::Float:
            return ScriptResult::success(JsValue::make_float(std::fabs(js_value_double(value))));
        default:
            return type_error();
    }
}

} // namespace

void register_math_funcs(StdLibrary &lib) {
    lib.register_func("math.floor", Library::FunctionSignature{.required_argc = 1, .fixed_argc = 1, .variadic = false},
                      &floor_fn, nullptr, "math.floor");
    lib.register_func("math.abs", Library::FunctionSignature{.required_argc = 1, .fixed_argc = 1, .variadic = false},
                      &abs_fn, nullptr, "math.abs");
}

} // namespace fiber::script::std_lib
