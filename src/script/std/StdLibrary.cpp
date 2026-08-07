#include <fiber/script/std/StdLibrary.h>

#include <fiber/script/std/ArrayFuncs.h>
#include <fiber/script/std/BinaryFuncs.h>
#include <fiber/script/std/HashFuncs.h>
#include <fiber/script/std/IncludesFunc.h>
#include <fiber/script/std/JsonFuncs.h>
#include <fiber/script/std/LengthFunc.h>
#include <fiber/script/std/MathFuncs.h>
#include <fiber/script/std/ObjectsFuncs.h>
#include <fiber/script/std/RandFuncs.h>
#include <fiber/script/std/StringsFuncs.h>
#include <fiber/script/std/UrlFuncs.h>

#include <fiber/common/Assert.h>

#include <cstdint>
#include <utility>

namespace fiber::script::std_lib {

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
Library::FunctionMatchResult match_entries(const std::deque<Entry> &entries,
                                           const Library::FunctionMatchRequest &request) {
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
    const fiber::script::JsValue *defaults = nullptr;
    std::uint16_t default_count = 0;
    if (!signature.variadic && request.known_argc < signature.fixed_argc) {
        const std::uint16_t default_offset = static_cast<std::uint16_t>(request.known_argc - signature.required_argc);
        default_count = static_cast<std::uint16_t>(signature.fixed_argc - request.known_argc);
        defaults = signature.defaults + default_offset;
    }
    return Library::FunctionMatchResult::found(&matched->callable, signature, defaults, default_count);
}

std::string make_constant_key(std::string_view ns, std::string_view key) {
    std::string name;
    name.reserve(ns.size() + 1 + key.size());
    name.append(ns.begin(), ns.end());
    name.push_back('/');
    name.append(key.begin(), key.end());
    return name;
}

Library::HostCallable make_function_callable(Library::Function function, void *userdata,
                                             const char *debug_name) noexcept {
    Library::HostCallable callable;
    callable.kind = Library::HostCallable::Kind::SyncFunction;
    callable.userdata = userdata;
    callable.function = function;
    callable.debug_name = debug_name;
    return callable;
}

Library::HostCallable make_async_function_callable(Library::AsyncFunction function, void *userdata,
                                                   const char *debug_name) noexcept {
    Library::HostCallable callable;
    callable.kind = Library::HostCallable::Kind::AsyncFunction;
    callable.userdata = userdata;
    callable.async_function = function;
    callable.debug_name = debug_name;
    return callable;
}

Library::HostCallable make_constant_callable(Library::Constant constant, void *userdata,
                                             const char *debug_name) noexcept {
    Library::HostCallable callable;
    callable.kind = Library::HostCallable::Kind::SyncConstant;
    callable.userdata = userdata;
    callable.constant = constant;
    callable.debug_name = debug_name;
    return callable;
}

Library::HostCallable make_async_constant_callable(Library::AsyncConstant constant, void *userdata,
                                                   const char *debug_name) noexcept {
    Library::HostCallable callable;
    callable.kind = Library::HostCallable::Kind::AsyncConstant;
    callable.userdata = userdata;
    callable.async_constant = constant;
    callable.debug_name = debug_name;
    return callable;
}
} // namespace

StdLibrary &StdLibrary::instance() {
    static StdLibrary inst;
    return inst;
}

void StdLibrary::add_ext_ops(void *ctx, ExtOps ops) {
    FIBER_ASSERT(ctx != nullptr);
    FIBER_ASSERT(ops.mark_root_prop != nullptr || ops.resolve_func != nullptr || ops.resolve_async_func != nullptr ||
                 ops.resolve_constant != nullptr || ops.resolve_async_constant != nullptr ||
                 ops.resolve_directive_def != nullptr);
    extensions_.push_back(Extension{.ctx = ctx, .ops = ops});
}

void StdLibrary::mark_root_prop(std::string_view prop_name) {
    for (const Extension &extension: extensions_) {
        if (extension.ops.mark_root_prop != nullptr) {
            extension.ops.mark_root_prop(extension.ctx, prop_name);
        }
    }
}

StdLibrary::StdLibrary() {
    register_array_funcs(*this);
    register_binary_funcs(*this);
    register_length_func(*this);
    register_includes_func(*this);
    register_math_funcs(*this);
    register_rand_funcs(*this);
    register_hash_funcs(*this);
    register_json_funcs(*this);
    register_objects_funcs(*this);
    register_strings_funcs(*this);
    register_url_funcs(*this);
}

Library::FunctionMatchResult StdLibrary::resolve_func(std::string_view name,
                                                      const FunctionMatchRequest &request) const {
    auto it = functions_.find(std::string(name));
    if (it != functions_.end()) {
        return match_entries(it->second, request);
    }
    FunctionMatchResult result = FunctionMatchResult::not_found();
    for (const Extension &extension: extensions_) {
        if (extension.ops.resolve_func == nullptr) {
            continue;
        }
        result = extension.ops.resolve_func(extension.ctx, name, request);
        if (result.status != FunctionMatchStatus::NotFound) {
            return result;
        }
    }
    return result;
}

Library::FunctionMatchResult StdLibrary::resolve_async_func(std::string_view name,
                                                            const FunctionMatchRequest &request) const {
    auto it = async_functions_.find(std::string(name));
    if (it != async_functions_.end()) {
        return match_entries(it->second, request);
    }
    FunctionMatchResult result = FunctionMatchResult::not_found();
    for (const Extension &extension: extensions_) {
        if (extension.ops.resolve_async_func == nullptr) {
            continue;
        }
        result = extension.ops.resolve_async_func(extension.ctx, name, request);
        if (result.status != FunctionMatchStatus::NotFound) {
            return result;
        }
    }
    return result;
}

const Library::HostCallable *StdLibrary::resolve_constant(std::string_view namespace_name, std::string_view key) const {
    auto it = constants_.find(make_constant_key(namespace_name, key));
    if (it != constants_.end()) {
        return &it->second;
    }
    for (const Extension &extension: extensions_) {
        if (extension.ops.resolve_constant == nullptr) {
            continue;
        }
        if (const HostCallable *callable = extension.ops.resolve_constant(extension.ctx, namespace_name, key)) {
            return callable;
        }
    }
    return nullptr;
}

const Library::HostCallable *StdLibrary::resolve_async_constant(std::string_view namespace_name,
                                                                std::string_view key) const {
    auto it = async_constants_.find(make_constant_key(namespace_name, key));
    if (it != async_constants_.end()) {
        return &it->second;
    }
    for (const Extension &extension: extensions_) {
        if (extension.ops.resolve_async_constant == nullptr) {
            continue;
        }
        if (const HostCallable *callable = extension.ops.resolve_async_constant(extension.ctx, namespace_name, key)) {
            return callable;
        }
    }
    return nullptr;
}

Library::DirectiveDef *StdLibrary::resolve_directive_def(std::string_view type, std::string_view name,
                                                         const std::vector<fiber::script::JsValue> &literals) const {
    for (const Extension &extension: extensions_) {
        if (extension.ops.resolve_directive_def == nullptr) {
            continue;
        }
        if (DirectiveDef *def = extension.ops.resolve_directive_def(extension.ctx, type, name, literals)) {
            return def;
        }
    }
    return nullptr;
}

void StdLibrary::register_func(std::string_view name, FunctionSignature signature, Function function, void *userdata,
                               const char *debug_name) {
    register_func(name, signature, {}, function, userdata, debug_name);
}

void StdLibrary::register_func(std::string_view name, FunctionSignature signature,
                               std::vector<fiber::script::JsValue> defaults, Function function, void *userdata,
                               const char *debug_name) {
    signature.default_count = static_cast<std::uint16_t>(defaults.size());
    signature.defaults = nullptr;
    FIBER_ASSERT(function != nullptr);
    FIBER_ASSERT(signature_valid(signature, defaults.size()));
    auto &entries = functions_[std::string(name)];
    for (const FunctionEntry &entry: entries) {
        FIBER_ASSERT(!signature_ranges_overlap(entry.signature, signature));
    }
    FunctionEntry entry;
    entry.signature = signature;
    entry.defaults = std::move(defaults);
    entry.callable = make_function_callable(function, userdata, debug_name);
    entries.push_back(std::move(entry));
}

void StdLibrary::register_async_func(std::string_view name, FunctionSignature signature, AsyncFunction function,
                                     void *userdata, const char *debug_name) {
    register_async_func(name, signature, {}, function, userdata, debug_name);
}

void StdLibrary::register_async_func(std::string_view name, FunctionSignature signature,
                                     std::vector<fiber::script::JsValue> defaults, AsyncFunction function,
                                     void *userdata, const char *debug_name) {
    signature.default_count = static_cast<std::uint16_t>(defaults.size());
    signature.defaults = nullptr;
    FIBER_ASSERT(function != nullptr);
    FIBER_ASSERT(signature_valid(signature, defaults.size()));
    auto &entries = async_functions_[std::string(name)];
    for (const FunctionEntry &entry: entries) {
        FIBER_ASSERT(!signature_ranges_overlap(entry.signature, signature));
    }
    FunctionEntry entry;
    entry.signature = signature;
    entry.defaults = std::move(defaults);
    entry.callable = make_async_function_callable(function, userdata, debug_name);
    entries.push_back(std::move(entry));
}

void StdLibrary::register_constant(std::string_view name, Constant constant, void *userdata, const char *debug_name) {
    FIBER_ASSERT(constant != nullptr);
    const bool inserted =
            constants_.emplace(std::string(name), make_constant_callable(constant, userdata, debug_name)).second;
    FIBER_ASSERT(inserted);
}

void StdLibrary::register_async_constant(std::string_view name, AsyncConstant constant, void *userdata,
                                         const char *debug_name) {
    FIBER_ASSERT(constant != nullptr);
    const bool inserted =
            async_constants_.emplace(std::string(name), make_async_constant_callable(constant, userdata, debug_name))
                    .second;
    FIBER_ASSERT(inserted);
}

} // namespace fiber::script::std_lib
