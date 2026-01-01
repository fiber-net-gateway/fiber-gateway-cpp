#ifndef FIBER_SCRIPT_RUN_VM_ERROR_H
#define FIBER_SCRIPT_RUN_VM_ERROR_H

#include <cstdint>
#include <expected>
#include <string>

#include "../../common/json/JsNode.h"

namespace fiber::script::run {

enum class VmErrorKind : std::uint8_t {
    Normal,
    Thrown
};

struct VmError {
    VmErrorKind kind = VmErrorKind::Normal;
    std::string name;
    std::string message;
    int status = 500;
    std::int64_t position = -1;
    fiber::json::JsValue meta = fiber::json::JsValue::make_undefined();
};

using VmResult = std::expected<fiber::json::JsValue, VmError>;

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_VM_ERROR_H
