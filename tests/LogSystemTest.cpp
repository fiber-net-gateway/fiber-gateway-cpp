#include <gtest/gtest.h>

#include <algorithm>
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
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#include "async/Sleep.h"
#include "async/Spawn.h"
#include "event/EventLoop.h"
#include "event/EventLoopGroup.h"
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
DEFINE_LOGGER(LOG_TEST_TIMER_DIRECT, "test.timer.direct");
DEFINE_LOGGER(LOG_TEST_TIMER_LONG, "test.timer.long");
DEFINE_LOGGER(LOG_TEST_TIMER_SHORT, "test.timer.short");
DEFINE_LOGGER(LOG_TEST_REOPEN, "test.reopen");
DEFINE_LOGGER(LOG_TEST_CONCURRENT, "test.concurrent");

namespace fiber::log {

class LogContextTestPeer {
public:
    [[nodiscard]] static bool timer_armed(const LogContext &context) noexcept { return context.timer_armed_; }
    [[nodiscard]] static bool timer_in_heap(const LogContext &context) noexcept {
        return context.flush_timer_.is_in_heap();
    }
    [[nodiscard]] static std::chrono::steady_clock::time_point scheduled_at(const LogContext &context) noexcept {
        return context.scheduled_at_;
    }
};

} // namespace fiber::log

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

class LoggingScope {
public:
    LoggingScope() { fiber::log::LoggerManager::global().shutdown(); }
    ~LoggingScope() { fiber::log::LoggerManager::global().shutdown(); }
};

fiber::async::DetachedTask wait_for_timer_flush(std::string path, std::promise<bool> *result) {
    LOG(LOG_TEST_TIMER, INFO) << "timer-flush-message";
    co_await fiber::async::sleep(50ms);
    result->set_value(read_file(path).find("timer-flush-message") != std::string::npos);
    fiber::event::EventLoop::current().stop();
}

struct TimerIdleResult {
    bool armed_after_append = false;
    bool in_heap_after_append = false;
    bool armed_after_flush = true;
    bool in_heap_after_flush = true;
    bool message_flushed = false;
};

fiber::async::DetachedTask inspect_timer_becomes_idle(std::string path, std::promise<TimerIdleResult> *promise) {
    LOG(LOG_TEST_TIMER, INFO) << "timer-idle-message";
    auto &context = fiber::log::LoggerManager::global().current_context();

    TimerIdleResult result{
            .armed_after_append = fiber::log::LogContextTestPeer::timer_armed(context),
            .in_heap_after_append = fiber::log::LogContextTestPeer::timer_in_heap(context),
    };
    co_await fiber::async::sleep(50ms);
    result.armed_after_flush = fiber::log::LogContextTestPeer::timer_armed(context);
    result.in_heap_after_flush = fiber::log::LogContextTestPeer::timer_in_heap(context);
    result.message_flushed = read_file(path).find("timer-idle-message") != std::string::npos;
    promise->set_value(result);
    fiber::event::EventLoop::current().stop();
}

fiber::async::DetachedTask inspect_direct_log_timer(std::promise<bool> *promise) {
    LOG(LOG_TEST_TIMER_DIRECT, INFO) << "direct-timer-message";
    auto &context = fiber::log::LoggerManager::global().current_context();
    promise->set_value(!fiber::log::LogContextTestPeer::timer_armed(context) &&
                       !fiber::log::LogContextTestPeer::timer_in_heap(context));
    fiber::event::EventLoop::current().stop();
    co_return;
}

struct EarliestTimerResult {
    bool long_timer_armed = false;
    bool short_timer_moved_earlier = false;
    bool long_deadline_restored = false;
    bool short_message_flushed = false;
    bool long_message_pending = false;
};

fiber::async::DetachedTask inspect_earliest_timer(std::string long_path, std::string short_path,
                                                  std::promise<EarliestTimerResult> *promise) {
    auto &manager = fiber::log::LoggerManager::global();

    LOG(LOG_TEST_TIMER_LONG, INFO) << "long-deadline-message";
    auto &context = manager.current_context();
    const auto long_deadline = fiber::log::LogContextTestPeer::scheduled_at(context);

    EarliestTimerResult result{
            .long_timer_armed = fiber::log::LogContextTestPeer::timer_armed(context),
    };
    LOG(LOG_TEST_TIMER_SHORT, INFO) << "short-deadline-message";
    const auto short_deadline = fiber::log::LogContextTestPeer::scheduled_at(context);
    result.short_timer_moved_earlier = short_deadline < long_deadline;

    co_await fiber::async::sleep(50ms);
    result.long_deadline_restored = fiber::log::LogContextTestPeer::timer_armed(context) &&
                                    fiber::log::LogContextTestPeer::scheduled_at(context) == long_deadline;
    result.short_message_flushed = read_file(short_path).find("short-deadline-message") != std::string::npos;
    result.long_message_pending = read_file(long_path).empty();
    promise->set_value(result);
    fiber::event::EventLoop::current().stop();
}

fiber::async::DetachedTask inspect_explicit_flush_cancels_timer(std::promise<bool> *promise) {
    auto &manager = fiber::log::LoggerManager::global();
    LOG(LOG_TEST_TIMER, INFO) << "explicit-flush-message";
    auto &context = manager.current_context();
    const bool armed_before = fiber::log::LogContextTestPeer::timer_armed(context);
    manager.flush_current_thread();
    promise->set_value(armed_before && !fiber::log::LogContextTestPeer::timer_armed(context) &&
                       !fiber::log::LogContextTestPeer::timer_in_heap(context));
    fiber::event::EventLoop::current().stop();
    co_return;
}

} // namespace

TEST(LogConfigTest, RejectsInvalidAppenderAndMissingRoot) {
    fiber::log::LogConfigBuilder builder;
    auto bad_name = builder.add_console_appender({.name = "bad..name"});
    ASSERT_FALSE(bad_name);
    EXPECT_EQ(bad_name.error().code, fiber::log::LogConfigErrorCode::InvalidName);

    auto finish = builder.finish();
    ASSERT_FALSE(finish);
    EXPECT_EQ(finish.error().code, fiber::log::LogConfigErrorCode::MissingRootLogger);

    fiber::log::LogConfigBuilder rotation_builder;
    auto bad_rotation = rotation_builder.add_file_appender({
            .name = "bad_rotation",
            .path = "/tmp/bad_rotation.log",
            .rotation =
                    fiber::log::FileRotationOptions{
                            .max_file_size = fiber::log::kMaxFormattedLogLineSize,
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

    auto gateway_rule = builder.add_logger(
            {.name = "test.gateway", .level = fiber::log::LogLevel::Debug, .additive = true}, {*gateway_id, *all_id});
    auto auth_rule = builder.add_logger(
            {.name = "test.gateway.auth", .level = fiber::log::LogLevel::Info, .additive = true}, {*auth_id});
    auto isolated_rule = builder.add_logger(
            {.name = "test.isolated", .level = fiber::log::LogLevel::Debug, .additive = false}, {*auth_id});
    auto root = builder.set_root_logger({.level = fiber::log::LogLevel::Info}, {*all_id, *error_id});
    ASSERT_TRUE(gateway_rule);
    ASSERT_TRUE(auth_rule);
    ASSERT_TRUE(isolated_rule);
    ASSERT_TRUE(root);
    auto config = builder.finish();
    ASSERT_TRUE(config);
    auto initialized = fiber::log::LoggerManager::global().initialize(std::move(*config));
    ASSERT_TRUE(initialized) << initialized.error().message;

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
    EXPECT_EQ(LOG_TEST_OTHER.get().targets(fiber::log::LogLevel::Debug).count, 0u);
    EXPECT_EQ(LOG_TEST_OTHER.get().targets(fiber::log::LogLevel::Info).count, 1u);

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
    EXPECT_EQ(auth_content.find("http-debug"), std::string::npos);
    EXPECT_NE(auth_content.find("isolated-info"), std::string::npos);

    EXPECT_NE(gateway_content.find("auth-info"), std::string::npos);
    EXPECT_NE(gateway_content.find("http-debug"), std::string::npos);
    EXPECT_NE(gateway_content.find("auth-error"), std::string::npos);
    EXPECT_EQ(gateway_content.find("isolated-info"), std::string::npos);

    EXPECT_NE(all_content.find("auth-info"), std::string::npos);
    EXPECT_NE(all_content.find("http-debug"), std::string::npos);
    EXPECT_NE(all_content.find("other-info"), std::string::npos);
    EXPECT_NE(all_content.find("auth-error"), std::string::npos);
    EXPECT_EQ(all_content.find("isolated-info"), std::string::npos);
    EXPECT_EQ(count_occurrences(all_content, "auth-info"), 1u);

    EXPECT_EQ(error_content.find("auth-info"), std::string::npos);
    EXPECT_NE(error_content.find("auth-error"), std::string::npos);
    EXPECT_EQ(error_content.find("isolated-info"), std::string::npos);

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
    auto macro_rule = builder.add_logger(
            {.name = "test.macro", .level = fiber::log::LogLevel::Debug, .verbosity = 2, .additive = true},
            std::initializer_list<fiber::log::AppenderId>{});
    auto root = builder.set_root_logger({.level = fiber::log::LogLevel::Warn}, {*output_id});
    ASSERT_TRUE(macro_rule);
    ASSERT_TRUE(root);
    auto config = builder.finish();
    ASSERT_TRUE(config);
    auto initialized = fiber::log::LoggerManager::global().initialize(std::move(*config));
    ASSERT_TRUE(initialized) << initialized.error().message;

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

TEST(LogSystemTest, EscapesControlCharactersAndTruncatesLongMessages) {
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

    LOG(LOG_TEST_OTHER, INFO) << "alpha\nbeta\r\t";
    LOG(LOG_TEST_OTHER, INFO) << fiber::log::quoted("alpha\"beta\\gamma\n");
    LOG(LOG_TEST_OTHER, INFO) << std::string(9000, 'x');
    fiber::log::LoggerManager::global().shutdown();

    const std::string content = read_file(output.path());
    EXPECT_NE(content.find("alpha\\nbeta\\r\\t"), std::string::npos);
    EXPECT_NE(content.find("\"alpha\\\"beta\\\\gamma\\n\""), std::string::npos);
    EXPECT_NE(content.find("<truncated>"), std::string::npos);
    EXPECT_EQ(std::count(content.begin(), content.end(), '\n'), 3);
}

TEST(LogSystemTest, ReusesOneFormattedRecordAcrossDirectAndBufferedAppenders) {
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
    ASSERT_TRUE(fiber::log::LoggerManager::global().initialize(std::move(*config)));

    LOG(LOG_TEST_BUFFER, INFO) << "shared-record-one";
    LOG(LOG_TEST_BUFFER, INFO) << "shared-record-two";

    EXPECT_EQ(fiber::log::LoggerManager::global().appender_stats(*direct_id).written_records, 2u);
    EXPECT_EQ(fiber::log::LoggerManager::global().appender_stats(*buffered_id).written_records, 0u);
    fiber::log::LoggerManager::global().flush_current_thread();
    EXPECT_EQ(fiber::log::LoggerManager::global().appender_stats(*buffered_id).written_records, 2u);

    fiber::log::LoggerManager::global().shutdown();
    const std::string direct_content = read_file(direct_output.path());
    const std::string buffered_content = read_file(buffered_output.path());
    EXPECT_FALSE(direct_content.empty());
    EXPECT_EQ(direct_content, buffered_content);
    EXPECT_EQ(std::count(direct_content.begin(), direct_content.end(), '\n'), 2);
}

TEST(LogSystemTest, BufferedAppenderFlushesCurrentThread) {
    LoggingScope scope;
    TempLogFile output;
    ASSERT_TRUE(output.valid());

    fiber::log::LogConfigBuilder builder;
    auto output_id = builder.add_file_appender(
            {.name = "buffered", .path = output.path(), .buffer_size = 32 * 1024, .flush_interval = 1s});
    ASSERT_TRUE(output_id);
    ASSERT_TRUE(builder.set_root_logger({.level = fiber::log::LogLevel::Info}, {*output_id}));
    auto config = builder.finish();
    ASSERT_TRUE(config);
    ASSERT_TRUE(fiber::log::LoggerManager::global().initialize(std::move(*config)));

    LOG(LOG_TEST_BUFFER, INFO) << "buffered-message";
    EXPECT_TRUE(read_file(output.path()).empty());
    EXPECT_EQ(fiber::log::LoggerManager::global().appender_stats(*output_id).written_records, 0u);

    fiber::log::LoggerManager::global().flush_current_thread();
    EXPECT_NE(read_file(output.path()).find("buffered-message"), std::string::npos);
    EXPECT_EQ(fiber::log::LoggerManager::global().appender_stats(*output_id).written_records, 1u);
}

TEST(LogSystemTest, EventLoopTimerFlushesLowTrafficBuffer) {
    LoggingScope scope;
    TempLogFile output;
    ASSERT_TRUE(output.valid());

    fiber::log::LogConfigBuilder builder;
    auto output_id = builder.add_file_appender(
            {.name = "timer_buffered", .path = output.path(), .buffer_size = 32 * 1024, .flush_interval = 10ms});
    ASSERT_TRUE(output_id);
    ASSERT_TRUE(builder.set_root_logger({.level = fiber::log::LogLevel::Info}, {*output_id}));
    auto config = builder.finish();
    ASSERT_TRUE(config);
    ASSERT_TRUE(fiber::log::LoggerManager::global().initialize(std::move(*config)));

    fiber::event::EventLoopGroup group(1);
    group.start();
    std::promise<bool> flushed;
    auto future = flushed.get_future();
    fiber::async::spawn(group.at(0), [&]() { return wait_for_timer_flush(output.path(), &flushed); });

    if (future.wait_for(2s) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "timed out waiting for the EventLoop log flush timer";
        return;
    }
    EXPECT_TRUE(future.get());
    group.join();
}

TEST(LogSystemTest, EventLoopFlushTimerStopsWhenBuffersBecomeIdle) {
    LoggingScope scope;
    TempLogFile output;
    ASSERT_TRUE(output.valid());

    fiber::log::LogConfigBuilder builder;
    auto output_id = builder.add_file_appender(
            {.name = "idle_timer_buffered", .path = output.path(), .buffer_size = 32 * 1024, .flush_interval = 10ms});
    ASSERT_TRUE(output_id);
    ASSERT_TRUE(builder.set_root_logger({.level = fiber::log::LogLevel::Info}, {*output_id}));
    auto config = builder.finish();
    ASSERT_TRUE(config);
    ASSERT_TRUE(fiber::log::LoggerManager::global().initialize(std::move(*config)));

    fiber::event::EventLoopGroup group(1);
    group.start();
    std::promise<TimerIdleResult> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return inspect_timer_becomes_idle(output.path(), &promise); });

    if (future.wait_for(2s) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "timed out inspecting the idle flush timer";
        return;
    }
    const TimerIdleResult result = future.get();
    EXPECT_TRUE(result.armed_after_append);
    EXPECT_TRUE(result.in_heap_after_append);
    EXPECT_FALSE(result.armed_after_flush);
    EXPECT_FALSE(result.in_heap_after_flush);
    EXPECT_TRUE(result.message_flushed);
    group.join();
}

TEST(LogSystemTest, DirectLogDoesNotArmTimerWhenBufferedAppenderExists) {
    LoggingScope scope;
    TempLogFile direct_output;
    TempLogFile unused_buffered_output;
    ASSERT_TRUE(direct_output.valid());
    ASSERT_TRUE(unused_buffered_output.valid());

    fiber::log::LogConfigBuilder builder;
    auto direct_id = builder.add_file_appender({.name = "timer_direct", .path = direct_output.path()});
    auto buffered_id = builder.add_file_appender({
            .name = "timer_unused_buffered",
            .path = unused_buffered_output.path(),
            .buffer_size = 32 * 1024,
            .flush_interval = 10ms,
    });
    ASSERT_TRUE(direct_id);
    ASSERT_TRUE(buffered_id);
    ASSERT_TRUE(builder.set_root_logger({.level = fiber::log::LogLevel::Info}, {*direct_id}));
    auto config = builder.finish();
    ASSERT_TRUE(config);
    ASSERT_TRUE(fiber::log::LoggerManager::global().initialize(std::move(*config)));

    fiber::event::EventLoopGroup group(1);
    group.start();
    std::promise<bool> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return inspect_direct_log_timer(&promise); });

    if (future.wait_for(2s) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "timed out inspecting the direct log timer state";
        return;
    }
    EXPECT_TRUE(future.get());
    group.join();
}

TEST(LogSystemTest, FlushTimerTracksExactEarliestDeadline) {
    LoggingScope scope;
    TempLogFile long_output;
    TempLogFile short_output;
    ASSERT_TRUE(long_output.valid());
    ASSERT_TRUE(short_output.valid());

    fiber::log::LogConfigBuilder builder;
    auto long_id = builder.add_file_appender({
            .name = "timer_long",
            .path = long_output.path(),
            .buffer_size = 32 * 1024,
            .flush_interval = 250ms,
    });
    auto short_id = builder.add_file_appender({
            .name = "timer_short",
            .path = short_output.path(),
            .buffer_size = 32 * 1024,
            .flush_interval = 10ms,
    });
    ASSERT_TRUE(long_id);
    ASSERT_TRUE(short_id);
    ASSERT_TRUE(builder.add_logger({.name = "test.timer.long", .additive = false}, {*long_id}));
    ASSERT_TRUE(builder.add_logger({.name = "test.timer.short", .additive = false}, {*short_id}));
    ASSERT_TRUE(builder.set_root_logger({.level = fiber::log::LogLevel::Info}, {}));
    auto config = builder.finish();
    ASSERT_TRUE(config);
    ASSERT_TRUE(fiber::log::LoggerManager::global().initialize(std::move(*config)));

    fiber::event::EventLoopGroup group(1);
    group.start();
    std::promise<EarliestTimerResult> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return inspect_earliest_timer(long_output.path(), short_output.path(), &promise); });

    if (future.wait_for(2s) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "timed out inspecting the earliest flush timer";
        return;
    }
    const EarliestTimerResult result = future.get();
    EXPECT_TRUE(result.long_timer_armed);
    EXPECT_TRUE(result.short_timer_moved_earlier);
    EXPECT_TRUE(result.long_deadline_restored);
    EXPECT_TRUE(result.short_message_flushed);
    EXPECT_TRUE(result.long_message_pending);
    group.join();
}

TEST(LogSystemTest, ExplicitFlushCancelsPendingTimer) {
    LoggingScope scope;
    TempLogFile output;
    ASSERT_TRUE(output.valid());

    fiber::log::LogConfigBuilder builder;
    auto output_id = builder.add_file_appender({
            .name = "explicit_flush_buffered",
            .path = output.path(),
            .buffer_size = 32 * 1024,
            .flush_interval = 1s,
    });
    ASSERT_TRUE(output_id);
    ASSERT_TRUE(builder.set_root_logger({.level = fiber::log::LogLevel::Info}, {*output_id}));
    auto config = builder.finish();
    ASSERT_TRUE(config);
    ASSERT_TRUE(fiber::log::LoggerManager::global().initialize(std::move(*config)));

    fiber::event::EventLoopGroup group(1);
    group.start();
    std::promise<bool> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return inspect_explicit_flush_cancels_timer(&promise); });

    if (future.wait_for(2s) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "timed out inspecting explicit flush timer cancellation";
        return;
    }
    EXPECT_TRUE(future.get());
    group.join();
    EXPECT_NE(read_file(output.path()).find("explicit-flush-message"), std::string::npos);
}

TEST(LogSystemTest, ThreadExitFlushesItsLocalBuffer) {
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
    ASSERT_TRUE(fiber::log::LoggerManager::global().initialize(std::move(*config)));

    std::thread producer([]() { LOG(LOG_TEST_BUFFER, INFO) << "thread-exit-message"; });
    producer.join();

    EXPECT_NE(read_file(output.path()).find("thread-exit-message"), std::string::npos);
}

TEST(LogSystemTest, ReopenSwitchesStableFileDescriptorToNewFile) {
    LoggingScope scope;
    TempLogFile output;
    ASSERT_TRUE(output.valid());

    fiber::log::LogConfigBuilder builder;
    auto output_id = builder.add_file_appender({.name = "reopen_output", .path = output.path()});
    ASSERT_TRUE(output_id);
    ASSERT_TRUE(builder.set_root_logger({.level = fiber::log::LogLevel::Info}, {*output_id}));
    auto config = builder.finish();
    ASSERT_TRUE(config);
    ASSERT_TRUE(fiber::log::LoggerManager::global().initialize(std::move(*config)));

    LOG(LOG_TEST_REOPEN, INFO) << "before-reopen";
    ASSERT_TRUE(output.rotate());
    ASSERT_TRUE(fiber::log::LoggerManager::global().reopen_all());
    LOG(LOG_TEST_REOPEN, INFO) << "after-reopen";
    fiber::log::LoggerManager::global().shutdown();

    const std::string old_content = read_file(output.rotated_path());
    const std::string new_content = read_file(output.path());
    EXPECT_NE(old_content.find("before-reopen"), std::string::npos);
    EXPECT_EQ(old_content.find("after-reopen"), std::string::npos);
    EXPECT_NE(new_content.find("after-reopen"), std::string::npos);
}

TEST(LogSystemTest, RollsBySizeAndRetainsNewestArchives) {
    LoggingScope scope;
    TempLogDirectory output;
    ASSERT_TRUE(output.valid());

    fiber::log::LogConfigBuilder builder;
    auto output_id = builder.add_file_appender({
            .name = "rolling_output",
            .path = output.log_path(),
            .rotation =
                    fiber::log::FileRotationOptions{
                            .max_file_size = fiber::log::kMaxFormattedLogLineSize,
                            .archive_name = "{base}.{utc}.{seq}",
                            .max_archives = 2,
                    },
    });
    ASSERT_TRUE(output_id);
    ASSERT_TRUE(builder.set_root_logger({.level = fiber::log::LogLevel::Info}, {*output_id}));
    auto config = builder.finish();
    ASSERT_TRUE(config);
    ASSERT_TRUE(fiber::log::LoggerManager::global().initialize(std::move(*config)));

    for (int record = 0; record < 4; ++record) {
        LOG(LOG_TEST_REOPEN, INFO) << "roll-record=[" << record << "] " << std::string(7800, 'a' + record);
    }

    const fiber::log::AppenderStats stats = fiber::log::LoggerManager::global().appender_stats(*output_id);
    EXPECT_EQ(stats.rotations, 3u);
    EXPECT_GT(stats.active_file_bytes, 0u);
    EXPECT_EQ(stats.rotation_errors, 0u);
    EXPECT_EQ(stats.retention_errors, 0u);
    fiber::log::LoggerManager::global().shutdown();

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

TEST(LogSystemTest, ConcurrentRotationPreservesCompleteRecords) {
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
    ASSERT_TRUE(fiber::log::LoggerManager::global().initialize(std::move(*config)));

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
    EXPECT_GT(fiber::log::LoggerManager::global().appender_stats(*output_id).rotations, 0u);
    fiber::log::LoggerManager::global().shutdown();

    const std::string content = read_files(list_output_log_files(output));
    EXPECT_EQ(std::count(content.begin(), content.end(), '\n'), kThreads * kRecordsPerThread);
    for (int thread = 0; thread < kThreads; ++thread) {
        for (int record = 0; record < kRecordsPerThread; ++record) {
            const std::string marker = "[thread=" + std::to_string(thread) + ";record=" + std::to_string(record) + "]";
            EXPECT_EQ(count_occurrences(content, marker), 1u) << marker;
        }
    }
}

TEST(LogSystemTest, MultipleThreadsAppendCompleteRecordsToSharedFile) {
    LoggingScope scope;
    TempLogFile output;
    ASSERT_TRUE(output.valid());

    fiber::log::LogConfigBuilder builder;
    auto output_id = builder.add_file_appender({.name = "concurrent_output", .path = output.path()});
    ASSERT_TRUE(output_id);
    ASSERT_TRUE(builder.set_root_logger({.level = fiber::log::LogLevel::Info}, {*output_id}));
    auto config = builder.finish();
    ASSERT_TRUE(config);
    ASSERT_TRUE(fiber::log::LoggerManager::global().initialize(std::move(*config)));

    constexpr int kThreads = 4;
    constexpr int kRecordsPerThread = 50;
    std::vector<std::thread> threads;
    for (int thread = 0; thread < kThreads; ++thread) {
        threads.emplace_back([thread]() {
            for (int record = 0; record < kRecordsPerThread; ++record) {
                LOG(LOG_TEST_CONCURRENT, INFO) << "thread=" << thread << " record=" << record;
            }
        });
    }
    for (auto &thread: threads) {
        thread.join();
    }
    fiber::log::LoggerManager::global().shutdown();

    const std::string content = read_file(output.path());
    EXPECT_EQ(std::count(content.begin(), content.end(), '\n'), kThreads * kRecordsPerThread);
}
