#include <fiber/log/Logger.h>

namespace fiber::log {
namespace {

constinit LoggerHandle *g_logger_registry_head = nullptr;
constinit bool g_logger_registry_sealed = false;
constinit bool g_late_logger_registration = false;

} // namespace

const Logger &bootstrap_logger() noexcept {
    static constexpr AppenderId targets[] = {kInvalidAppenderId};
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
