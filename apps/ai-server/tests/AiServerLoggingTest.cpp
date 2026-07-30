#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

#include <log/Log.h>
#include <log/LoggerManager.h>

#include "AiServerLogging.h"
#include "observability/AiServerLogCategories.h"

namespace {

DEFINE_LOGGER(LOG_TEST_AUDIT, fiber::ai_server::kAiServerAuditLogger);
DEFINE_LOGGER(LOG_TEST_HTTP, fiber::ai_server::kAiServerHttpLogger);
DEFINE_LOGGER(LOG_TEST_LIFECYCLE, fiber::ai_server::kAiServerLifecycleLogger);

using fiber::ai_server::AiServerLoggingErrorCode;

class TempFile {
public:
    explicit TempFile(std::string_view prefix) {
        std::string pattern = "/tmp/";
        pattern.append(prefix);
        pattern.append("-XXXXXX");
        path_ = std::move(pattern);
        const int fd = ::mkstemp(path_.data());
        if (fd < 0) {
            path_.clear();
            return;
        }
        (void) ::close(fd);
    }

    ~TempFile() {
        if (!path_.empty()) {
            (void) ::unlink(path_.c_str());
        }
    }

    TempFile(const TempFile &) = delete;
    TempFile &operator=(const TempFile &) = delete;

    [[nodiscard]] bool valid() const noexcept { return !path_.empty(); }
    [[nodiscard]] const std::string &path() const noexcept { return path_; }

private:
    std::string path_;
};

class TempDirectory {
public:
    TempDirectory() {
        std::string pattern = "/tmp/fiber-ai-logging-dir-XXXXXX";
        char *created = ::mkdtemp(pattern.data());
        if (created) {
            path_ = created;
        }
    }

    ~TempDirectory() {
        if (path_.empty()) {
            return;
        }
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TempDirectory(const TempDirectory &) = delete;
    TempDirectory &operator=(const TempDirectory &) = delete;

    [[nodiscard]] bool valid() const noexcept { return !path_.empty(); }
    [[nodiscard]] const std::string &path() const noexcept { return path_; }
    [[nodiscard]] std::string target_path() const { return path_ + "/target.ndjson"; }
    [[nodiscard]] std::string audit_path() const { return path_ + "/audit.ndjson"; }

private:
    std::string path_;
};

class LoggingScope {
public:
    LoggingScope() { fiber::log::LoggerManager::global().shutdown(); }
    ~LoggingScope() { fiber::log::LoggerManager::global().shutdown(); }
};

std::string console_config(std::string_view audit_path) {
    return R"({
  "version": 1,
  "queue": {"capacity_bytes": 67108864},
  "appenders": [
    {
      "name": "stderr",
      "type": "console",
      "stream": "stderr",
      "min_level": "trace",
      "max_level": "fatal"
    }
  ],
  "root_logger": {
    "level": "info",
    "verbosity": 0,
    "appenders": ["stderr"]
  },
  "loggers": [],
  "audit": {
    "path": ")" +
           std::string(audit_path) + R"(",
    "max_record_bytes": 134217728,
    "rotate_bytes": 0,
    "max_archives": 30
  }
})";
}

std::string file_config(std::string_view operational_path, std::string_view audit_path) {
    return R"({
  "version": 1,
  "queue": {"capacity_bytes": 67108864},
  "appenders": [
    {
      "name": "operational",
      "type": "file",
      "path": ")" +
           std::string(operational_path) + R"(",
      "mode": "0644",
      "min_level": "trace",
      "max_level": "fatal"
    }
  ],
  "root_logger": {
    "level": "info",
    "verbosity": 0,
    "appenders": ["operational"]
  },
  "loggers": [
    {
      "name": "ai_server.lifecycle",
      "level": "error"
    },
    {
      "name": "ai_server.http",
      "level": "debug",
      "verbosity": 2
    }
  ],
  "audit": {
    "path": ")" +
           std::string(audit_path) + R"(",
    "max_record_bytes": 4096,
    "rotate_bytes": 0,
    "max_archives": 30
  }
})";
}

bool replace_once(std::string &value, std::string_view from, std::string_view to) {
    const std::size_t position = value.find(from);
    if (position == std::string::npos) {
        return false;
    }
    value.replace(position, from.size(), to);
    return true;
}

void write_file(std::string_view path, std::string_view content) {
    std::ofstream output(std::string(path), std::ios::binary | std::ios::trunc);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string read_file(std::string_view path) {
    std::ifstream input(std::string(path), std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

} // namespace

TEST(AiServerLoggingTest, LoadsExampleAndResolvesRelativeAuditPath) {
    auto config = fiber::ai_server::load_ai_server_log_config(FIBER_AI_SERVER_TEST_LOG_CONFIG_PATH);

    ASSERT_TRUE(config) << config.error().detail;
    const auto expected_path =
            (std::filesystem::path(FIBER_AI_SERVER_TEST_LOG_CONFIG_PATH).parent_path() / "ai-server-audit.ndjson")
                    .lexically_normal();
    EXPECT_EQ(config->audit.path, expected_path.string());
    EXPECT_EQ(config->audit.max_record_bytes, 134217728u);
    EXPECT_EQ(config->audit.rotate_bytes, 1073741824u);
    EXPECT_EQ(config->audit.max_archives, 30u);
    EXPECT_NE(config->audit_appender_id, fiber::log::kInvalidAppenderId);
}

TEST(AiServerLoggingTest, ReportsSyntaxPositionAndRejectsOversizedInput) {
    auto malformed = fiber::ai_server::parse_ai_server_log_config("{\n  \"version\": 1,\n]", "/tmp/logging.json");

    ASSERT_FALSE(malformed);
    EXPECT_EQ(malformed.error().code, AiServerLoggingErrorCode::InvalidJson);
    EXPECT_EQ(malformed.error().line, 3u);
    EXPECT_NE(malformed.error().column, 0u);

    const std::string oversized(1024 * 1024 + 1, ' ');
    auto too_large = fiber::ai_server::parse_ai_server_log_config(oversized, "/tmp/logging.json");
    ASSERT_FALSE(too_large);
    EXPECT_EQ(too_large.error().code, AiServerLoggingErrorCode::FileTooLarge);
}

TEST(AiServerLoggingTest, StrictlyRejectsUnknownDuplicateReservedAndInvalidReferences) {
    std::string unknown = console_config("audit.ndjson");
    ASSERT_TRUE(replace_once(unknown, R"("version": 1,)", R"("version": 1, "full_policy": "block",)"));
    auto unknown_result = fiber::ai_server::parse_ai_server_log_config(unknown, "/tmp/logging.json");
    ASSERT_FALSE(unknown_result);
    EXPECT_EQ(unknown_result.error().code, AiServerLoggingErrorCode::UnknownField);
    EXPECT_EQ(unknown_result.error().field, "/full_policy");

    std::string duplicate = console_config("audit.ndjson");
    ASSERT_TRUE(replace_once(duplicate, R"("version": 1,)", R"("version": 1, "version": 1,)"));
    auto duplicate_result = fiber::ai_server::parse_ai_server_log_config(duplicate, "/tmp/logging.json");
    ASSERT_FALSE(duplicate_result);
    EXPECT_EQ(duplicate_result.error().code, AiServerLoggingErrorCode::DuplicateField);
    EXPECT_EQ(duplicate_result.error().field, "/version");

    std::string reserved = console_config("audit.ndjson");
    ASSERT_TRUE(replace_once(reserved, R"("loggers": [])",
                             R"("loggers": [{"name": "ai_server.audit", "level": "debug"}])"));
    auto reserved_result = fiber::ai_server::parse_ai_server_log_config(reserved, "/tmp/logging.json");
    ASSERT_FALSE(reserved_result);
    EXPECT_EQ(reserved_result.error().code, AiServerLoggingErrorCode::ReservedName);
    EXPECT_EQ(reserved_result.error().field, "/loggers/0/name");

    std::string invalid_reference = console_config("audit.ndjson");
    ASSERT_TRUE(replace_once(invalid_reference, R"("appenders": ["stderr"])", R"("appenders": ["missing"])"));
    auto invalid_reference_result =
            fiber::ai_server::parse_ai_server_log_config(invalid_reference, "/tmp/logging.json");
    ASSERT_FALSE(invalid_reference_result);
    EXPECT_EQ(invalid_reference_result.error().code, AiServerLoggingErrorCode::InvalidReference);
    EXPECT_EQ(invalid_reference_result.error().field, "/root_logger/appenders/0");

    std::string stdout_appender = console_config("audit.ndjson");
    ASSERT_TRUE(replace_once(stdout_appender, R"("stream": "stderr")", R"("stream": "stdout")"));
    auto stdout_result = fiber::ai_server::parse_ai_server_log_config(stdout_appender, "/tmp/logging.json");
    ASSERT_FALSE(stdout_result);
    EXPECT_EQ(stdout_result.error().code, AiServerLoggingErrorCode::InvalidValue);
    EXPECT_EQ(stdout_result.error().field, "/appenders/0/stream");
}

TEST(AiServerLoggingTest, RejectsProtectedAuditOptionsAndInvalidFileTopology) {
    std::string unsupported = console_config("audit.ndjson");
    ASSERT_TRUE(replace_once(unsupported, R"("version": 1)", R"("version": 2)"));
    auto unsupported_result = fiber::ai_server::parse_ai_server_log_config(unsupported, "/tmp/logging.json");
    ASSERT_FALSE(unsupported_result);
    EXPECT_EQ(unsupported_result.error().code, AiServerLoggingErrorCode::UnsupportedVersion);
    EXPECT_EQ(unsupported_result.error().field, "/version");

    std::string reserved_appender = console_config("audit.ndjson");
    ASSERT_TRUE(replace_once(reserved_appender, R"("name": "stderr")", R"("name": "ai_server_audit_file")"));
    auto reserved_appender_result =
            fiber::ai_server::parse_ai_server_log_config(reserved_appender, "/tmp/logging.json");
    ASSERT_FALSE(reserved_appender_result);
    EXPECT_EQ(reserved_appender_result.error().code, AiServerLoggingErrorCode::ReservedName);
    EXPECT_EQ(reserved_appender_result.error().field, "/appenders/0/name");

    std::string protected_audit = console_config("audit.ndjson");
    ASSERT_TRUE(replace_once(protected_audit, R"("max_archives": 30)", R"("max_archives": 30, "mode": "0644")"));
    auto protected_audit_result = fiber::ai_server::parse_ai_server_log_config(protected_audit, "/tmp/logging.json");
    ASSERT_FALSE(protected_audit_result);
    EXPECT_EQ(protected_audit_result.error().code, AiServerLoggingErrorCode::UnknownField);
    EXPECT_EQ(protected_audit_result.error().field, "/audit/mode");

    auto collision =
            fiber::ai_server::parse_ai_server_log_config(file_config("shared.log", "shared.log"), "/tmp/logging.json");
    ASSERT_FALSE(collision);
    EXPECT_EQ(collision.error().code, AiServerLoggingErrorCode::InvalidValue);
    EXPECT_EQ(collision.error().field, "/audit/path");

    std::string partial_buffer = file_config("operational.log", "audit.ndjson");
    ASSERT_TRUE(replace_once(partial_buffer, R"("mode": "0644",)", R"("mode": "0644", "buffer_bytes": 4096,)"));
    auto partial_buffer_result = fiber::ai_server::parse_ai_server_log_config(partial_buffer, "/tmp/logging.json");
    ASSERT_FALSE(partial_buffer_result);
    EXPECT_EQ(partial_buffer_result.error().code, AiServerLoggingErrorCode::InvalidValue);
    EXPECT_EQ(partial_buffer_result.error().field, "/appenders/0");
}

TEST(AiServerLoggingTest, AppliesCategoryOverridesAndKeepsAuditIsolated) {
    TempFile operational("fiber-ai-operational");
    TempFile audit("fiber-ai-audit");
    ASSERT_TRUE(operational.valid());
    ASSERT_TRUE(audit.valid());
    write_file(audit.path(), "complete\npartial");
    ASSERT_EQ(::chmod(audit.path().c_str(), 0666), 0);
    LoggingScope logging;

    auto config = fiber::ai_server::parse_ai_server_log_config(file_config(operational.path(), audit.path()),
                                                               "/tmp/ai-server.logging.json");
    ASSERT_TRUE(config) << config.error().detail;
    const fiber::log::AppenderId audit_appender_id = config->audit_appender_id;
    auto initialized = fiber::log::LoggerManager::global().initialize(std::move(config->config));
    ASSERT_TRUE(initialized) << initialized.error().message;

    const fiber::log::Logger *audit_logger =
            fiber::log::LoggerManager::global().find_logger(fiber::ai_server::kAiServerAuditLogger);
    const fiber::log::Logger *http_logger =
            fiber::log::LoggerManager::global().find_logger(fiber::ai_server::kAiServerHttpLogger);
    const fiber::log::Logger *lifecycle_logger =
            fiber::log::LoggerManager::global().find_logger(fiber::ai_server::kAiServerLifecycleLogger);
    ASSERT_NE(audit_logger, nullptr);
    ASSERT_NE(http_logger, nullptr);
    ASSERT_NE(lifecycle_logger, nullptr);
    EXPECT_FALSE(audit_logger->enabled(fiber::log::LogLevel::Debug));
    EXPECT_TRUE(audit_logger->enabled(fiber::log::LogLevel::Info));
    EXPECT_FALSE(audit_logger->enabled(fiber::log::LogLevel::Warn));
    EXPECT_TRUE(http_logger->enabled(fiber::log::LogLevel::Debug));
    EXPECT_TRUE(http_logger->vlog_enabled(2));
    EXPECT_FALSE(http_logger->vlog_enabled(3));
    EXPECT_FALSE(lifecycle_logger->enabled(fiber::log::LogLevel::Info));
    EXPECT_TRUE(lifecycle_logger->enabled(fiber::log::LogLevel::Error));

    LOG(LOG_TEST_LIFECYCLE, INFO) << "suppressed-operational-record";
    LOG(LOG_TEST_LIFECYCLE, ERROR) << "visible-operational-record";
    ASSERT_TRUE(fiber::log::log_complete_message(LOG_TEST_AUDIT.get(), fiber::log::LogLevel::Info, __FILE__, __LINE__,
                                                 __func__, R"({"audit":true})"));
    fiber::log::LoggerManager::global().flush();
    EXPECT_EQ(fiber::log::LoggerManager::global().appender_stats(audit_appender_id).written_records, 1u);
    fiber::log::LoggerManager::global().shutdown();

    const std::string operational_content = read_file(operational.path());
    EXPECT_NE(operational_content.find("visible-operational-record"), std::string::npos);
    EXPECT_EQ(operational_content.find("suppressed-operational-record"), std::string::npos);
    EXPECT_EQ(operational_content.find(R"({"audit":true})"), std::string::npos);
    const std::string audit_content = read_file(audit.path());
    EXPECT_EQ(audit_content, "complete\n{\"audit\":true}\n");
    EXPECT_EQ(audit_content.find(" ai_server.audit "), std::string::npos);
    EXPECT_EQ(audit_content.find("visible-operational-record"), std::string::npos);
    EXPECT_EQ(std::count(audit_content.begin(), audit_content.end(), '\n'), 2);

    struct stat status{};
    ASSERT_EQ(::stat(audit.path().c_str(), &status), 0);
    EXPECT_EQ(status.st_mode & 0777, 0600);
}

TEST(AiServerLoggingTest, AuditArchivesIncludeUtcTimestampAndSequence) {
    TempDirectory directory;
    ASSERT_TRUE(directory.valid());
    LoggingScope logging;

    std::string source = console_config(directory.audit_path());
    ASSERT_TRUE(replace_once(source, R"("rotate_bytes": 0)", R"("rotate_bytes": 64)"));
    auto config = fiber::ai_server::parse_ai_server_log_config(source, "/tmp/ai-server.logging.json");
    ASSERT_TRUE(config) << config.error().detail;
    auto initialized = fiber::log::LoggerManager::global().initialize(std::move(config->config));
    ASSERT_TRUE(initialized) << initialized.error().message;

    const std::string record = R"({"payload":")" + std::string(96, 'x') + R"("})";
    ASSERT_TRUE(fiber::log::log_complete_message(LOG_TEST_AUDIT.get(), fiber::log::LogLevel::Info, __FILE__, __LINE__,
                                                 __func__, record));
    ASSERT_TRUE(fiber::log::log_complete_message(LOG_TEST_AUDIT.get(), fiber::log::LogLevel::Info, __FILE__, __LINE__,
                                                 __func__, record));
    fiber::log::LoggerManager::global().shutdown();

    std::vector<std::filesystem::path> archives;
    for (const auto &entry: std::filesystem::directory_iterator(directory.path())) {
        const std::string name = entry.path().filename().string();
        if (name.starts_with("audit.ndjson.")) {
            archives.push_back(entry.path());
        }
    }
    ASSERT_EQ(archives.size(), 1u);

    constexpr std::string_view prefix = "audit.ndjson.";
    constexpr std::string_view suffix = ".000001";
    const std::string name = archives.front().filename().string();
    ASSERT_EQ(name.size(), prefix.size() + 16 + suffix.size());
    EXPECT_TRUE(name.starts_with(prefix));
    EXPECT_TRUE(name.ends_with(suffix));
    const std::string_view stamp(name.data() + prefix.size(), 16);
    EXPECT_EQ(stamp[8], 'T');
    EXPECT_EQ(stamp[15], 'Z');
    for (std::size_t index = 0; index < stamp.size(); ++index) {
        if (index != 8 && index != 15) {
            EXPECT_GE(stamp[index], '0');
            EXPECT_LE(stamp[index], '9');
        }
    }
    EXPECT_EQ(read_file(archives.front().string()), record + "\n");
    EXPECT_EQ(read_file(directory.audit_path()), record + "\n");
}

TEST(AiServerLoggingTest, AuditAppenderRejectsSymbolicLinks) {
    TempDirectory directory;
    ASSERT_TRUE(directory.valid());
    write_file(directory.target_path(), "target\n");
    ASSERT_EQ(::symlink(directory.target_path().c_str(), directory.audit_path().c_str()), 0);
    LoggingScope logging;

    auto config = fiber::ai_server::parse_ai_server_log_config(console_config(directory.audit_path()),
                                                               "/tmp/ai-server.logging.json");
    ASSERT_TRUE(config) << config.error().detail;
    auto initialized = fiber::log::LoggerManager::global().initialize(std::move(config->config));

    ASSERT_FALSE(initialized);
    EXPECT_EQ(initialized.error().code, fiber::log::LogConfigErrorCode::OpenFailed);
    EXPECT_EQ(initialized.error().system_error, ELOOP);
    EXPECT_EQ(read_file(directory.target_path()), "target\n");
}
