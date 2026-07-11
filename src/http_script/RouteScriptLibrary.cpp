#include "RouteScriptLibrary.h"

#include "ScriptExchangeCtx.h"

#include "../script/JsValue.h"
#include "../script/Library.h"
#include "../script/ScriptResult.h"
#include "../script/std/StdLibrary.h"

#include <string>
#include <string_view>
#include <utility>

namespace fiber::http_script {

namespace {

using fiber::script::GcHeap;
using HostCallFrame = fiber::script::Library::HostCallFrame;
using fiber::script::JsValue;
using fiber::script::Library;
using fiber::script::ScriptAbortReason;
using fiber::script::ScriptResult;

ScriptExchangeCtx *ctx_of(const HostCallFrame &frame) noexcept {
    return static_cast<ScriptExchangeCtx *>(frame.attach);
}

// Lowercases ASCII and folds '-' to '_' so that $header.x_forwarded_for matches the
// "X-Forwarded-For" header. Applied to the script key for header/cookie namespaces.
std::string normalize_lookup_key(std::string_view key) {
    std::string out;
    out.reserve(key.size());
    for (unsigned char ch: key) {
        if (ch == '-') {
            out.push_back('_');
        } else if (ch >= 'A' && ch <= 'Z') {
            out.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else {
            out.push_back(static_cast<char>(ch));
        }
    }
    return out;
}

std::string make_cache_key(std::string_view ns, std::string_view key) {
    std::string out;
    out.reserve(ns.size() + 1 + key.size());
    out.append(ns);
    out.push_back('/');
    out.append(key);
    return out;
}

} // namespace

ScriptResult RouteScriptLibrary::path_var_fn(void *userdata, const HostCallFrame &frame) noexcept {
    auto *ref = static_cast<const VarRef *>(userdata);
    auto *ctx = ctx_of(frame);
    if (ref == nullptr || ctx == nullptr || frame.runtime == nullptr) {
        return ScriptResult::abort(ScriptAbortReason::InvalidState);
    }
    return ctx->path_var(*frame.runtime, ref->name);
}

ScriptResult RouteScriptLibrary::query_var_fn(void *userdata, const HostCallFrame &frame) noexcept {
    auto *ref = static_cast<const VarRef *>(userdata);
    auto *ctx = ctx_of(frame);
    if (ref == nullptr || ctx == nullptr || frame.runtime == nullptr) {
        return ScriptResult::abort(ScriptAbortReason::InvalidState);
    }
    return ctx->query_var(*frame.runtime, ref->name);
}

ScriptResult RouteScriptLibrary::header_var_fn(void *userdata, const HostCallFrame &frame) noexcept {
    auto *ref = static_cast<const VarRef *>(userdata);
    auto *ctx = ctx_of(frame);
    if (ref == nullptr || ctx == nullptr || frame.runtime == nullptr) {
        return ScriptResult::abort(ScriptAbortReason::InvalidState);
    }
    return ctx->header_var(*frame.runtime, ref->name);
}

ScriptResult RouteScriptLibrary::cookie_var_fn(void *userdata, const HostCallFrame &frame) noexcept {
    auto *ref = static_cast<const VarRef *>(userdata);
    auto *ctx = ctx_of(frame);
    if (ref == nullptr || ctx == nullptr || frame.runtime == nullptr) {
        return ScriptResult::abort(ScriptAbortReason::InvalidState);
    }
    return ctx->cookie_var(*frame.runtime, ref->name);
}

ScriptResult RouteScriptLibrary::req_field_fn(void *userdata, const HostCallFrame &frame) noexcept {
    auto *ref = static_cast<const VarRef *>(userdata);
    auto *ctx = ctx_of(frame);
    if (ref == nullptr || ctx == nullptr || frame.runtime == nullptr) {
        return ScriptResult::abort(ScriptAbortReason::InvalidState);
    }
    return ctx->req_field(*frame.runtime, ref->name);
}

RouteScriptLibrary::RouteScriptLibrary(fiber::script::std_lib::StdLibrary &shared,
                                       const std::vector<std::string> &path_var_names) :
    shared_(shared), path_var_names_(path_var_names.begin(), path_var_names.end()) {}

void RouteScriptLibrary::mark_root_prop(std::string_view prop_name) { shared_.mark_root_prop(prop_name); }

Library::FunctionMatchResult RouteScriptLibrary::resolve_func(std::string_view name,
                                                              const FunctionMatchRequest &request) const {
    return shared_.resolve_func(name, request);
}

Library::FunctionMatchResult RouteScriptLibrary::resolve_async_func(std::string_view name,
                                                                    const FunctionMatchRequest &request) const {
    return shared_.resolve_async_func(name, request);
}

Library::DirectiveDef *
RouteScriptLibrary::resolve_directive_def(std::string_view type, std::string_view name,
                                          const std::vector<fiber::script::JsValue> &literals) const {
    return shared_.resolve_directive_def(type, name, literals);
}

const Library::HostCallable *RouteScriptLibrary::resolve_constant(std::string_view namespace_name,
                                                                  std::string_view key) const {
    if (namespace_name == "$path") {
        // Compile-time existence check: the name must be a path variable captured by this
        // location's route pattern.
        if (path_var_names_.find(std::string(key)) == path_var_names_.end()) {
            return nullptr;
        }
        return get_or_create(VarKind::Path, namespace_name, key);
    }
    if (namespace_name == "$query") {
        return get_or_create(VarKind::Query, namespace_name, key);
    }
    if (namespace_name == "$header") {
        return get_or_create(VarKind::Header, namespace_name, key);
    }
    if (namespace_name == "$cookie") {
        return get_or_create(VarKind::Cookie, namespace_name, key);
    }
    if (namespace_name == "$req") {
        // Compile-time existence check: fixed field set.
        if (key != "uri" && key != "method" && key != "path" && key != "query") {
            return nullptr;
        }
        return get_or_create(VarKind::ReqField, namespace_name, key);
    }
    return shared_.resolve_constant(namespace_name, key);
}

const Library::HostCallable *RouteScriptLibrary::resolve_async_constant(std::string_view /*namespace_name*/,
                                                                        std::string_view /*key*/) const {
    return nullptr; // no async route constants
}

const Library::HostCallable *RouteScriptLibrary::get_or_create(VarKind kind, std::string_view namespace_name,
                                                               std::string_view key) const {
    const std::string cache_key = make_cache_key(namespace_name, key);
    auto it = cache_.find(cache_key);
    if (it != cache_.end()) {
        return &it->second;
    }

    std::string name;
    if (kind == VarKind::Header || kind == VarKind::Cookie) {
        name = normalize_lookup_key(key);
    } else {
        name = std::string(key);
    }
    refs_.push_back(VarRef{kind, std::move(name)});

    HostCallable callable;
    callable.kind = HostCallable::Kind::SyncConstant;
    callable.userdata = &refs_.back();
    switch (kind) {
        case VarKind::Path:
            callable.constant = &RouteScriptLibrary::path_var_fn;
            callable.debug_name = "$path";
            break;
        case VarKind::Query:
            callable.constant = &RouteScriptLibrary::query_var_fn;
            callable.debug_name = "$query";
            break;
        case VarKind::Header:
            callable.constant = &RouteScriptLibrary::header_var_fn;
            callable.debug_name = "$header";
            break;
        case VarKind::Cookie:
            callable.constant = &RouteScriptLibrary::cookie_var_fn;
            callable.debug_name = "$cookie";
            break;
        case VarKind::ReqField:
            callable.constant = &RouteScriptLibrary::req_field_fn;
            callable.debug_name = "$req";
            break;
    }

    auto inserted = cache_.emplace(std::move(cache_key), callable);
    return &inserted.first->second;
}

} // namespace fiber::http_script
