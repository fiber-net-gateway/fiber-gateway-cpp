#include "RouteScriptExtension.h"

#include "../script/JsValue.h"
#include "../script/std/NodeText.h"
#include "../script/std/StdLibrary.h"

#include <cassert>
#include <string>
#include <string_view>
#include <utility>

namespace fiber::http_script {

RouteScriptExtension::CompileScope::CompileScope(RouteScriptExtension &extension, ConstPackage::Builder &builder,
                                                 std::span<const std::string> path_var_names,
                                                 bool http_directives_enabled) : extension_(&extension) {
    extension_->begin_compile(builder, path_var_names, http_directives_enabled);
}

RouteScriptExtension::CompileScope::~CompileScope() {
    if (extension_ != nullptr) {
        extension_->end_compile();
    }
}

const fiber::script::std_lib::StdLibrary::ExtOps &RouteScriptExtension::ops() noexcept {
    static const fiber::script::std_lib::StdLibrary::ExtOps kOps{
            .resolve_constant = &RouteScriptExtension::resolve_constant_op,
            .resolve_directive_def = &RouteScriptExtension::resolve_directive_def_op,
    };
    return kOps;
}

void RouteScriptExtension::begin_compile(ConstPackage::Builder &builder, std::span<const std::string> path_var_names,
                                         bool http_directives_enabled) {
    assert(const_builder_ == nullptr);
    current_path_var_names_.clear();
    current_path_var_names_.reserve(path_var_names.size());
    current_path_var_names_.insert(path_var_names.begin(), path_var_names.end());
    const_builder_ = &builder;
    allow_http_directives_ = http_directives_enabled;
}

void RouteScriptExtension::end_compile() noexcept {
    current_path_var_names_.clear();
    const_builder_ = nullptr;
    allow_http_directives_ = false;
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
    if (const_builder_ == nullptr) {
        return nullptr;
    }
    if (namespace_name == "$path") {
        // Compile-time existence check: the name must be a path variable captured by this
        // location's route pattern.
        if (current_path_var_names_.find(std::string(key)) == current_path_var_names_.end()) {
            return nullptr;
        }
        return const_builder_->add_constant(ConstType::Path, key);
    }
    if (namespace_name == "$query") {
        return const_builder_->add_constant(ConstType::Query, key);
    }
    if (namespace_name == "$header") {
        return const_builder_->add_constant(ConstType::Header, key);
    }
    if (namespace_name == "$cookie") {
        return const_builder_->add_constant(ConstType::Cookie, key);
    }
    if (namespace_name == "$context") {
        return const_builder_->add_constant(ConstType::Context, key);
    }
    if (namespace_name == "$req") {
        // Compile-time existence check: fixed field set.
        if (key != "uri" && key != "method" && key != "path" && key != "query") {
            return nullptr;
        }
        return const_builder_->add_constant(ConstType::Request, key);
    }
    if (namespace_name == "$conn") {
        if (key != "remote_addr" && key != "remote_port" && key != "http_version" && key != "scheme" && key != "tls") {
            return nullptr;
        }
        return const_builder_->add_constant(ConstType::Connection, key);
    }
    return nullptr;
}

} // namespace fiber::http_script
