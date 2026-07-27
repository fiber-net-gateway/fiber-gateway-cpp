#include "Logger.h"

#include <array>
#include <limits>
#include <memory>
#include <new>

#include "Appender.h"
#include "LogContext.h"
#include "LogFormatter.h"

namespace fiber::log {
namespace {

constinit LoggerHandle *g_logger_registry_head = nullptr;
constinit bool g_logger_registry_sealed = false;
constinit bool g_late_logger_registration = false;

void append_to_targets(const LevelTargets &targets, FormattedLogLine line, LogContext &context) noexcept {
    for (std::uint32_t i = 0; i < targets.count; ++i) {
        targets.first[i]->append(line, context);
    }
}

#if defined(__GNUC__) || defined(__clang__)
[[gnu::noinline]]
#endif
void dispatch_with_stack_scratch(const LogEvent &event, const LevelTargets &targets, LogContext &context) noexcept {
    std::array<char, kMaxFormattedLogLineSize> scratch;
    const std::size_t size = detail::format_log_event(event, scratch.data(), scratch.size());
    append_to_targets(targets, FormattedLogLine{.bytes = std::string_view(scratch.data(), size)}, context);
}

} // namespace

void Logger::dispatch(const LogEvent &event, LogContext &context) const noexcept {
    if (!valid_log_level(event.level)) {
        return;
    }
    const LevelTargets &targets = levels_[level_index(event.level)];
    if (targets.empty()) {
        return;
    }

    auto scratch = context.acquire_format_scratch();
    if (!scratch) {
        dispatch_with_stack_scratch(event, targets, context);
        return;
    }

    const std::size_t size = detail::format_log_event(event, scratch.data(), kMaxFormattedLogLineSize);
    append_to_targets(targets, FormattedLogLine{.bytes = std::string_view(scratch.data(), size)}, context);
}

bool Logger::dispatch_complete(const LogEvent &event, LogContext &context) const noexcept {
    if (!valid_log_level(event.level)) {
        return false;
    }
    const LevelTargets &targets = levels_[level_index(event.level)];
    if (targets.empty()) {
        return true;
    }
    if (event.message.size() > kMaxCompleteLogMessageSize ||
        event.message.size() > std::numeric_limits<std::size_t>::max() - kMaxFormattedLogLineSize) {
        return false;
    }

    const std::size_t capacity = event.message.size() + kMaxFormattedLogLineSize;
    std::unique_ptr<char[]> scratch(new (std::nothrow) char[capacity]);
    if (!scratch) {
        return false;
    }
    const std::size_t size = detail::format_log_event(event, scratch.get(), capacity);
    append_to_targets(targets, FormattedLogLine{.bytes = std::string_view(scratch.get(), size)}, context);
    return true;
}

const Logger &bootstrap_logger() noexcept {
    static ConsoleAppender appender(0, ConsoleAppenderOptions{.name = "bootstrap", .stream = ConsoleStream::Stderr});
    static Appender *targets[] = {&appender};
    static Logger logger("bootstrap");
    static const bool initialized = [&]() noexcept {
        for (auto &level: logger.levels_) {
            level = LevelTargets{.first = targets, .count = 1};
        }
        return true;
    }();
    (void) initialized;
    return logger;
}

LoggerHandle::LoggerHandle(std::string_view name) noexcept : name_(name), logger_(&bootstrap_logger()) {
    if (g_logger_registry_sealed) {
        g_late_logger_registration = true;
        return;
    }
    registry_next_ = g_logger_registry_head;
    g_logger_registry_head = this;
}

namespace detail {

LoggerHandle *logger_registry_head() noexcept { return g_logger_registry_head; }

void seal_logger_registry() noexcept { g_logger_registry_sealed = true; }

bool late_logger_registration() noexcept { return g_late_logger_registration; }

} // namespace detail

} // namespace fiber::log
