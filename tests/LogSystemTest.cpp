#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
DEFINE_LOGGER(LOG_TEST_REOPEN, "test.reopen");
DEFINE_LOGGER(LOG_TEST_CONCURRENT, "test.concurrent");

namespace {

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

} // namespace

TEST(LogConfigTest, RejectsInvalidAppenderAndMissingRoot) {
    fiber::log::LogConfigBuilder builder;
    auto bad_name = builder.add_console_appender({.name = "bad..name"});
    ASSERT_FALSE(bad_name);
    EXPECT_EQ(bad_name.error().code, fiber::log::LogConfigErrorCode::InvalidName);

    auto finish = builder.finish();
    ASSERT_FALSE(finish);
    EXPECT_EQ(finish.error().code, fiber::log::LogConfigErrorCode::MissingRootLogger);
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
