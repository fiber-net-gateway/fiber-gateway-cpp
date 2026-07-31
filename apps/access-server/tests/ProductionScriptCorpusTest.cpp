#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "config/AccessConfigCodec.h"
#include "routing/ProjectRouteSnapshot.h"
#include "runtime/AccessScriptRuntime.h"

namespace {

using fiber::access_server::AccessScriptRuntime;
using fiber::access_server::compile_project_config;
using fiber::access_server::parse_project_config;

std::string read_file(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

TEST(ProductionScriptCorpusTest, CompilesExternalSnapshotWhenProvided) {
    const char *raw_path = std::getenv("ACCESS_SERVER_SCRIPT_CORPUS_DIR");
    if (raw_path == nullptr || *raw_path == '\0') {
        GTEST_SKIP() << "ACCESS_SERVER_SCRIPT_CORPUS_DIR is not set";
    }

    std::error_code error;
    std::filesystem::directory_iterator iterator(raw_path, error);
    ASSERT_FALSE(error) << "cannot open corpus directory";

    std::vector<std::filesystem::path> files;
    const std::filesystem::directory_iterator end;
    while (iterator != end) {
        const std::filesystem::directory_entry &entry = *iterator;
        const bool regular_file = entry.is_regular_file(error);
        ASSERT_FALSE(error) << "cannot inspect corpus entry";
        if (regular_file && entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
        iterator.increment(error);
        ASSERT_FALSE(error) << "cannot enumerate corpus directory";
    }
    std::sort(files.begin(), files.end());
    ASSERT_FALSE(files.empty());

    AccessScriptRuntime scripts;
    std::size_t parsed = 0;
    std::size_t compiled = 0;
    std::size_t unloaded = 0;
    for (std::size_t index = 0; index < files.size(); ++index) {
        const std::string content = read_file(files[index]);
        ASSERT_FALSE(content.empty()) << "empty corpus entry index=" << index;

        auto config = parse_project_config(content);
        ASSERT_TRUE(config) << "decode failed for corpus entry index=" << index << " field=" << config.error().field
                            << " message=" << config.error().message;
        ++parsed;
        if (!*config) {
            ++unloaded;
            continue;
        }

        auto snapshot = compile_project_config("corpus", **config, scripts.compiler_adapter());
        ASSERT_TRUE(snapshot) << "compile failed for corpus entry index=" << index
                              << " field=" << snapshot.error().field << " message=" << snapshot.error().message;
        if (*snapshot) {
            ++compiled;
        } else {
            ++unloaded;
        }
    }

    EXPECT_EQ(parsed, files.size());
    EXPECT_EQ(compiled + unloaded, files.size());
}

} // namespace
