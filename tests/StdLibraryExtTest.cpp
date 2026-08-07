#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include <fiber/script/Library.h>
#include <fiber/script/std/StdLibrary.h>

namespace {

using fiber::script::AbiResult;
using fiber::script::AsyncTask;
using fiber::script::JsValue;
using fiber::script::Library;
using fiber::script::std_lib::StdLibrary;

AbiResult sync_func(void *, const Library::HostCallFrame &, Library::Arguments) noexcept {
    return AbiResult::success(JsValue::make_undefined());
}

AsyncTask async_func(void *, const Library::HostCallFrame &, Library::Arguments) noexcept {
    co_return AbiResult::success(JsValue::make_undefined());
}

AbiResult sync_constant(void *, const Library::HostCallFrame &) noexcept {
    return AbiResult::success(JsValue::make_undefined());
}

AsyncTask async_constant(void *, const Library::HostCallFrame &) noexcept {
    co_return AbiResult::success(JsValue::make_undefined());
}

class TestDirectiveDef final : public Library::DirectiveDef {
public:
    Library::FunctionMatchResult resolve_func(std::string_view, std::string_view, const Library::FunctionMatchRequest &,
                                              const Library &) const override {
        return Library::FunctionMatchResult::not_found();
    }

    Library::FunctionMatchResult resolve_async_func(std::string_view, std::string_view,
                                                    const Library::FunctionMatchRequest &,
                                                    const Library &) const override {
        return Library::FunctionMatchResult::not_found();
    }
};

struct ExtContext {
    ExtContext() {
        function.kind = Library::HostCallable::Kind::SyncFunction;
        function.function = &sync_func;
        function.debug_name = "extension.sync";
        async_function.kind = Library::HostCallable::Kind::AsyncFunction;
        async_function.async_function = &async_func;
        async_function.debug_name = "extension.async";
        constant.kind = Library::HostCallable::Kind::SyncConstant;
        constant.constant = &sync_constant;
        constant.debug_name = "extension.constant";
        async_constant_value.kind = Library::HostCallable::Kind::AsyncConstant;
        async_constant_value.async_constant = &async_constant;
        async_constant_value.debug_name = "extension.async_constant";
    }

    std::string_view function_name;
    std::string_view async_function_name;
    std::string_view constant_namespace;
    std::string_view constant_key;
    std::string_view async_constant_namespace;
    std::string_view async_constant_key;
    std::string_view directive_type;
    Library::FunctionMatchStatus function_status = Library::FunctionMatchStatus::Found;
    int function_calls = 0;
    int async_function_calls = 0;
    int constant_calls = 0;
    int async_constant_calls = 0;
    int directive_calls = 0;
    std::vector<std::string> root_props;
    Library::HostCallable function{};
    Library::HostCallable async_function{};
    Library::HostCallable constant{};
    Library::HostCallable async_constant_value{};
    TestDirectiveDef directive;
};

Library::FunctionMatchResult resolve_func(void *ctx, std::string_view name, const Library::FunctionMatchRequest &) {
    auto &ext = *static_cast<ExtContext *>(ctx);
    ++ext.function_calls;
    if (name != ext.function_name) {
        return Library::FunctionMatchResult::not_found();
    }
    if (ext.function_status == Library::FunctionMatchStatus::ArityMismatch) {
        return Library::FunctionMatchResult::arity_mismatch();
    }
    if (ext.function_status == Library::FunctionMatchStatus::Ambiguous) {
        return Library::FunctionMatchResult::ambiguous();
    }
    Library::FunctionSignature signature{.required_argc = 0, .fixed_argc = 0, .variadic = false};
    return Library::FunctionMatchResult::found(&ext.function, signature, nullptr, 0);
}

Library::FunctionMatchResult resolve_async_func(void *ctx, std::string_view name,
                                                const Library::FunctionMatchRequest &) {
    auto &ext = *static_cast<ExtContext *>(ctx);
    ++ext.async_function_calls;
    if (name != ext.async_function_name) {
        return Library::FunctionMatchResult::not_found();
    }
    Library::FunctionSignature signature{.required_argc = 0, .fixed_argc = 0, .variadic = false};
    return Library::FunctionMatchResult::found(&ext.async_function, signature, nullptr, 0);
}

const Library::HostCallable *resolve_constant(void *ctx, std::string_view namespace_name, std::string_view key) {
    auto &ext = *static_cast<ExtContext *>(ctx);
    ++ext.constant_calls;
    return namespace_name == ext.constant_namespace && key == ext.constant_key ? &ext.constant : nullptr;
}

const Library::HostCallable *resolve_async_constant(void *ctx, std::string_view namespace_name, std::string_view key) {
    auto &ext = *static_cast<ExtContext *>(ctx);
    ++ext.async_constant_calls;
    return namespace_name == ext.async_constant_namespace && key == ext.async_constant_key ? &ext.async_constant_value
                                                                                           : nullptr;
}

Library::DirectiveDef *resolve_directive_def(void *ctx, std::string_view type, std::string_view,
                                             const std::vector<JsValue> &) {
    auto &ext = *static_cast<ExtContext *>(ctx);
    ++ext.directive_calls;
    return type == ext.directive_type ? &ext.directive : nullptr;
}

void mark_root_prop(void *ctx, std::string_view prop_name) {
    static_cast<ExtContext *>(ctx)->root_props.emplace_back(prop_name);
}

const StdLibrary::ExtOps kExtOps{
        .mark_root_prop = &mark_root_prop,
        .resolve_func = &resolve_func,
        .resolve_async_func = &resolve_async_func,
        .resolve_constant = &resolve_constant,
        .resolve_async_constant = &resolve_async_constant,
        .resolve_directive_def = &resolve_directive_def,
};

TEST(StdLibraryExtTest, StandardEntriesTakePrecedence) {
    StdLibrary library;
    ExtContext extension;
    extension.function_name = "test.standard";
    extension.constant_namespace = "$test";
    extension.constant_key = "standard";
    library.add_ext_ops(&extension, kExtOps);

    Library::FunctionSignature signature{.required_argc = 0, .fixed_argc = 0, .variadic = false};
    library.register_func("test.standard", signature, &sync_func, nullptr, "standard.sync");
    library.register_constant("$test/standard", &sync_constant, nullptr, "standard.constant");

    auto match = library.resolve_func("test.standard", {.known_argc = 0});
    ASSERT_EQ(match.status, Library::FunctionMatchStatus::Found);
    ASSERT_NE(match.callable, nullptr);
    EXPECT_STREQ(match.callable->debug_name, "standard.sync");
    EXPECT_EQ(extension.function_calls, 0);

    match = library.resolve_func("test.standard", {.known_argc = 1});
    EXPECT_EQ(match.status, Library::FunctionMatchStatus::ArityMismatch);
    EXPECT_EQ(extension.function_calls, 0);

    const Library::HostCallable *constant = library.resolve_constant("$test", "standard");
    ASSERT_NE(constant, nullptr);
    EXPECT_STREQ(constant->debug_name, "standard.constant");
    EXPECT_EQ(extension.constant_calls, 0);
}

TEST(StdLibraryExtTest, ExtensionsResolveInRegistrationOrder) {
    StdLibrary library;
    ExtContext first;
    ExtContext second;
    first.function_name = "test.owned";
    first.function_status = Library::FunctionMatchStatus::ArityMismatch;
    second.function_name = "test.owned";
    second.constant_namespace = "$test";
    second.constant_key = "value";
    library.add_ext_ops(&first, kExtOps);
    library.add_ext_ops(&second, kExtOps);

    auto match = library.resolve_func("test.owned", {.known_argc = 0});
    EXPECT_EQ(match.status, Library::FunctionMatchStatus::ArityMismatch);
    EXPECT_EQ(first.function_calls, 1);
    EXPECT_EQ(second.function_calls, 0);

    const Library::HostCallable *constant = library.resolve_constant("$test", "value");
    EXPECT_EQ(constant, &second.constant);
    EXPECT_EQ(first.constant_calls, 1);
    EXPECT_EQ(second.constant_calls, 1);
}

TEST(StdLibraryExtTest, AsyncDirectiveAndRootCallbacksAreForwarded) {
    StdLibrary library;
    ExtContext extension;
    extension.async_function_name = "test.async";
    extension.async_constant_namespace = "$test";
    extension.async_constant_key = "async";
    extension.directive_type = "test";
    library.add_ext_ops(&extension, kExtOps);

    auto match = library.resolve_async_func("test.async", {.known_argc = 0});
    EXPECT_EQ(match.status, Library::FunctionMatchStatus::Found);
    EXPECT_EQ(match.callable, &extension.async_function);
    EXPECT_EQ(library.resolve_async_constant("$test", "async"), &extension.async_constant_value);
    EXPECT_EQ(library.resolve_directive_def("test", "svc", {}), &extension.directive);

    library.mark_root_prop("request");
    ASSERT_EQ(extension.root_props.size(), 1);
    EXPECT_EQ(extension.root_props.front(), "request");
}

} // namespace
