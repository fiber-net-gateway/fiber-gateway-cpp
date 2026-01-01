#ifndef FIBER_SCRIPT_RUN_ACCESS_H
#define FIBER_SCRIPT_RUN_ACCESS_H

#include "../../common/json/JsNode.h"

namespace fiber::script::run {

class Access {
public:
    static fiber::json::JsValue expand_object(const fiber::json::JsValue &target, const fiber::json::JsValue &addition);
    static fiber::json::JsValue expand_array(const fiber::json::JsValue &target, const fiber::json::JsValue &addition);
    static fiber::json::JsValue push_array(const fiber::json::JsValue &target, const fiber::json::JsValue &addition);

    static fiber::json::JsValue index_get(const fiber::json::JsValue &parent, const fiber::json::JsValue &key);
    static fiber::json::JsValue index_set(const fiber::json::JsValue &parent, const fiber::json::JsValue &key, const fiber::json::JsValue &value);
    static fiber::json::JsValue index_set1(const fiber::json::JsValue &parent, const fiber::json::JsValue &key, const fiber::json::JsValue &value);

    static fiber::json::JsValue prop_get(const fiber::json::JsValue &parent, const fiber::json::JsValue &key);
    static fiber::json::JsValue prop_set(const fiber::json::JsValue &parent, const fiber::json::JsValue &value, const fiber::json::JsValue &key);
    static fiber::json::JsValue prop_set1(const fiber::json::JsValue &parent, const fiber::json::JsValue &value, const fiber::json::JsValue &key);
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_ACCESS_H
