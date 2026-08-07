#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <fiber/http_script/HttpScriptLib.h>
#include <fiber/script/ScriptCompiler.h>
#include <fiber/script/std/StdLibrary.h>

namespace {

// Resolves a path relative to this source file, so the test works regardless of the
// working directory ctest runs from (e.g. from build/ rather than the repo root).
std::string repo_path(const std::string &relative) {
    auto base = std::filesystem::path(__FILE__).parent_path(); // tests/
    base = base.parent_path(); // repo root
    return (base / relative).lexically_normal().string();
}

std::string read_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::shared_ptr<fiber::script::Script> compile(const std::string &src) {
    auto lib = std::make_shared<fiber::script::std_lib::StdLibrary>();
    fiber::http_script::register_http_functions_to_lib(*lib);
    auto compiled = fiber::script::compile_script(*lib, src);
    return compiled.has_value() ? std::make_shared<fiber::script::Script>(std::move(*compiled)) : nullptr;
}

} // namespace

// Guards the example scripts shipped under apps/lite_nginx/conf/scripts/ against
// regressions: each must compile against the HTTP script library so the bundled config
// keeps working.
TEST(ExampleScriptConfigs, HealthCompiles) {
    auto script = compile(read_file(repo_path("apps/lite_nginx/conf/scripts/health.js")));
    EXPECT_NE(script, nullptr);
}

TEST(ExampleScriptConfigs, EchoCompiles) {
    auto script = compile(read_file(repo_path("apps/lite_nginx/conf/scripts/echo.js")));
    EXPECT_NE(script, nullptr);
}

TEST(ExampleScriptConfigs, InspectCompiles) {
    auto script = compile(read_file(repo_path("apps/lite_nginx/conf/scripts/inspect.js")));
    EXPECT_NE(script, nullptr);
}
