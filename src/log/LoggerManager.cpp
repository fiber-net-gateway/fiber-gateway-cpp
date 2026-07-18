#include "LoggerManager.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

#include "../common/Assert.h"
#include "../common/mem/BufPool.h"
#include "Logger.h"

namespace fiber::log {
namespace {

constinit LoggerManager *g_logger_manager = nullptr;

LogConfigError make_error(LogConfigErrorCode code, std::string message, int system_error = 0) {
    return LogConfigError{.code = code, .message = std::move(message), .system_error = system_error};
}

bool logger_rule_matches(std::string_view rule, std::string_view logger) noexcept {
    if (rule == logger) {
        return true;
    }
    return logger.size() > rule.size() && logger.starts_with(rule) && logger[rule.size()] == '.';
}

LogContext &thread_log_context() noexcept {
    static thread_local LogContext context;
    return context;
}

} // namespace

struct LoggerManager::Runtime {
    explicit Runtime(std::uint64_t value) : generation(value) {}

    struct NamedLogger {
        std::string_view name;
        const Logger *logger = nullptr;
    };

    std::vector<std::unique_ptr<Appender>> appenders;
    std::vector<LoggerHandle *> handles;
    std::vector<NamedLogger> named_loggers;
    mem::BufPool logger_arena{4096};
    std::uint64_t generation = 0;
    std::uint16_t buffer_count = 0;
};

LoggerManager::LoggerManager() noexcept { g_logger_manager = this; }

LoggerManager::~LoggerManager() {
    shutdown();
    g_logger_manager = nullptr;
}

LoggerManager &LoggerManager::global() noexcept {
    static LoggerManager manager;
    return manager;
}

LoggerManager *LoggerManager::try_global() noexcept { return g_logger_manager; }

LogConfigResult<void> LoggerManager::initialize(LogConfig config) {
    if (runtime_) {
        return std::unexpected(make_error(LogConfigErrorCode::AlreadyInitialized, "logger manager is already running"));
    }
    if (!config.has_root_) {
        return std::unexpected(make_error(LogConfigErrorCode::MissingRootLogger, "root logger is not configured"));
    }

    detail::seal_logger_registry();
    if (detail::late_logger_registration()) {
        return std::unexpected(
                make_error(LogConfigErrorCode::LateLoggerRegistration, "logger registered after registry was sealed"));
    }

    auto candidate = std::unique_ptr<Runtime>(new (std::nothrow) Runtime(next_generation_));
    if (!candidate) {
        return std::unexpected(make_error(LogConfigErrorCode::OutOfMemory, "failed to allocate logging runtime"));
    }
    candidate->appenders.reserve(config.appenders_.size());

    for (std::size_t index = 0; index < config.appenders_.size(); ++index) {
        auto &definition = config.appenders_[index];
        const auto id = static_cast<AppenderId>(index);
        if (definition.type == LogConfig::AppenderDefinition::Type::Console) {
            auto *raw = new (std::nothrow) ConsoleAppender(id, std::move(definition.console));
            if (!raw) {
                return std::unexpected(
                        make_error(LogConfigErrorCode::OutOfMemory, "failed to allocate console appender"));
            }
            candidate->appenders.emplace_back(raw);
            continue;
        }

        std::uint16_t buffer_slot = FileAppender::kNoBufferSlot;
        if (definition.file.buffer_size > 0) {
            if (candidate->buffer_count == FileAppender::kNoBufferSlot) {
                return std::unexpected(make_error(LogConfigErrorCode::SizeOverflow, "too many buffered appenders"));
            }
            buffer_slot = candidate->buffer_count++;
        }
        auto *raw = new (std::nothrow) FileAppender(id, std::move(definition.file), buffer_slot);
        if (!raw) {
            return std::unexpected(make_error(LogConfigErrorCode::OutOfMemory, "failed to allocate file appender"));
        }
        std::unique_ptr<FileAppender> appender(raw);
        int system_error = 0;
        if (!appender->open_file(system_error)) {
            return std::unexpected(make_error(LogConfigErrorCode::OpenFailed, "failed to open log file", system_error));
        }
        candidate->appenders.push_back(std::move(appender));
    }

    std::vector<LoggerHandle *> handles;
    for (LoggerHandle *handle = detail::logger_registry_head(); handle; handle = handle->registry_next_) {
        if (!valid_logger_name(handle->name_)) {
            return std::unexpected(make_error(LogConfigErrorCode::InvalidName, "invalid registered logger name"));
        }
        handles.push_back(handle);
    }
    std::sort(handles.begin(), handles.end(),
              [](const LoggerHandle *lhs, const LoggerHandle *rhs) { return lhs->name() < rhs->name(); });

    std::vector<std::string_view> logger_names;
    logger_names.reserve(handles.size() + config.requested_loggers_.size());
    for (const LoggerHandle *handle: handles) {
        logger_names.push_back(handle->name());
    }
    for (const std::string &name: config.requested_loggers_) {
        logger_names.push_back(name);
    }
    std::sort(logger_names.begin(), logger_names.end());
    logger_names.erase(std::unique(logger_names.begin(), logger_names.end()), logger_names.end());
    candidate->named_loggers.reserve(logger_names.size());

    for (std::string_view logger_name: logger_names) {

        std::vector<const LogConfig::LoggerDefinition *> matching_rules;
        for (const auto &rule: config.loggers_) {
            if (logger_rule_matches(rule.options.name, logger_name)) {
                matching_rules.push_back(&rule);
            }
        }
        std::sort(matching_rules.begin(), matching_rules.end(),
                  [](const auto *lhs, const auto *rhs) { return lhs->options.name.size() > rhs->options.name.size(); });

        LogLevel effective_level = config.root_.level;
        unsigned effective_verbosity = config.root_.verbosity;
        for (const auto *rule: matching_rules) {
            if (rule->options.level) {
                effective_level = *rule->options.level;
                break;
            }
        }
        for (const auto *rule: matching_rules) {
            if (rule->options.verbosity) {
                effective_verbosity = *rule->options.verbosity;
                break;
            }
        }

        std::array<std::vector<Appender *>, kLogLevelCount> resolved;
        std::size_t total_count = 0;
        for (std::size_t level_pos = 0; level_pos < kLogLevelCount; ++level_pos) {
            const auto level = static_cast<LogLevel>(level_pos);
            if (level_pos < level_index(effective_level)) {
                continue;
            }
            auto &targets = resolved[level_pos];
            auto append_unique = [&](AppenderId id) {
                Appender *appender = candidate->appenders[id].get();
                if (!appender->accepts(level)) {
                    return;
                }
                if (std::find(targets.begin(), targets.end(), appender) == targets.end()) {
                    targets.push_back(appender);
                }
            };

            bool propagate = true;
            for (const auto *rule: matching_rules) {
                for (AppenderId id: rule->appenders) {
                    append_unique(id);
                }
                if (!rule->options.additive) {
                    propagate = false;
                    break;
                }
            }
            if (propagate) {
                for (AppenderId id: config.root_appenders_) {
                    append_unique(id);
                }
            }
            if (targets.size() > std::numeric_limits<std::uint32_t>::max()) {
                return std::unexpected(
                        make_error(LogConfigErrorCode::SizeOverflow, "logger level has too many appenders"));
            }
            if (total_count > std::numeric_limits<std::size_t>::max() - targets.size()) {
                return std::unexpected(make_error(LogConfigErrorCode::SizeOverflow, "logger target count overflow"));
            }
            total_count += targets.size();
        }

        constexpr std::size_t pointer_align = alignof(Appender *);
        if (sizeof(Logger) > std::numeric_limits<std::size_t>::max() - (pointer_align - 1)) {
            return std::unexpected(make_error(LogConfigErrorCode::SizeOverflow, "logger size overflow"));
        }
        const std::size_t logger_size = (sizeof(Logger) + pointer_align - 1) & ~(pointer_align - 1);
        if (total_count > (std::numeric_limits<std::size_t>::max() - logger_size) / sizeof(Appender *)) {
            return std::unexpected(make_error(LogConfigErrorCode::SizeOverflow, "logger allocation size overflow"));
        }
        const std::size_t allocation_size = logger_size + total_count * sizeof(Appender *);
        void *memory = candidate->logger_arena.alloc(allocation_size,
                                                     std::max<std::size_t>(alignof(Logger), alignof(Appender *)));
        if (!memory) {
            return std::unexpected(make_error(LogConfigErrorCode::OutOfMemory, "failed to allocate logger"));
        }

        void *name_memory = candidate->logger_arena.alloc(logger_name.size(), alignof(char));
        if (!name_memory) {
            return std::unexpected(make_error(LogConfigErrorCode::OutOfMemory, "failed to allocate logger name"));
        }
        std::memcpy(name_memory, logger_name.data(), logger_name.size());
        const std::string_view owned_name(static_cast<const char *>(name_memory), logger_name.size());

        auto *logger = new (memory) Logger(owned_name);
        logger->verbosity_ = effective_verbosity;
        auto **storage = reinterpret_cast<Appender **>(static_cast<char *>(memory) + logger_size);
        Appender **current = storage;
        for (std::size_t level_pos = 0; level_pos < kLogLevelCount; ++level_pos) {
            const auto &targets = resolved[level_pos];
            if (targets.empty()) {
                continue;
            }
            logger->levels_[level_pos] = {
                    .first = current,
                    .count = static_cast<std::uint32_t>(targets.size()),
            };
            for (Appender *appender: targets) {
                *current++ = appender;
            }
        }
        candidate->named_loggers.push_back({.name = owned_name, .logger = logger});
    }

    candidate->handles = handles;
    runtime_ = std::move(candidate);
    ++next_generation_;
    for (LoggerHandle *handle: handles) {
        const Logger *logger = find_logger(handle->name());
        FIBER_ASSERT(logger != nullptr);
        handle->logger_ = logger;
    }
    return {};
}

void LoggerManager::shutdown() noexcept {
    if (!runtime_) {
        return;
    }
    LogContext &context = thread_log_context();
    flush_context(context);
    for (LoggerHandle *handle: runtime_->handles) {
        handle->logger_ = &bootstrap_logger();
    }
    context.reset();
    runtime_.reset();
}

bool LoggerManager::running() const noexcept { return runtime_ != nullptr; }

const Logger *LoggerManager::find_logger(std::string_view name) const noexcept {
    if (!runtime_) {
        return nullptr;
    }
    const auto &loggers = runtime_->named_loggers;
    auto it =
            std::lower_bound(loggers.begin(), loggers.end(), name,
                             [](const Runtime::NamedLogger &entry, std::string_view key) { return entry.name < key; });
    return it != loggers.end() && it->name == name ? it->logger : nullptr;
}

bool LoggerManager::reopen_all() noexcept {
    if (!runtime_) {
        return false;
    }
    bool success = true;
    for (auto &appender: runtime_->appenders) {
        if (!appender->reopen()) {
            success = false;
        }
    }
    return success;
}

void LoggerManager::flush_current_thread() noexcept {
    if (!runtime_) {
        return;
    }
    flush_context(thread_log_context());
}

LogContext &LoggerManager::current_context() noexcept {
    LogContext &context = thread_log_context();
    if (!runtime_) {
        (void) context.prepare(0, 0);
        return context;
    }
    (void) context.prepare(runtime_->generation, runtime_->buffer_count);
    return context;
}

void LoggerManager::flush_context(LogContext &context) noexcept {
    if (!runtime_ || context.generation_ != runtime_->generation) {
        return;
    }
    for (auto &appender: runtime_->appenders) {
        appender->flush(context);
    }
    context.cancel_flush_schedule();
}

void LoggerManager::destroy_context(LogContext &context) noexcept {
    flush_context(context);
    context.reset();
}

void LoggerManager::on_context_timer(LogContext &context) noexcept {
    if (!runtime_ || context.generation_ != runtime_->generation || !context.loop_) {
        context.cancel_flush_schedule();
        return;
    }
    FIBER_ASSERT(context.loop_->in_loop());
    const auto now = event::EventLoop::current().now();
    for (std::uint16_t i = 0; i < context.buffer_count_; ++i) {
        LogBuffer &buffer = context.buffers_[i];
        if (buffer.size == 0 || now < buffer.flush_at) {
            continue;
        }
        FIBER_ASSERT(buffer.owner != nullptr);
        buffer.owner->flush_buffer(buffer);
    }
    context.rebuild_flush_schedule();
}

AppenderStats LoggerManager::appender_stats(AppenderId id) const noexcept {
    if (!runtime_ || id >= runtime_->appenders.size()) {
        return {};
    }
    return runtime_->appenders[id]->stats();
}

} // namespace fiber::log
