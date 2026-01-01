#include "StdLibrary.h"

namespace fiber::script::std_lib {

namespace {
std::string make_constant_key(std::string_view ns, std::string_view key) {
    std::string name;
    name.reserve(ns.size() + 1 + key.size());
    name.append(ns.begin(), ns.end());
    name.push_back('/');
    name.append(key.begin(), key.end());
    return name;
}
} // namespace

StdLibrary &StdLibrary::instance() {
    static StdLibrary inst;
    return inst;
}

Library::Function *StdLibrary::find_func(std::string_view name) {
    auto it = functions_.find(std::string(name));
    if (it == functions_.end()) {
        return nullptr;
    }
    return it->second;
}

Library::AsyncFunction *StdLibrary::find_async_func(std::string_view name) {
    auto it = async_functions_.find(std::string(name));
    if (it == async_functions_.end()) {
        return nullptr;
    }
    return it->second;
}

Library::Constant *StdLibrary::find_constant(std::string_view namespace_name, std::string_view key) {
    auto it = constants_.find(make_constant_key(namespace_name, key));
    if (it == constants_.end()) {
        return nullptr;
    }
    return it->second;
}

Library::AsyncConstant *StdLibrary::find_async_constant(std::string_view namespace_name, std::string_view key) {
    auto it = async_constants_.find(make_constant_key(namespace_name, key));
    if (it == async_constants_.end()) {
        return nullptr;
    }
    return it->second;
}

Library::DirectiveDef *StdLibrary::find_directive_def(std::string_view type,
                                                      std::string_view name,
                                                      const std::vector<fiber::json::JsValue> &literals) {
    (void)type;
    (void)name;
    (void)literals;
    return nullptr;
}

void StdLibrary::register_func(std::string name, Function *func) {
    functions_.emplace(std::move(name), func);
}

void StdLibrary::register_async_func(std::string name, AsyncFunction *func) {
    async_functions_.emplace(std::move(name), func);
}

void StdLibrary::register_constant(std::string name, Constant *constant) {
    constants_.emplace(std::move(name), constant);
}

void StdLibrary::register_async_constant(std::string name, AsyncConstant *constant) {
    async_constants_.emplace(std::move(name), constant);
}

} // namespace fiber::script::std_lib
