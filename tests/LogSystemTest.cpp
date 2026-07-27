#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "log/Log.h"

using namespace std::chrono_literals;

DEFINE_LOGGER(LOG_TEST_AUTH, "test.gateway.auth");
DEFINE_LOGGER(LOG_TEST_AUTH_DUP, "test.gateway.auth");
DEFINE_LOGGER(LOG_TEST_HTTP, "test.gateway.http");
DEFINE_LOGGER(LOG_TEST_GATEWAY2, "test.gateway2");
DEFINE_LOGGER(LOG_TEST_ISOLATED, "test.isolated.child");
DEFINE_LOGGER(LOG_TEST_OTHER, "test.other");
DEFINE_LOGGER(LOG_TEST_MACRO, "test.macro");
DEFINE_LOGGER(LOG_TEST_BUFFER, "test.buffer");
DEFINE_LOGGER(LOG_TEST_TIMER, "test.timer");
DEFINE_LOGGER(LOG_TEST_REOPEN, "test.reopen");
DEFINE_LOGGER(LOG_TEST_CONCURRENT, "test.concurrent");

namespace {

namespace fs = std::filesystem;

class TempLogFile {
public:
    TempLogFile() {
        char pattern[] = "/tmp/fiber_log_test_XXXXXX";
        const int fd = ::mkstemp(pattern);
        if (fd >= 0) {
            ::close(fd);
            path_ = pattern;
            rotated_path_ = path_ + ".old";
        }
    }

    ~TempLogFile() {
        if (!path_.empty()) {
            ::unlink(path_.c_str());
            ::unlink(rotated_path_.c_str());
        }
    }

    TempLogFile(const TempLogFile &) = delete;
    TempLogFile &operator=(const TempLogFile &) = delete;

    [[nodiscard]] const std::string &path() const noexcept { return path_; }
    [[nodiscard]] const std::string &rotated_path() const noexcept { return rotated_path_; }
    [[nodiscard]] bool valid() const noexcept { return !path_.empty(); }
    [[nodiscard]] bool rotate() const noexcept { return ::rename(path_.c_str(), rotated_path_.c_str()) == 0; }

private:
    std::string path_;
    std::string rotated_path_;
};

class TempLogDirectory {
public:
    TempLogDirectory() {
        char pattern[] = "/tmp/fiber_log_rotation_test_XXXXXX";
        if (char *path = ::mkdtemp(pattern)) {
            path_ = path;
            log_path_ = path_ + "/output.log";
        }
    }

    ~TempLogDirectory() {
        if (!path_.empty()) {
            std::error_code error;
            fs::remove_all(path_, error);
        }
    }

    TempLogDirectory(const TempLogDirectory &) = delete;
    TempLogDirectory &operator=(const TempLogDirectory &) = delete;

    [[nodiscard]] bool valid() const noexcept { return !path_.empty(); }
    [[nodiscard]] const std::string &path() const noexcept { return path_; }
    [[nodiscard]] const std::string &log_path() const noexcept { return log_path_; }

private:
    std::string path_;
    std::string log_path_;
};

std::string read_file(const std::string &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::size_t count_occurrences(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::string_view::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

std::vector<std::string> list_output_log_files(const TempLogDirectory &directory) {
    std::vector<std::string> files;
    std::error_code error;
    for (fs::directory_iterator it(directory.path(), error), end; !error && it != end; it.increment(error)) {
        if (it->is_regular_file(error) && it->path().filename().string().starts_with("output.log")) {
            files.push_back(it->path().string());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string read_files(const std::vector<std::string> &paths) {
    std::string content;
    for (const auto &path: paths) {
        content.append(read_file(path));
    }
    return content;
}

template<typename Predicate>
bool wait_until(Predicate predicate, std::chrono::steady_clock::duration timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

std::uint64_t current_thread_id() noexcept {
#if defined(SYS_gettid)
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

class LoggingScope {
public:
    LoggingScope() { fiber::log::LoggerManager::global().shutdown(); }
    ~LoggingScope() { fiber::log::LoggerManager::global().shutdown(); }
};

class StderrPipeCapture {
public:
    StderrPipeCapture() = default;
    ~StderrPipeCapture() { finish(); }

    StderrPipeCapture(const StderrPipeCapture &) = delete;
    StderrPipeCapture &operator=(const StderrPipeCapture &) = delete;

    [[nodiscard]] bool start() {
        int pipe_fds[2];
        if (::pipe(pipe_fds) != 0) {
            return false;
        }
        saved_stderr_ = ::dup(STDERR_FILENO);
        if (saved_stderr_ < 0) {
            ::close(pipe_fds[0]);
            ::close(pipe_fds[1]);
            return false;
        }
        if (::dup2(pipe_fds[1], STDERR_FILENO) < 0) {
            ::close(saved_stderr_);
            saved_stderr_ = -1;
            ::close(pipe_fds[0]);
            ::close(pipe_fds[1]);
            return false;
        }
        ::close(pipe_fds[1]);
        read_fd_ = pipe_fds[0];
        active_ = true;
        reader_ = std::thread([this]() {
            char buffer[8192];
            for (;;) {
                const ssize_t size = ::read(read_fd_, buffer, sizeof(buffer));
                if (size > 0) {
                    output_.append(buffer, static_cast<std::size_t>(size));
                    continue;
                }
                if (size < 0 && errno == EINTR) {
                    continue;
                }
                break;
            }
            ::close(read_fd_);
            read_fd_ = -1;
        });
        return true;
    }

    void finish() {
        if (!active_) {
            return;
        }
        (void) ::dup2(saved_stderr_, STDERR_FILENO);
        ::close(saved_stderr_);
        saved_stderr_ = -1;
        if (reader_.joinable()) {
            reader_.join();
        }
        active_ = false;
    }

    [[nodiscard]] const std::string &output() const noexcept { return output_; }

private:
    std::thread reader_;
    std::string output_;
    int saved_stderr_ = -1;
    int read_fd_ = -1;
    bool active_ = false;
};

} // namespace

TEST(LogBacklogTest, AppliesDropNewestAndWakesBlockedAdmissionOnClose) {
    auto make_record = []() {
        return fiber::log::OwnedLogRecord::create("backlog.test", nullptr, 0, fiber::log::LogLevel::Info, __FILE__,
                                                  __LINE__, __func__, 0, 0);
    };

    fiber::log::OwnedLogRecord *drop_first = make_record();
    fiber::log::OwnedLogRecord *drop_second = make_record();
    ASSERT_NE(drop_first, nullptr);
    ASSERT_NE(drop_second, nullptr);
    const std::size_t record_size = drop_first->allocated_bytes();
    fiber::log::LogBacklog dropping({
            .backlog_capacity = record_size,
            .full_policy = fiber::log::LogQueueFullPolicy::DropNewest,
    });
    EXPECT_TRUE(dropping.admit(*drop_first));
    EXPECT_FALSE(dropping.admit(*drop_second));
    EXPECT_EQ(dropping.stats().queued_records, 1u);
    EXPECT_EQ(dropping.stats().dropped_records, 1u);
    dropping.release(record_size, false);
    delete drop_first;
    delete drop_second;

    fiber::log::OwnedLogRecord *block_first = make_record();
    fiber::log::OwnedLogRecord *block_second = make_record();
    ASSERT_NE(block_first, nullptr);
    ASSERT_NE(block_second, nullptr);
    fiber::log::LogBacklog blocking({
            .backlog_capacity = block_first->allocated_bytes(),
            .full_policy = fiber::log::LogQueueFullPolicy::Block,
    });
    ASSERT_TRUE(blocking.admit(*block_first));

    std::promise<void> entered;
    std::promise<bool> admitted;
    auto entered_future = entered.get_future();
    auto admitted_future = admitted.get_future();
    std::thread waiter([&]() {
        entered.set_value();
        admitted.set_value(blocking.admit(*block_second));
    });
    entered_future.wait();
    EXPECT_EQ(admitted_future.wait_for(10ms), std::future_status::timeout);
    blocking.stop_accepting();
    EXPECT_FALSE(admitted_future.get());
    waiter.join();

    blocking.release(block_first->allocated_bytes(), false);
    delete block_first;
    delete block_second;
}

TEST(LogConfigTest, RejectsInvalidOptionsAndMissingRoot) {
    fiber::log::LogConfigBuilder builder;
    auto bad_name = builder.add_console_appender({.name = "bad..name"});
    ASSERT_FALSE(bad_name);
    EXPECT_EQ(bad_name.error().code, fiber::log::LogConfigErrorCode::InvalidName);

    auto bad_async = builder.set_async_options({.backlog_capacity = 0});
    ASSERT_FALSE(bad_async);
    EXPECT_EQ(bad_async.error().code, fiber::log::LogConfigErrorCode::InvalidBufferOptions);

    auto finish = builder.finish();
    ASSERT_FALSE(finish);
    EXPECT_EQ(finish.error().code, fiber::log::LogConfigErrorCode::MissingRootLogger);

    fiber::log::LogConfigBuilder rotation_builder;
    auto bad_rotation = rotation_builder.add_file_appender({
            .name = "bad_rotation",
            .path = "/tmp/bad_rotation.log",
            .rotation =
                    fiber::log::FileRotationOptions{
                            .max_file_size = 8192,
                            .archive_name = "{base}",
                            .max_archives = 4,
                    },
    });
    ASSERT_FALSE(bad_rotation);
    EXPECT_EQ(bad_rotation.error().code, fiber::log::LogConfigErrorCode::InvalidArchiveName);
}

TEST(LogSystemTest, MaterializesLoggerRequestedByRuntimeName) {
    LoggingScope scope;
    TempLogFile output;
    ASSERT_TRUE(output.valid());

    fiber::log::LogConfigBuilder builder;
    auto output_id = builder.add_file_appender({.name = "dynamic_output", .path = output.path()});
    ASSERT_TRUE(output_id);
    ASSERT_TRUE(builder.set_root_logger({.level = fiber::log::LogLevel::Info}, {*output_id}));
    ASSERT_TRUE(builder.request_logger("runtime.access"));
    auto config = builder.finish();
    ASSERT_TRUE(config);
    auto initialized = fiber::log::LoggerManager::global().initialize(std::move(*config));
    ASSERT_TRUE(initialized) << initialized.error().message;

    const fiber::log::Logger *logger = fiber::log::LoggerManager::global().find_logger("runtime.access");
    ASSERT_NE(logger, nullptr);
    fiber::log::LogLine(*logger, fiber::log::LogLevel::Info, __FILE__, __LINE__, __func__) << "dynamic-message";

    fiber::log::LoggerManager::global().shutdown();
    EXPECT_NE(read_file(output.path()).find("dynamic-message"), std::string::npos);
}

TEST(LogSystemTest, CompleteMessageHasNoConfiguredSizeLimit) {
    LoggingScope scope;
    TempLogFile output;
    ASSERT_TRUE(output.valid());

    fiber::log::LogConfigBuilder builder;
    auto output_id = builder.add_file_appender({.name = "complete_output", .path = output.path()});
    ASSERT_TRUE(output_id);
    ASSERT_TRUE(builder.set_async_options({.backlog_capacity = 64 * 1024}));
    ASSERT_TRUE(builder.set_root_logger({.level = fiber::log::LogLevel::Info}, {*output_id}));
    auto config = builder.finish();
    ASSERT_TRUE(config);
    ASSERT_TRUE(fiber::log::LoggerManager::global().initialize(std::move(*config)));

    std::string message = R"({"event":"large","content":")";
    message.append(2 * 1024 * 1024, 'x');
    message.append(R"("})");
    ASSERT_TRUE(fiber::log::log_complete_message(LOG_TEST_OTHER.get(), fiber::log::LogLevel::Info, __FILE__, __LINE__,
                                                 __func__, message));
    fiber::log::LoggerManager::global().flush();

    const fiber::log::LogQueueStats stats = fiber::log::LoggerManager::global().queue_stats();
    EXPECT_EQ(stats.queued_records, 0u);
    EXPECT_GT(stats.peak_queued_bytes, 64u * 1024u);
    EXPECT_EQ(stats.dropped_records, 0u);

    fiber::log::LoggerManager::global().shutdown();
    const std::string content = read_file(output.path());
    EXPECT_NE(content.find(message), std::string::npos);
    EXPECT_EQ(std::count(content.begin(), content.end(), '\n'), 1);
    EXPECT_EQ(content.find("<truncated>"), std::string::npos);
}

TEST(LogSystemTest, CompilesHierarchyIntoPerLevelAppenderArrays) {
    LoggingScope scope;
    TempLogFile all;
    TempLogFile gateway;
    TempLogFile auth;
    TempLogFile errors;
    ASSERT_TRUE(all.valid());
    ASSERT_TRUE(gateway.valid());
    ASSERT_TRUE(auth.valid());
    ASSERT_TRUE(errors.valid());

    fiber::log::LogConfigBuilder builder;
    auto all_id = builder.add_file_appender({.name = "all", .path = all.path()});
    auto gateway_id = builder.add_file_appender({.name = "gateway", .path = gateway.path()});
    auto auth_id = builder.add_file_appender({.name = "auth", .path = auth.path()});
    auto error_id = builder.add_file_appender(
            {.name = "errors", .path = errors.path(), .min_level = fiber::log::LogLevel::Error});
    ASSERT_TRUE(all_id);
    ASSERT_TRUE(gateway_id);
    ASSERT_TRUE(auth_id);
    ASSERT_TRUE(error_id);

    ASSERT_TRUE(builder.add_logger({.name = "test.gateway", .level = fiber::log::LogLevel::Debug, .additive = true},
                                   {*gateway_id, *all_id}));
    ASSERT_TRUE(builder.add_logger({.name = "test.gateway.auth", .level = fiber::log::LogLevel::Info, .additive = true},
                                   {*auth_id}));
    ASSERT_TRUE(builder.add_logger({.name = "test.isolated", .level = fiber::log::LogLevel::Debug, .additive = false},
                                   {*auth_id}));
    ASSERT_TRUE(builder.set_root_logger({.level = fiber::log::LogLevel::Info}, {*all_id, *error_id}));
    auto config = builder.finish();
    ASSERT_TRUE(config);
    ASSERT_TRUE(fiber::log::LoggerManager::global().initialize(std::move(*config)));

    EXPECT_EQ(&LOG_TEST_AUTH.get(), &LOG_TEST_AUTH_DUP.get());
    EXPECT_EQ(LOG_TEST_AUTH.get().targets(fiber::log::LogLevel::Debug).count, 0u);
    EXPECT_EQ(LOG_TEST_AUTH.get().targets(fiber::log::LogLevel::Info).count, 3u);
    EXPECT_EQ(LOG_TEST_AUTH.get().targets(fiber::log::LogLevel::Error).count, 4u);
    const auto &info_targets = LOG_TEST_AUTH.get().targets(fiber::log::LogLevel::Info);
    const auto &warn_targets = LOG_TEST_AUTH.get().targets(fiber::log::LogLevel::Warn);
    const auto &error_targets = LOG_TEST_AUTH.get().targets(fiber::log::LogLevel::Error);
    EXPECT_EQ(info_targets.first + info_targets.count, warn_targets.first);
    EXPECT_EQ(warn_targets.first + warn_targets.count, error_targets.first);
    EXPECT_EQ(LOG_TEST_HTTP.get().targets(fiber::log::LogLevel::Debug).count, 2u);
    EXPECT_EQ(LOG_TEST_GATEWAY2.get().targets(fiber::log::LogLevel::Debug).count, 0u);
    EXPECT_EQ(LOG_TEST_GATEWAY2.get().targets(fiber::log::LogLevel::Info).count, 1u);
    EXPECT_EQ(LOG_TEST_ISOLATED.get().targets(fiber::log::LogLevel::Debug).count, 1u);
    EXPECT_EQ(LOG_TEST_ISOLATED.get().targets(fiber::log::LogLevel::Error).count, 1u);

    LOG(LOG_TEST_AUTH, INFO) << "auth-info";
    LOG(LOG_TEST_HTTP, DEBUG) << "http-debug";
    LOG(LOG_TEST_OTHER, INFO) << "other-info";
    LOG(LOG_TEST_ISOLATED, INFO) << "isolated-info";
    LOG(LOG_TEST_AUTH, ERROR) << "auth-error";

    fiber::log::LoggerManager::global().shutdown();
    const std::string auth_content = read_file(auth.path());
    const std::string gateway_content = read_file(gateway.path());
    const std::string all_content = read_file(all.path());
    const std::string error_content = read_file(errors.path());

    EXPECT_NE(auth_content.find("auth-info"), std::string::npos);
    EXPECT_NE(auth_content.find("auth-error"), std::string::npos);
    EXPECT_NE(auth_content.find("isolated-info"), std::string::npos);
    EXPECT_NE(gateway_content.find("http-debug"), std::string::npos);
    EXPECT_EQ(gateway_content.find("isolated-info"), std::string::npos);
    EXPECT_NE(all_content.find("other-info"), std::string::npos);
    EXPECT_EQ(count_occurrences(all_content, "auth-info"), 1u);
    EXPECT_EQ(error_content.find("auth-info"), std::string::npos);
    EXPECT_NE(error_content.find("auth-error"), std::string::npos);

    EXPECT_EQ(&LOG_TEST_AUTH.get(), &fiber::log::bootstrap_logger());
    EXPECT_EQ(&LOG_TEST_AUTH_DUP.get(), &fiber::log::bootstrap_logger());
}

TEST(LogSystemTest, InitializationFailureLeavesHandlesOnBootstrapLogger) {
    LoggingScope scope;
    TempLogFile path_blocker;
    ASSERT_TRUE(path_blocker.valid());
    const fiber::log::Logger *before = &LOG_TEST_OTHER.get();
    ASSERT_EQ(before, &fiber::log::bootstrap_logger());

    fiber::log::LogConfigBuilder builder;
    auto output_id = builder.add_file_appender({.name = "cannot_open", .path = path_blocker.path() + "/output.log"});
    ASSERT_TRUE(output_id);
    ASSERT_TRUE(builder.set_root_logger({.level = fiber::log::LogLevel::Info}, {*output_id}));
    auto config = builder.finish();
    ASSERT_TRUE(config);

    auto initialized = fiber::log::LoggerManager::global().initialize(std::move(*config));
    ASSERT_FALSE(initialized);
    EXPECT_EQ(initialized.error().code, fiber::log::LogConfigErrorCode::OpenFailed);
    EXPECT_EQ(&LOG_TEST_OTHER.get(), before);
}

TEST(LogSystemTest, DisabledAndConditionalMacrosDoNotEvaluateMessages) {
    LoggingScope scope;
    TempLogFile output;
    ASSERT_TRUE(output.valid());

    fiber::log::LogConfigBuilder builder;
    auto output_id = builder.add_file_appender({.name = "macro_output", .path = output.path()});
    ASSERT_TRUE(output_id);
    ASSERT_TRUE(builder.add_logger(
            {.name = "test.macro", .level = fiber::log::LogLevel::Debug, .verbosity = 2, .additive = true},
            std::initializer_list<fiber::log::AppenderId>{}));
    ASSERT_TRUE(builder.set_root_logger({.level = fiber::log::LogLevel::Warn}, {*output_id}));
    auto config = builder.finish();
    ASSERT_TRUE(config);
    ASSERT_TRUE(fiber::log::LoggerManager::global().initialize(std::move(*config)));

    int message_calls = 0;
    int condition_calls = 0;
    LOG(LOG_TEST_MACRO, TRACE) << ++message_calls;
    LOG_IF(LOG_TEST_MACRO, INFO, (++condition_calls, false)) << ++message_calls;
    LOG_IF(LOG_TEST_MACRO, INFO, (++condition_calls, true)) << "condition " << ++message_calls;
    VLOG(LOG_TEST_MACRO, 3) << ++message_calls;
    VLOG(LOG_TEST_MACRO, 2) << "verbose " << ++message_calls;

    int else_branch = 0;
    if (false)
        LOG(LOG_TEST_MACRO, INFO) << "unreachable";
    else
        else_branch = 1;

    EXPECT_EQ(condition_calls, 2);
    EXPECT_EQ(message_calls, 2);
    EXPECT_EQ(else_branch, 1);
    fiber::log::LoggerManager::global().shutdown();

    const std::string content = read_file(output.path());
    EXPECT_NE(content.find("condition 1"), std::string::npos);
    EXPECT_NE(content.find("verbose 2"), std::string::npos);
    EXPECT_EQ(content.find("unreachable"), std::string::npos);
}

TEST(LogSystemTest, EscapesControlCharactersWithoutTruncatingLongMessages) {
    LoggingScope scope;
    TempLogFile output;
    ASSERT_TRUE(output.valid());

    fiber::log::LogConfigBuilder builder;
    auto output_id = builder.add_file_appender({.name = "escape_output", .path = output.path()});
    ASSERT_TRUE(output_id);
    ASSERT_TRUE(builder.set_root_logger({.level = fiber::log::LogLevel::Trace}, {*output_id}));
    auto config = builder.finish();
    ASSERT_TRUE(config);
    ASSERT_TRUE(fiber::log::LoggerManager::global().initialize(std::move(*config)));

    const std::string long_message(256 * 1024, 'x');
    LOG(LOG_TEST_OTHER, INFO) << "alpha\nbeta\r\t";
    LOG(LOG_TEST_OTHER, INFO) << fiber::log::quoted("alpha\"beta\\gamma\n");
    LOG(LOG_TEST_OTHER, INFO) << long_message;
    fiber::log::LoggerManager::global().shutdown();

    const std::string content = read_file(output.path());
    EXPECT_NE(content.find("alpha\\nbeta\\r\\t"), std::string::npos);
    EXPECT_NE(content.find("\"alpha\\\"beta\\\\gamma\\n\""), std::string::npos);
    EXPECT_NE(content.find(long_message), std::string::npos);
    EXPECT_EQ(content.find("<truncated>"), std::string::npos);
    EXPECT_EQ(std::count(content.begin(), content.end(), '\n'), 3);
}

TEST(LogSystemTest, RawAppendPreservesBytesAndDiscardCancelsTheRecord) {
    LoggingScope scope;
    TempLogFile output;
    ASSERT_TRUE(output.valid());

    fiber::log::LogConfigBuilder builder;
    auto output_id = builder.add_file_appender({.name = "raw_output", .path = output.path()});
    ASSERT_TRUE(output_id);
    ASSERT_TRUE(builder.set_root_logger({.level = fiber::log::LogLevel::Info}, {*output_id}));
    auto config = builder.finish();
    ASSERT_TRUE(config);
    auto &manager = fiber::log::LoggerManager::global();
    ASSERT_TRUE(manager.initialize(std::move(*config)));

    std::string raw = "audit_json={\"control\":\"";
    raw.push_back('\x7f');
    raw.append("\"}");
    {
        fiber::log::LogLine line(LOG_TEST_OTHER.get(), fiber::log::LogLevel::Info, __FILE__, __LINE__, __func__);
        ASSERT_TRUE(line.append_raw(raw));
        ASSERT_TRUE(line.good());
    }
    {
        fiber::log::LogLine line(LOG_TEST_OTHER.get(), fiber::log::LogLevel::Info, __FILE__, __LINE__, __func__);
        ASSERT_TRUE(line.append_raw("discarded-partial-json"));
        line.discard();
    }
    manager.flush();
    EXPECT_EQ(manager.appender_stats(*output_id).written_records, 1u);
    manager.shutdown();

    const std::string content = read_file(output.path());
    EXPECT_NE(content.find(raw), std::string::npos);
    EXPECT_EQ(content.find("discarded-partial-json"), std::string::npos);
    EXPECT_EQ(std::count(content.begin(), content.end(), '\n'), 1);
}

TEST(LogSystemTest, SecureFileAppenderRecoversTailAndEnforcesMode) {
    LoggingScope scope;
    TempLogFile output;
    ASSERT_TRUE(output.valid());
    {
        std::ofstream file(output.path(), std::ios::binary | std::ios::trunc);
        file << "complete\npartial";
    }
    ASSERT_EQ(::chmod(output.path().c_str(), 0666), 0);

    fiber::log::LogConfigBuilder builder;
    auto output_id = builder.add_file_appender({
            .name = "secure_output",
            .path = output.path(),
            .file_mode = 0600,
            .no_follow = true,
            .regular_file_only = true,
            .enforce_file_mode = true,
            .truncate_incomplete_tail = true,
    });
    ASSERT_TRUE(output_id);
    ASSERT_TRUE(builder.set_root_logger({.level = fiber::log::LogLevel::Info}, {*output_id}));
    auto config = builder.finish();
    ASSERT_TRUE(config);
    auto &manager = fiber::log::LoggerManager::global();
    ASSERT_TRUE(manager.initialize(std::move(*config)));
    EXPECT_EQ(manager.appender_stats(*output_id).active_file_bytes, 9u);
    manager.shutdown();

    EXPECT_EQ(read_file(output.path()), "complete\n");
    struct stat file_stat{};
    ASSERT_EQ(::stat(output.path().c_str(), &file_stat), 0);
    EXPECT_EQ(file_stat.st_mode & 0777, 0600);
}

TEST(LogSystemTest, SecureFileAppenderRejectsSymbolicLink) {
    LoggingScope scope;
    TempLogDirectory output;
    ASSERT_TRUE(output.valid());
    const std::string target = output.path() + "/target.log";
    {
        std::ofstream file(target, std::ios::binary);
        file << "target\n";
    }
    ASSERT_EQ(::symlink(target.c_str(), output.log_path().c_str()), 0);

    fiber::log::LogConfigBuilder builder;
    auto output_id = builder.add_file_appender({
            .name = "no_follow_output",
            .path = output.log_path(),
            .file_mode = 0600,
            .no_follow = true,
            .regular_file_only = true,
            .enforce_file_mode = true,
            .truncate_incomplete_tail = true,
    });
    ASSERT_TRUE(output_id);
    ASSERT_TRUE(builder.set_root_logger({.level = fiber::log::LogLevel::Info}, {*output_id}));
    auto config = builder.finish();
    ASSERT_TRUE(config);

    auto initialized = fiber::log::LoggerManager::global().initialize(std::move(*config));
    ASSERT_FALSE(initialized);
    EXPECT_EQ(initialized.error().code, fiber::log::LogConfigErrorCode::OpenFailed);
    EXPECT_EQ(initialized.error().system_error, ELOOP);
    EXPECT_EQ(read_file(target), "target\n");
}

TEST(LogSystemTest, FormatsOnceAndFlushesAllAppenderBuffersOnWriterThread) {
    LoggingScope scope;
    TempLogFile direct_output;
    TempLogFile buffered_output;
    ASSERT_TRUE(direct_output.valid());
    ASSERT_TRUE(buffered_output.valid());

    fiber::log::LogConfigBuilder builder;
    auto direct_id = builder.add_file_appender({.name = "direct_output", .path = direct_output.path()});
    auto buffered_id = builder.add_file_appender({
            .name = "buffered_output",
            .path = buffered_output.path(),
            .buffer_size = 32 * 1024,
            .flush_interval = 1s,
    });
    ASSERT_TRUE(direct_id);
    ASSERT_TRUE(buffered_id);
    ASSERT_TRUE(builder.set_root_logger({.level = fiber::log::LogLevel::Info}, {*direct_id, *buffered_id}));
    auto config = builder.finish();
    ASSERT_TRUE(config);
    auto &manager = fiber::log::LoggerManager::global();
    ASSERT_TRUE(manager.initialize(std::move(*config)));

    LOG(LOG_TEST_BUFFER, INFO) << "shared-record-one";
    LOG(LOG_TEST_BUFFER, INFO) << "shared-record-two";
    manager.flush();

    const fiber::log::AppenderStats direct_stats = manager.appender_stats(*direct_id);
    const fiber::log::AppenderStats buffered_stats = manager.appender_stats(*buffered_id);
    const fiber::log::LogQueueStats queue_stats = manager.queue_stats();
    EXPECT_EQ(direct_stats.written_records, 2u);
    EXPECT_EQ(buffered_stats.written_records, 2u);
    EXPECT_EQ(direct_stats.writer_thread_id, queue_stats.writer_thread_id);
    EXPECT_EQ(buffered_stats.writer_thread_id, queue_stats.writer_thread_id);
    EXPECT_NE(queue_stats.writer_thread_id, current_thread_id());

    manager.shutdown();
    const std::string direct_content = read_file(direct_output.path());
    const std::string buffered_content = read_file(buffered_output.path());
    EXPECT_FALSE(direct_content.empty());
    EXPECT_EQ(direct_content, buffered_content);
    EXPECT_EQ(std::count(direct_content.begin(), direct_content.end(), '\n'), 2);
}

TEST(LogSystemTest, DedicatedTimerFlushesLowTrafficBuffer) {
    LoggingScope scope;
    TempLogFile output;
    ASSERT_TRUE(output.valid());

    fiber::log::LogConfigBuilder builder;
    auto output_id = builder.add_file_appender(
            {.name = "timer_buffered", .path = output.path(), .buffer_size = 32 * 1024, .flush_interval = 20ms});
    ASSERT_TRUE(output_id);
    ASSERT_TRUE(builder.set_root_logger({.level = fiber::log::LogLevel::Info}, {*output_id}));
    auto config = builder.finish();
    ASSERT_TRUE(config);
    ASSERT_TRUE(fiber::log::LoggerManager::global().initialize(std::move(*config)));

    LOG(LOG_TEST_TIMER, INFO) << "timer-flush-message";
    EXPECT_TRUE(wait_until([&]() { return read_file(output.path()).find("timer-flush-message") != std::string::npos; },
                           2s));
}

TEST(LogSystemTest, ProducerThreadExitDoesNotOwnTheFileBuffer) {
    LoggingScope scope;
    TempLogFile output;
    ASSERT_TRUE(output.valid());

    fiber::log::LogConfigBuilder builder;
    auto output_id = builder.add_file_appender(
            {.name = "thread_buffered", .path = output.path(), .buffer_size = 32 * 1024, .flush_interval = 1s});
    ASSERT_TRUE(output_id);
    ASSERT_TRUE(builder.set_root_logger({.level = fiber::log::LogLevel::Info}, {*output_id}));
    auto config = builder.finish();
    ASSERT_TRUE(config);
    auto &manager = fiber::log::LoggerManager::global();
    ASSERT_TRUE(manager.initialize(std::move(*config)));

    std::thread producer([]() { LOG(LOG_TEST_BUFFER, INFO) << "thread-exit-message"; });
    producer.join();
    manager.flush();

    EXPECT_NE(read_file(output.path()).find("thread-exit-message"), std::string::npos);
}

TEST(LogSystemTest, ReopenIsOrderedWithQueuedRecords) {
    LoggingScope scope;
    TempLogFile output;
    ASSERT_TRUE(output.valid());

    fiber::log::LogConfigBuilder builder;
    auto output_id = builder.add_file_appender({.name = "reopen_output", .path = output.path()});
    ASSERT_TRUE(output_id);
    ASSERT_TRUE(builder.set_root_logger({.level = fiber::log::LogLevel::Info}, {*output_id}));
    auto config = builder.finish();
    ASSERT_TRUE(config);
    auto &manager = fiber::log::LoggerManager::global();
    ASSERT_TRUE(manager.initialize(std::move(*config)));

    LOG(LOG_TEST_REOPEN, INFO) << "before-reopen";
    ASSERT_TRUE(output.rotate());
    ASSERT_TRUE(manager.reopen_all());
    LOG(LOG_TEST_REOPEN, INFO) << "after-reopen";
    manager.shutdown();

    const std::string old_content = read_file(output.rotated_path());
    const std::string new_content = read_file(output.path());
    EXPECT_NE(old_content.find("before-reopen"), std::string::npos);
    EXPECT_EQ(old_content.find("after-reopen"), std::string::npos);
    EXPECT_NE(new_content.find("after-reopen"), std::string::npos);
}

TEST(LogSystemTest, RollsByRecordSizeAndRetainsNewestArchives) {
    LoggingScope scope;
    TempLogDirectory output;
    ASSERT_TRUE(output.valid());

    fiber::log::LogConfigBuilder builder;
    auto output_id = builder.add_file_appender({
            .name = "rolling_output",
            .path = output.log_path(),
            .rotation =
                    fiber::log::FileRotationOptions{
                            .max_file_size = 8192,
                            .archive_name = "{base}.{utc}.{seq}",
                            .max_archives = 2,
                    },
    });
    ASSERT_TRUE(output_id);
    ASSERT_TRUE(builder.set_root_logger({.level = fiber::log::LogLevel::Info}, {*output_id}));
    auto config = builder.finish();
    ASSERT_TRUE(config);
    auto &manager = fiber::log::LoggerManager::global();
    ASSERT_TRUE(manager.initialize(std::move(*config)));

    for (int record = 0; record < 4; ++record) {
        LOG(LOG_TEST_REOPEN, INFO) << "roll-record=[" << record << "] " << std::string(7800, 'a' + record);
    }
    manager.flush();

    const fiber::log::AppenderStats stats = manager.appender_stats(*output_id);
    EXPECT_EQ(stats.rotations, 3u);
    EXPECT_GT(stats.active_file_bytes, 0u);
    EXPECT_EQ(stats.rotation_errors, 0u);
    EXPECT_EQ(stats.retention_errors, 0u);
    manager.shutdown();

    const std::vector<std::string> files = list_output_log_files(output);
    ASSERT_EQ(files.size(), 3u);
    EXPECT_NE(std::find(files.begin(), files.end(), output.log_path()), files.end());
    EXPECT_NE(
            std::find_if(files.begin(), files.end(), [](const std::string &path) { return path.ends_with(".000002"); }),
            files.end());
    EXPECT_NE(
            std::find_if(files.begin(), files.end(), [](const std::string &path) { return path.ends_with(".000003"); }),
            files.end());

    const std::string content = read_files(files);
    EXPECT_EQ(content.find("roll-record=[0]"), std::string::npos);
    EXPECT_EQ(count_occurrences(content, "roll-record=[1]"), 1u);
    EXPECT_EQ(count_occurrences(content, "roll-record=[2]"), 1u);
    EXPECT_EQ(count_occurrences(content, "roll-record=[3]"), 1u);
}

TEST(LogSystemTest, ConcurrentProducersPreserveCompleteRecordsAndUseOneWriter) {
    LoggingScope scope;
    TempLogDirectory output;
    ASSERT_TRUE(output.valid());

    fiber::log::LogConfigBuilder builder;
    auto output_id = builder.add_file_appender({
            .name = "concurrent_rolling_output",
            .path = output.log_path(),
            .rotation =
                    fiber::log::FileRotationOptions{
                            .max_file_size = 16 * 1024,
                            .archive_name = "{base}.{seq}",
                            .max_archives = 64,
                    },
    });
    ASSERT_TRUE(output_id);
    ASSERT_TRUE(builder.set_root_logger({.level = fiber::log::LogLevel::Info}, {*output_id}));
    auto config = builder.finish();
    ASSERT_TRUE(config);
    auto &manager = fiber::log::LoggerManager::global();
    ASSERT_TRUE(manager.initialize(std::move(*config)));

    constexpr int kThreads = 4;
    constexpr int kRecordsPerThread = 50;
    std::vector<std::thread> threads;
    for (int thread = 0; thread < kThreads; ++thread) {
        threads.emplace_back([thread]() {
            for (int record = 0; record < kRecordsPerThread; ++record) {
                LOG(LOG_TEST_CONCURRENT, INFO)
                        << "[thread=" << thread << ";record=" << record << "] " << std::string(192, 'x');
            }
        });
    }
    for (auto &thread: threads) {
        thread.join();
    }
    manager.flush();
    EXPECT_GT(manager.appender_stats(*output_id).rotations, 0u);
    EXPECT_EQ(manager.appender_stats(*output_id).writer_thread_id, manager.queue_stats().writer_thread_id);
    manager.shutdown();

    const std::string content = read_files(list_output_log_files(output));
    EXPECT_EQ(std::count(content.begin(), content.end(), '\n'), kThreads * kRecordsPerThread);
    for (int thread = 0; thread < kThreads; ++thread) {
        for (int record = 0; record < kRecordsPerThread; ++record) {
            const std::string marker = "[thread=" + std::to_string(thread) + ";record=" + std::to_string(record) + "]";
            EXPECT_EQ(count_occurrences(content, marker), 1u) << marker;
        }
    }
}

TEST(LogSystemTest, MultipleThreadsWriteCompleteLongRecordsToConsolePipe) {
    LoggingScope scope;
    StderrPipeCapture capture;
    ASSERT_TRUE(capture.start());

    fiber::log::LogConfigBuilder builder;
    auto output_id = builder.add_console_appender({
            .name = "concurrent_console",
            .stream = fiber::log::ConsoleStream::Stderr,
    });
    ASSERT_TRUE(output_id);
    ASSERT_TRUE(builder.set_root_logger({.level = fiber::log::LogLevel::Info}, {*output_id}));
    auto config = builder.finish();
    ASSERT_TRUE(config);
    ASSERT_TRUE(fiber::log::LoggerManager::global().initialize(std::move(*config)));

    constexpr int kThreads = 4;
    constexpr int kRecordsPerThread = 20;
    constexpr std::size_t kPayloadSize = 6000;
    std::vector<std::thread> threads;
    for (int thread = 0; thread < kThreads; ++thread) {
        threads.emplace_back([thread]() {
            const std::string payload(kPayloadSize, static_cast<char>('a' + thread));
            for (int record = 0; record < kRecordsPerThread; ++record) {
                LOG(LOG_TEST_CONCURRENT, INFO) << "record=[" << thread << ':' << record << "] " << payload;
            }
        });
    }
    for (auto &thread: threads) {
        thread.join();
    }
    fiber::log::LoggerManager::global().shutdown();
    capture.finish();

    const std::string &output = capture.output();
    EXPECT_EQ(std::count(output.begin(), output.end(), '\n'), kThreads * kRecordsPerThread);
    for (int thread = 0; thread < kThreads; ++thread) {
        for (int record = 0; record < kRecordsPerThread; ++record) {
            const std::string marker = "record=[" + std::to_string(thread) + ':' + std::to_string(record) + "] ";
            const std::size_t marker_pos = output.find(marker);
            ASSERT_NE(marker_pos, std::string::npos) << marker;
            EXPECT_EQ(output.find(marker, marker_pos + marker.size()), std::string::npos) << marker;

            const std::size_t payload_pos = marker_pos + marker.size();
            const std::size_t line_end = output.find('\n', payload_pos);
            ASSERT_NE(line_end, std::string::npos) << marker;
            ASSERT_EQ(line_end - payload_pos, kPayloadSize) << marker;
            EXPECT_TRUE(std::all_of(output.begin() + static_cast<std::ptrdiff_t>(payload_pos),
                                    output.begin() + static_cast<std::ptrdiff_t>(line_end),
                                    [thread](char ch) { return ch == static_cast<char>('a' + thread); }))
                    << marker;
        }
    }
}
