#include "StdLibrary.h"

#include "../../common/Assert.h"

#include <cstdint>
#include <utility>

namespace fiber::script::std_lib {

void register_std_library(StdLibrary &library);

namespace {
bool signature_valid(const Library::FunctionSignature &signature, std::size_t default_count) {
    if (signature.required_argc > signature.fixed_argc) {
        return false;
    }
    if (default_count != signature.default_count) {
        return false;
    }
    if (signature.default_count != signature.fixed_argc - signature.required_argc) {
        return false;
    }
    if (signature.variadic && signature.default_count != 0) {
        return false;
    }
    return true;
}

bool signature_ranges_overlap(const Library::FunctionSignature &lhs, const Library::FunctionSignature &rhs) {
    const std::uint32_t lhs_min = lhs.variadic ? lhs.fixed_argc : lhs.required_argc;
    const std::uint32_t lhs_max = lhs.variadic ? UINT32_MAX : lhs.fixed_argc;
    const std::uint32_t rhs_min = rhs.variadic ? rhs.fixed_argc : rhs.required_argc;
    const std::uint32_t rhs_max = rhs.variadic ? UINT32_MAX : rhs.fixed_argc;
    return lhs_min <= rhs_max && rhs_min <= lhs_max;
}

bool matches_signature(const Library::FunctionSignature &signature, const Library::FunctionMatchRequest &request) {
    if (request.spread_argc_unknown && !signature.variadic) {
        return false;
    }
    if (signature.variadic) {
        return request.known_argc >= signature.fixed_argc;
    }
    return request.known_argc >= signature.required_argc && request.known_argc <= signature.fixed_argc;
}

template<typename Entry>
Library::FunctionMatchResult match_entries(const std::vector<Entry> &entries,
                                           const Library::FunctionMatchRequest &request, const Library &library) {
    const Entry *matched = nullptr;
    for (const Entry &entry: entries) {
        if (!matches_signature(entry.signature, request)) {
            continue;
        }
        if (matched) {
            return Library::FunctionMatchResult::ambiguous();
        }
        matched = &entry;
    }
    if (!matched) {
        return Library::FunctionMatchResult::arity_mismatch();
    }
    Library::FunctionSignature signature = matched->signature;
    signature.defaults = matched->defaults.empty() ? nullptr : matched->defaults.data();
    const fiber::json::JsValue *defaults = nullptr;
    std::uint16_t default_count = 0;
    if (!signature.variadic && request.known_argc < signature.fixed_argc) {
        const std::uint16_t default_offset = static_cast<std::uint16_t>(request.known_argc - signature.required_argc);
        default_count = static_cast<std::uint16_t>(signature.fixed_argc - request.known_argc);
        defaults = signature.defaults + default_offset;
    }
    return Library::FunctionMatchResult::found(library.host_callable_for(matched->func), signature, defaults,
                                               default_count);
}

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

StdLibrary::StdLibrary() { register_std_library(*this); }

Library::FunctionMatchResult StdLibrary::find_func(std::string_view name, const FunctionMatchRequest &request) {
    auto it = functions_.find(std::string(name));
    if (it == functions_.end()) {
        return FunctionMatchResult::not_found();
    }
    return match_entries(it->second, request, *this);
}

Library::FunctionMatchResult StdLibrary::find_async_func(std::string_view name, const FunctionMatchRequest &request) {
    auto it = async_functions_.find(std::string(name));
    if (it == async_functions_.end()) {
        return FunctionMatchResult::not_found();
    }
    return match_entries(it->second, request, *this);
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

Library::DirectiveDef *StdLibrary::find_directive_def(std::string_view type, std::string_view name,
                                                      const std::vector<fiber::json::JsValue> &literals) {
    (void) type;
    (void) name;
    (void) literals;
    return nullptr;
}

void StdLibrary::register_func(std::string name, FunctionSignature signature, Function *func) {
    register_func(std::move(name), signature, {}, func);
}

void StdLibrary::register_func(std::string name, FunctionSignature signature,
                               std::vector<fiber::json::JsValue> defaults, Function *func) {
    signature.default_count = static_cast<std::uint16_t>(defaults.size());
    signature.defaults = nullptr;
    FIBER_ASSERT(func != nullptr);
    FIBER_ASSERT(signature_valid(signature, defaults.size()));
    auto &entries = functions_[std::move(name)];
    for (const FunctionEntry &entry: entries) {
        FIBER_ASSERT(!signature_ranges_overlap(entry.signature, signature));
    }
    FunctionEntry entry;
    entry.signature = signature;
    entry.defaults = std::move(defaults);
    entry.func = func;
    entries.push_back(std::move(entry));
}

void StdLibrary::register_async_func(std::string name, FunctionSignature signature, AsyncFunction *func) {
    register_async_func(std::move(name), signature, {}, func);
}

void StdLibrary::register_async_func(std::string name, FunctionSignature signature,
                                     std::vector<fiber::json::JsValue> defaults, AsyncFunction *func) {
    signature.default_count = static_cast<std::uint16_t>(defaults.size());
    signature.defaults = nullptr;
    FIBER_ASSERT(func != nullptr);
    FIBER_ASSERT(signature_valid(signature, defaults.size()));
    auto &entries = async_functions_[std::move(name)];
    for (const AsyncFunctionEntry &entry: entries) {
        FIBER_ASSERT(!signature_ranges_overlap(entry.signature, signature));
    }
    AsyncFunctionEntry entry;
    entry.signature = signature;
    entry.defaults = std::move(defaults);
    entry.func = func;
    entries.push_back(std::move(entry));
}

void StdLibrary::register_constant(std::string name, Constant *constant) {
    constants_.emplace(std::move(name), constant);
}

void StdLibrary::register_async_constant(std::string name, AsyncConstant *constant) {
    async_constants_.emplace(std::move(name), constant);
}

} // namespace fiber::script::std_lib
