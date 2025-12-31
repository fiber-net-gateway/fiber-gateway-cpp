//
// Created by dear on 2025/12/31.
//

#ifndef FIBER_JSVALUEENCODE_H
#define FIBER_JSVALUEENCODE_H

#include "JsonEncode.h"
#include "JsNode.h"

namespace fiber::json {

Generator::Result encode_js_value(Generator &gen, const JsValue &value);

} // namespace fiber::json

#endif // FIBER_JSVALUEENCODE_H
