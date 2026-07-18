#include "RouteScriptExtension.h"

#include "ScriptExchangeCtx.h"

#include "../script/JsValue.h"
#include "../script/Library.h"
#include "../script/ScriptResult.h"
#include "../script/std/NodeText.h"
#include "../script/std/StdLibrary.h"

#include <string>
#include <string_view>
#include <utility>

namespace fiber::http_script {

namespace {

using fiber::script::GcHeap;
using HostCallFrame = fiber::script::Library::HostCallFrame;
using fiber::script::AbiResult;
using fiber::script::JsValue;
using fiber::script::Library;
using fiber::script::ScriptAbortReason;

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

AbiResult RouteScriptExtension::path_var_fn(void *userdata, const HostCallFrame &frame) noexcept {
    auto *ref = static_cast<const VarRef *>(userdata);
    auto *ctx = ctx_of(frame);
    if (ref == nullptr || ctx == nullptr) {
        return AbiResult::abort(ScriptAbortReason::InvalidState);
    }
    return ctx->path_var(frame.runtime, ref->name);
}

AbiResult RouteScriptExtension::query_var_fn(void *userdata, const HostCallFrame &frame) noexcept {
    auto *ref = static_cast<const VarRef *>(userdata);
    auto *ctx = ctx_of(frame);
    if (ref == nullptr || ctx == nullptr) {
        return AbiResult::abort(ScriptAbortReason::InvalidState);
    }
    return ctx->query_var(frame.runtime, ref->name);
}

AbiResult RouteScriptExtension::header_var_fn(void *userdata, const HostCallFrame &frame) noexcept {
    auto *ref = static_cast<const VarRef *>(userdata);
    auto *ctx = ctx_of(frame);
    if (ref == nullptr || ctx == nullptr) {
        return AbiResult::abort(ScriptAbortReason::InvalidState);
    }
    return ctx->header_var(frame.runtime, ref->name);
}

AbiResult RouteScriptExtension::cookie_var_fn(void *userdata, const HostCallFrame &frame) noexcept {
    auto *ref = static_cast<const VarRef *>(userdata);
    auto *ctx = ctx_of(frame);
    if (ref == nullptr || ctx == nullptr) {
        return AbiResult::abort(ScriptAbortReason::InvalidState);
    }
    return ctx->cookie_var(frame.runtime, ref->name);
}

AbiResult RouteScriptExtension::req_field_fn(void *userdata, const HostCallFrame &frame) noexcept {
    auto *ref = static_cast<const VarRef *>(userdata);
    auto *ctx = ctx_of(frame);
    if (ref == nullptr || ctx == nullptr) {
        return AbiResult::abort(ScriptAbortReason::InvalidState);
    }
    return ctx->req_field(frame.runtime, ref->name);
}

AbiResult RouteScriptExtension::conn_field_fn(void *userdata, const HostCallFrame &frame) noexcept {
    auto *ref = static_cast<const VarRef *>(userdata);
    auto *ctx = ctx_of(frame);
    if (ref == nullptr || ctx == nullptr) {
        return AbiResult::abort(ScriptAbortReason::InvalidState);
    }
    return ctx->conn_field(frame.runtime, ref->name);
}

const fiber::script::std_lib::StdLibrary::ExtOps &RouteScriptExtension::ops() noexcept {
    static const fiber::script::std_lib::StdLibrary::ExtOps kOps{
            .resolve_constant = &RouteScriptExtension::resolve_constant_op,
            .resolve_directive_def = &RouteScriptExtension::resolve_directive_def_op,
    };
    return kOps;
}

void RouteScriptExtension::set_compile_path_vars(const std::vector<std::string> &path_var_names) {
    current_path_var_names_.clear();
    current_path_var_names_.reserve(path_var_names.size());
    current_path_var_names_.insert(path_var_names.begin(), path_var_names.end());
    has_current_route_info_ = true;
}

const RouteScriptExtension::HostCallable *
RouteScriptExtension::resolve_constant_op(void *ctx, std::string_view namespace_name, std::string_view key) {
    return static_cast<RouteScriptExtension *>(ctx)->resolve_constant(namespace_name, key);
}

RouteScriptExtension::DirectiveDef *
RouteScriptExtension::resolve_directive_def_op(void *ctx, std::string_view type, std::string_view name,
                                               const std::vector<fiber::script::JsValue> &literals) {
    return static_cast<RouteScriptExtension *>(ctx)->resolve_directive_def(type, name, literals);
}

RouteScriptExtension::DirectiveDef *
RouteScriptExtension::resolve_directive_def(std::string_view type, std::string_view name,
                                            const std::vector<fiber::script::JsValue> &literals) {
    if (!allow_http_directives_) {
        return nullptr;
    }
    if (type == "http" && literals.size() == 1) {
        // `directive <name> = http "<target>";` binds <name> to an upstream name or ad-hoc URL.
        std::string_view target_sv;
        if (!fiber::script::std_lib::string_utf8_view(literals[0], target_sv) || target_sv.empty()) {
            return nullptr;
        }
        auto target = HttpTargetSpec::parse(target_sv);
        if (!target) {
            return nullptr;
        }
        directive_defs_.push_back(std::make_unique<HttpDirectiveDef>(std::move(*target)));
        return directive_defs_.back().get();
    }
    return nullptr;
}

const RouteScriptExtension::HostCallable *RouteScriptExtension::resolve_constant(std::string_view namespace_name,
                                                                                 std::string_view key) {
    if (!has_current_route_info_) {
        return nullptr;
    }
    if (namespace_name == "$path") {
        // Compile-time existence check: the name must be a path variable captured by this
        // location's route pattern.
        if (current_path_var_names_.find(std::string(key)) == current_path_var_names_.end()) {
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
    if (namespace_name == "$conn") {
        if (key != "remote_addr" && key != "remote_port" && key != "http_version" && key != "scheme" && key != "tls") {
            return nullptr;
        }
        return get_or_create(VarKind::ConnField, namespace_name, key);
    }
    return nullptr;
}

const RouteScriptExtension::HostCallable *
RouteScriptExtension::get_or_create(VarKind kind, std::string_view namespace_name, std::string_view key) {
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
            callable.constant = &RouteScriptExtension::path_var_fn;
            callable.debug_name = "$path";
            break;
        case VarKind::Query:
            callable.constant = &RouteScriptExtension::query_var_fn;
            callable.debug_name = "$query";
            break;
        case VarKind::Header:
            callable.constant = &RouteScriptExtension::header_var_fn;
            callable.debug_name = "$header";
            break;
        case VarKind::Cookie:
            callable.constant = &RouteScriptExtension::cookie_var_fn;
            callable.debug_name = "$cookie";
            break;
        case VarKind::ReqField:
            callable.constant = &RouteScriptExtension::req_field_fn;
            callable.debug_name = "$req";
            break;
        case VarKind::ConnField:
            callable.constant = &RouteScriptExtension::conn_field_fn;
            callable.debug_name = "$conn";
            break;
    }

    auto inserted = cache_.emplace(std::move(cache_key), callable);
    return &inserted.first->second;
}

} // namespace fiber::http_script
