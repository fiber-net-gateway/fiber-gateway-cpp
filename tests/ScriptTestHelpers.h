#ifndef FIBER_TEST_SCRIPT_TEST_HELPERS_H
#define FIBER_TEST_SCRIPT_TEST_HELPERS_H

#include <gtest/gtest.h>

#include <string_view>
#include <utility>

#include "script/ScriptCompiler.h"
#include "script/std/StdLibrary.h"

namespace fiber::test {

// ScriptResult may contain borrowed string constants owned by Script::Compiled.
// Keep the Script alive for as long as tests inspect the result.
class RetainedScriptResult {
public:
    RetainedScriptResult(script::Script script, script::ScriptResult result) noexcept :
        script_(std::move(script)), result_(result) {}

    [[nodiscard]] bool is_value() const noexcept { return result_.is_value(); }
    [[nodiscard]] bool is_void() const noexcept { return result_.is_void(); }
    [[nodiscard]] bool is_exception() const noexcept { return result_.is_exception(); }
    [[nodiscard]] bool is_abort() const noexcept { return result_.is_abort(); }
    [[nodiscard]] bool is_success() const noexcept { return result_.is_success(); }
    [[nodiscard]] bool is_pending() const noexcept { return result_.is_pending(); }
    [[nodiscard]] bool is_done() const noexcept { return result_.is_done(); }
    [[nodiscard]] bool has_value() const noexcept { return result_.has_value(); }

    [[nodiscard]] const script::JsValue &value() const noexcept { return result_.value(); }
    [[nodiscard]] const script::JsValue &exception() const noexcept { return result_.exception(); }
    [[nodiscard]] const script::ScriptAbort &abort() const noexcept { return result_.abort(); }
    [[nodiscard]] const script::JsValue &error() const noexcept { return result_.error(); }

private:
    script::Script script_{};
    script::ScriptResult result_{};
};

inline RetainedScriptResult run_script(std::string_view source, script::GcHeap &heap) {
    auto compiled = script::compile_script(script::std_lib::StdLibrary::instance(), source);
    EXPECT_TRUE(compiled.has_value()) << (compiled ? "" : compiled.error().message);
    if (!compiled) {
        return {script::Script{}, script::ScriptResult::abort(script::ScriptAbortReason::Internal)};
    }

    script::Script retained = std::move(*compiled);
    script::JsValue root = script::JsValue::make_undefined();
    script::ScriptResult result = retained.exec_sync(root, nullptr, heap);
    return {std::move(retained), result};
}

inline RetainedScriptResult run_template(std::string_view body, script::GcHeap &heap) {
    auto compiled = script::compile_template_string(script::std_lib::StdLibrary::instance(), body);
    EXPECT_TRUE(compiled.has_value()) << (compiled ? "" : compiled.error().message);
    if (!compiled) {
        return {script::Script{}, script::ScriptResult::abort(script::ScriptAbortReason::Internal)};
    }

    script::Script retained = std::move(*compiled);
    script::JsValue root = script::JsValue::make_undefined();
    script::ScriptResult result = retained.exec_sync(root, nullptr, heap);
    return {std::move(retained), result};
}

} // namespace fiber::test

#endif // FIBER_TEST_SCRIPT_TEST_HELPERS_H
