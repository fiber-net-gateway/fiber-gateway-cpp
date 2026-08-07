//
// Created by dear on 2025/12/31.
//

#ifndef FIBER_JSVALUEENCODE_H
#define FIBER_JSVALUEENCODE_H

#include "../../common/json/JsonEncode.h"
#include "../JsValue.h"

namespace fiber::script::json {

using fiber::json::Generator;

Generator::Result encode_js_value(Generator &gen, const JsValue &value);

} // namespace fiber::script::json

#endif // FIBER_JSVALUEENCODE_H
