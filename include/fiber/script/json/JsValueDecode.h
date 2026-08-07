//
// Created by dear on 2026/7/7.
//

#ifndef FIBER_JSVALUEDECODE_H
#define FIBER_JSVALUEDECODE_H

#include <cstddef>

#include "../../common/json/JsonDecode.h"
#include "../JsGc.h"

namespace fiber::script::json {

using fiber::json::DecodeStatus;
using fiber::json::ParseError;

[[nodiscard]] DecodeStatus decode_js_value(GcHeap &heap, const char *data, std::size_t len, ValueHandle out,
                                           ParseError *error = nullptr) noexcept;

[[nodiscard]] DecodeStatus decode_js_value(GcHeap &heap, const char *data, std::size_t len, JsValue &out,
                                           ParseError *error = nullptr) noexcept;

} // namespace fiber::script::json

#endif // FIBER_JSVALUEDECODE_H
