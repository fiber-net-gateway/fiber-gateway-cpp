#include "AiServerLogging.h"
#include "observability/AiServerLogCategories.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fiber/common/json/JsonParse.h>
#include <fiber/common/json/JsonParser.h>
#include <fiber/common/json/JsonValue.h>
#include <fiber/common/mem/BufPool.h>

namespace fiber::ai_server {
namespace {

using json::JsonAny;
using json::JsonArray;
using json::JsonObject;

constexpr std::size_t kMaxLoggingConfigBytes = 1024 * 1024;
constexpr std::string_view kAuditAppenderName = "ai_server_audit_file";

using AppenderMap = std::map<std::string, log::AppenderId, std::less<>>;
using StringSet = std::set<std::string, std::less<>>;

class FileHandle {
public:
    explicit FileHandle(int fd) noexcept : fd_(fd) {}
    ~FileHandle() {
        if (fd_ >= 0) {
            (void) ::close(fd_);
        }
    }

    FileHandle(const FileHandle &) = delete;
    FileHandle &operator=(const FileHandle &) = delete;

    [[nodiscard]] int get() const noexcept { return fd_; }

private:
    int fd_ = -1;
};

AiServerLoggingError make_error(AiServerLoggingErrorCode code, std::string field, std::string detail,
                                int system_error = 0) {
    return {
            .code = code,
            .field = std::move(field),
            .detail = std::move(detail),
            .system_error = system_error,
    };
}

std::string child_field(std::string_view parent, std::string_view name) {
    std::string result(parent);
    result.push_back('/');
    result.append(name);
    return result;
}

std::string indexed_field(std::string_view parent, std::size_t index) {
    std::string result(parent);
    result.push_back('/');
    result.append(std::to_string(index));
    return result;
}

bool listed(std::string_view value, std::initializer_list<std::string_view> choices) noexcept {
    return std::find(choices.begin(), choices.end(), value) != choices.end();
}

std::expected<void, AiServerLoggingError> validate_object(const JsonObject<JsonAny> &object, std::string_view path,
                                                          std::initializer_list<std::string_view> allowed,
                                                          std::initializer_list<std::string_view> required = {}) {
    for (std::size_t i = 0; i < object.size(); ++i) {
        const std::string_view name = object[i].key;
        if (!listed(name, allowed)) {
            return std::unexpected(make_error(AiServerLoggingErrorCode::UnknownField, child_field(path, name),
                                              "unknown logging configuration field"));
        }
        for (std::size_t previous = 0; previous < i; ++previous) {
            if (object[previous].key == name) {
                return std::unexpected(make_error(AiServerLoggingErrorCode::DuplicateField, child_field(path, name),
                                                  "duplicate logging configuration field"));
            }
        }
    }
    for (std::string_view name: required) {
        if (!object.find_first(name)) {
            return std::unexpected(make_error(AiServerLoggingErrorCode::MissingField, child_field(path, name),
                                              "required logging configuration field is missing"));
        }
    }
    return {};
}

const JsonAny *find_field(const JsonObject<JsonAny> &object, std::string_view name) noexcept {
    const auto *entry = object.find_first(name);
    return entry ? &entry->value : nullptr;
}

std::expected<const JsonObject<JsonAny> *, AiServerLoggingError>
require_object(const JsonObject<JsonAny> &parent, std::string_view name, std::string_view path) {
    const std::string field = child_field(path, name);
    const JsonAny *value = find_field(parent, name);
    if (!value) {
        return std::unexpected(make_error(AiServerLoggingErrorCode::MissingField, field, "required object is missing"));
    }
    if (!value->is_object()) {
        return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue, field, "expected object"));
    }
    return &value->as_object();
}

std::expected<const JsonArray<JsonAny> *, AiServerLoggingError>
require_array(const JsonObject<JsonAny> &parent, std::string_view name, std::string_view path) {
    const std::string field = child_field(path, name);
    const JsonAny *value = find_field(parent, name);
    if (!value) {
        return std::unexpected(make_error(AiServerLoggingErrorCode::MissingField, field, "required array is missing"));
    }
    if (!value->is_array()) {
        return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue, field, "expected array"));
    }
    return &value->as_array();
}

std::expected<std::string_view, AiServerLoggingError> require_text(const JsonObject<JsonAny> &parent,
                                                                   std::string_view name, std::string_view path) {
    const std::string field = child_field(path, name);
    const JsonAny *value = find_field(parent, name);
    if (!value) {
        return std::unexpected(make_error(AiServerLoggingErrorCode::MissingField, field, "required string is missing"));
    }
    if (!value->is_text()) {
        return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue, field, "expected string"));
    }
    return value->as_text();
}

std::expected<std::uint64_t, AiServerLoggingError>
require_unsigned(const JsonObject<JsonAny> &parent, std::string_view name, std::string_view path, bool allow_zero) {
    const std::string field = child_field(path, name);
    const JsonAny *value = find_field(parent, name);
    if (!value) {
        return std::unexpected(
                make_error(AiServerLoggingErrorCode::MissingField, field, "required integer is missing"));
    }
    if (!value->is_integer() || value->as_integer() < 0 || (!allow_zero && value->as_integer() == 0)) {
        return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue, field,
                                          allow_zero ? "expected non-negative integer" : "expected positive integer"));
    }
    return static_cast<std::uint64_t>(value->as_integer());
}

std::expected<std::optional<std::uint64_t>, AiServerLoggingError>
optional_unsigned(const JsonObject<JsonAny> &parent, std::string_view name, std::string_view path, bool allow_zero) {
    if (!find_field(parent, name)) {
        return std::optional<std::uint64_t>{};
    }
    auto parsed = require_unsigned(parent, name, path, allow_zero);
    if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    return std::optional<std::uint64_t>(*parsed);
}

std::expected<std::optional<bool>, AiServerLoggingError> optional_bool(const JsonObject<JsonAny> &parent,
                                                                       std::string_view name, std::string_view path) {
    const JsonAny *value = find_field(parent, name);
    if (!value) {
        return std::optional<bool>{};
    }
    if (!value->is_bool()) {
        return std::unexpected(
                make_error(AiServerLoggingErrorCode::InvalidValue, child_field(path, name), "expected boolean"));
    }
    return std::optional<bool>(value->as_bool());
}

std::expected<log::LogLevel, AiServerLoggingError> parse_level(std::string_view value, std::string field) {
    if (value == "trace") {
        return log::LogLevel::Trace;
    }
    if (value == "debug") {
        return log::LogLevel::Debug;
    }
    if (value == "info") {
        return log::LogLevel::Info;
    }
    if (value == "warn") {
        return log::LogLevel::Warn;
    }
    if (value == "error") {
        return log::LogLevel::Error;
    }
    if (value == "fatal") {
        return log::LogLevel::Fatal;
    }
    return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue, std::move(field),
                                      "expected one of trace, debug, info, warn, error, fatal"));
}

std::expected<std::optional<log::LogLevel>, AiServerLoggingError>
optional_level(const JsonObject<JsonAny> &object, std::string_view name, std::string_view path) {
    const JsonAny *value = find_field(object, name);
    if (!value) {
        return std::optional<log::LogLevel>{};
    }
    if (!value->is_text()) {
        return std::unexpected(
                make_error(AiServerLoggingErrorCode::InvalidValue, child_field(path, name), "expected string"));
    }
    auto parsed = parse_level(value->as_text(), child_field(path, name));
    if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    return std::optional<log::LogLevel>(*parsed);
}

std::expected<mode_t, AiServerLoggingError> parse_mode(std::string_view value, std::string field) {
    if (value.size() != 4 || value.front() != '0') {
        return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue, std::move(field),
                                          "file mode must be a four-digit octal string"));
    }
    std::uint32_t mode = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), mode, 8);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || mode > 0777) {
        return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue, std::move(field),
                                          "file mode must be in range 0000 through 0777"));
    }
    return static_cast<mode_t>(mode);
}

std::expected<std::string, AiServerLoggingError> resolve_path(std::string_view source_path, std::string_view value,
                                                              std::string field) {
    if (value.empty() || value.size() > 4096 || value.find('\0') != std::string_view::npos) {
        return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue, std::move(field),
                                          "expected a non-empty NUL-free path of at most 4096 bytes"));
    }
    namespace fs = std::filesystem;
    fs::path target{std::string(value)};
    if (!target.is_absolute() && !source_path.empty()) {
        target = fs::path(source_path).parent_path() / target;
    }
    std::error_code error;
    fs::path absolute = fs::absolute(target, error);
    if (error) {
        return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue, std::move(field),
                                          "failed to resolve path", error.value()));
    }
    std::string result = absolute.lexically_normal().string();
    if (result.empty() || result.size() > 4096) {
        return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue, std::move(field),
                                          "resolved path exceeds 4096 bytes"));
    }
    return result;
}

AiServerLoggingError build_error(std::string field, const log::LogConfigError &error) {
    return make_error(AiServerLoggingErrorCode::BuildFailed, std::move(field), error.message, error.system_error);
}

std::expected<std::vector<log::AppenderId>, AiServerLoggingError>
resolve_appender_references(const JsonArray<JsonAny> &references, std::string_view path, const AppenderMap &appenders,
                            StringSet &used_appenders, bool require_non_empty) {
    if (require_non_empty && references.empty()) {
        return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue, std::string(path),
                                          "at least one appender reference is required"));
    }

    std::vector<log::AppenderId> targets;
    targets.reserve(references.size());
    StringSet local_names;
    for (std::size_t index = 0; index < references.size(); ++index) {
        const std::string field = indexed_field(path, index);
        const JsonAny &value = references[index];
        if (!value.is_text()) {
            return std::unexpected(
                    make_error(AiServerLoggingErrorCode::InvalidValue, field, "expected appender name string"));
        }
        const std::string_view name = value.as_text();
        const auto appender = appenders.find(name);
        if (appender == appenders.end()) {
            return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidReference, field,
                                              "logger references unknown appender"));
        }
        if (!local_names.emplace(name).second) {
            return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidReference, field,
                                              "appender reference must not be repeated"));
        }
        used_appenders.emplace(name);
        targets.push_back(appender->second);
    }
    return targets;
}

std::expected<log::FileRotationOptions, AiServerLoggingError> parse_rotation(const JsonObject<JsonAny> &object,
                                                                             std::string_view path) {
    auto valid = validate_object(object, path, {"max_bytes", "archive_name", "max_archives"},
                                 {"max_bytes", "archive_name", "max_archives"});
    if (!valid) {
        return std::unexpected(std::move(valid.error()));
    }
    auto max_bytes = require_unsigned(object, "max_bytes", path, false);
    auto archive_name = require_text(object, "archive_name", path);
    auto max_archives = require_unsigned(object, "max_archives", path, false);
    if (!max_bytes) {
        return std::unexpected(std::move(max_bytes.error()));
    }
    if (!archive_name) {
        return std::unexpected(std::move(archive_name.error()));
    }
    if (!max_archives) {
        return std::unexpected(std::move(max_archives.error()));
    }
    if (*max_archives > log::kMaxRetainedLogArchives) {
        return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue, child_field(path, "max_archives"),
                                          "max_archives is too large"));
    }
    return log::FileRotationOptions{
            .max_file_size = *max_bytes,
            .archive_name = std::string(*archive_name),
            .max_archives = static_cast<std::uint32_t>(*max_archives),
    };
}

std::expected<void, AiServerLoggingError> add_operational_appenders(const JsonArray<JsonAny> &definitions,
                                                                    std::string_view source_path,
                                                                    log::LogConfigBuilder &builder,
                                                                    AppenderMap &appenders, StringSet &file_paths) {
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        const std::string path = indexed_field("/appenders", index);
        const JsonAny &value = definitions[index];
        if (!value.is_object()) {
            return std::unexpected(
                    make_error(AiServerLoggingErrorCode::InvalidValue, path, "expected appender object"));
        }
        const JsonObject<JsonAny> &object = value.as_object();
        auto valid = validate_object(object, path,
                                     {"name", "type", "stream", "path", "mode", "buffer_bytes", "flush_interval_ms",
                                      "min_level", "max_level", "rotation"},
                                     {"name", "type"});
        if (!valid) {
            return std::unexpected(std::move(valid.error()));
        }

        auto name_value = require_text(object, "name", path);
        auto type_value = require_text(object, "type", path);
        if (!name_value) {
            return std::unexpected(std::move(name_value.error()));
        }
        if (!type_value) {
            return std::unexpected(std::move(type_value.error()));
        }
        const std::string name(*name_value);
        if (!log::valid_logger_name(name)) {
            return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue, child_field(path, "name"),
                                              "invalid appender name"));
        }
        if (name == kAuditAppenderName) {
            return std::unexpected(make_error(AiServerLoggingErrorCode::ReservedName, child_field(path, "name"),
                                              "appender name is reserved by ai-server"));
        }
        if (appenders.contains(name)) {
            return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue, child_field(path, "name"),
                                              "duplicate appender name"));
        }

        auto min_level_value = optional_level(object, "min_level", path);
        auto max_level_value = optional_level(object, "max_level", path);
        if (!min_level_value) {
            return std::unexpected(std::move(min_level_value.error()));
        }
        if (!max_level_value) {
            return std::unexpected(std::move(max_level_value.error()));
        }
        const log::LogLevel min_level = min_level_value->value_or(log::LogLevel::Trace);
        const log::LogLevel max_level = max_level_value->value_or(log::LogLevel::Fatal);
        if (log::level_index(min_level) > log::level_index(max_level)) {
            return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue, path,
                                              "appender min_level must not exceed max_level"));
        }

        log::LogConfigResult<log::AppenderId> added = std::unexpected(log::LogConfigError{});
        if (*type_value == "console") {
            if (find_field(object, "path") || find_field(object, "mode") || find_field(object, "buffer_bytes") ||
                find_field(object, "flush_interval_ms") || find_field(object, "rotation")) {
                return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue, path,
                                                  "console appender does not accept file options"));
            }
            auto stream = require_text(object, "stream", path);
            if (!stream) {
                return std::unexpected(std::move(stream.error()));
            }
            if (*stream != "stderr") {
                return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue, child_field(path, "stream"),
                                                  "ai-server console appenders must use stderr"));
            }
            added = builder.add_console_appender({
                    .name = name,
                    .stream = log::ConsoleStream::Stderr,
                    .min_level = min_level,
                    .max_level = max_level,
            });
        } else if (*type_value == "file") {
            if (find_field(object, "stream")) {
                return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue, child_field(path, "stream"),
                                                  "file appender does not accept stream"));
            }
            auto raw_path = require_text(object, "path", path);
            if (!raw_path) {
                return std::unexpected(std::move(raw_path.error()));
            }
            auto resolved_path = resolve_path(source_path, *raw_path, child_field(path, "path"));
            if (!resolved_path) {
                return std::unexpected(std::move(resolved_path.error()));
            }
            if (!file_paths.emplace(*resolved_path).second) {
                return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue, child_field(path, "path"),
                                                  "multiple appenders use the same file path"));
            }

            mode_t mode = 0644;
            if (const JsonAny *mode_value = find_field(object, "mode")) {
                if (!mode_value->is_text()) {
                    return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue, child_field(path, "mode"),
                                                      "expected octal mode string"));
                }
                auto parsed_mode = parse_mode(mode_value->as_text(), child_field(path, "mode"));
                if (!parsed_mode) {
                    return std::unexpected(std::move(parsed_mode.error()));
                }
                mode = *parsed_mode;
            }

            auto buffer_bytes = optional_unsigned(object, "buffer_bytes", path, false);
            auto flush_interval = optional_unsigned(object, "flush_interval_ms", path, false);
            if (!buffer_bytes) {
                return std::unexpected(std::move(buffer_bytes.error()));
            }
            if (!flush_interval) {
                return std::unexpected(std::move(flush_interval.error()));
            }
            if (buffer_bytes->has_value() != flush_interval->has_value()) {
                return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue, path,
                                                  "buffer_bytes and flush_interval_ms must appear together"));
            }
            if (buffer_bytes->value_or(0) > std::numeric_limits<std::size_t>::max()) {
                return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue,
                                                  child_field(path, "buffer_bytes"),
                                                  "buffer size exceeds platform size limit"));
            }
            if (flush_interval->value_or(0) >
                static_cast<std::uint64_t>(std::numeric_limits<std::chrono::milliseconds::rep>::max())) {
                return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue,
                                                  child_field(path, "flush_interval_ms"),
                                                  "flush interval exceeds platform duration limit"));
            }

            log::FileAppenderOptions options{
                    .name = name,
                    .path = std::move(*resolved_path),
                    .file_mode = mode,
                    .buffer_size = static_cast<std::size_t>(buffer_bytes->value_or(0)),
                    .flush_interval = std::chrono::milliseconds(flush_interval->value_or(0)),
                    .min_level = min_level,
                    .max_level = max_level,
            };
            if (const JsonAny *rotation = find_field(object, "rotation")) {
                if (!rotation->is_object()) {
                    return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue,
                                                      child_field(path, "rotation"), "expected rotation object"));
                }
                auto parsed_rotation = parse_rotation(rotation->as_object(), child_field(path, "rotation"));
                if (!parsed_rotation) {
                    return std::unexpected(std::move(parsed_rotation.error()));
                }
                options.rotation = std::move(*parsed_rotation);
            }
            added = builder.add_file_appender(std::move(options));
        } else {
            return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue, child_field(path, "type"),
                                              "appender type must be console or file"));
        }

        if (!added) {
            return std::unexpected(build_error(path, added.error()));
        }
        appenders.emplace(name, *added);
    }
    return {};
}

std::expected<LlmAuditLogOptions, AiServerLoggingError>
parse_audit_options(const JsonObject<JsonAny> &object, std::string_view source_path, StringSet &file_paths) {
    constexpr std::string_view path = "/audit";
    auto valid = validate_object(object, path, {"path", "max_record_bytes", "rotate_bytes", "max_archives"},
                                 {"path", "max_record_bytes", "rotate_bytes", "max_archives"});
    if (!valid) {
        return std::unexpected(std::move(valid.error()));
    }

    auto raw_path = require_text(object, "path", path);
    auto max_record_bytes = require_unsigned(object, "max_record_bytes", path, false);
    auto rotate_bytes = require_unsigned(object, "rotate_bytes", path, true);
    auto max_archives = require_unsigned(object, "max_archives", path, false);
    if (!raw_path) {
        return std::unexpected(std::move(raw_path.error()));
    }
    if (!max_record_bytes) {
        return std::unexpected(std::move(max_record_bytes.error()));
    }
    if (!rotate_bytes) {
        return std::unexpected(std::move(rotate_bytes.error()));
    }
    if (!max_archives) {
        return std::unexpected(std::move(max_archives.error()));
    }
    if (*max_record_bytes > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue, child_field(path, "max_record_bytes"),
                                          "audit record size exceeds platform size limit"));
    }
    if (*max_archives > log::kMaxRetainedLogArchives) {
        return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue, child_field(path, "max_archives"),
                                          "max_archives is too large"));
    }

    auto resolved_path = resolve_path(source_path, *raw_path, child_field(path, "path"));
    if (!resolved_path) {
        return std::unexpected(std::move(resolved_path.error()));
    }
    if (!file_paths.emplace(*resolved_path).second) {
        return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue, child_field(path, "path"),
                                          "audit path duplicates another file appender"));
    }

    return LlmAuditLogOptions{
            .path = std::move(*resolved_path),
            .max_record_bytes = static_cast<std::size_t>(*max_record_bytes),
            .rotate_bytes = *rotate_bytes,
            .max_archives = static_cast<std::uint32_t>(*max_archives),
    };
}

std::expected<void, AiServerLoggingError> add_logger_rules(const JsonArray<JsonAny> &rules,
                                                           const AppenderMap &appenders, StringSet &used_appenders,
                                                           log::LogConfigBuilder &builder) {
    StringSet logger_names;
    for (std::size_t index = 0; index < rules.size(); ++index) {
        const std::string path = indexed_field("/loggers", index);
        const JsonAny &value = rules[index];
        if (!value.is_object()) {
            return std::unexpected(
                    make_error(AiServerLoggingErrorCode::InvalidValue, path, "expected logger rule object"));
        }
        const JsonObject<JsonAny> &object = value.as_object();
        auto valid = validate_object(object, path, {"name", "level", "verbosity", "appenders", "additive"}, {"name"});
        if (!valid) {
            return std::unexpected(std::move(valid.error()));
        }

        auto name_value = require_text(object, "name", path);
        if (!name_value) {
            return std::unexpected(std::move(name_value.error()));
        }
        if (*name_value == kAiServerAuditLogger) {
            return std::unexpected(make_error(AiServerLoggingErrorCode::ReservedName, child_field(path, "name"),
                                              "ai_server.audit is configured internally"));
        }
        if (!configurable_ai_server_logger(*name_value)) {
            return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue, child_field(path, "name"),
                                              "logger is not a configurable ai-server category"));
        }
        if (!logger_names.emplace(*name_value).second) {
            return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue, child_field(path, "name"),
                                              "duplicate logger rule"));
        }

        auto level = optional_level(object, "level", path);
        auto verbosity_value = optional_unsigned(object, "verbosity", path, true);
        auto additive_value = optional_bool(object, "additive", path);
        if (!level) {
            return std::unexpected(std::move(level.error()));
        }
        if (!verbosity_value) {
            return std::unexpected(std::move(verbosity_value.error()));
        }
        if (!additive_value) {
            return std::unexpected(std::move(additive_value.error()));
        }
        if (verbosity_value->value_or(0) > std::numeric_limits<unsigned>::max()) {
            return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue, child_field(path, "verbosity"),
                                              "verbosity is too large"));
        }

        std::vector<log::AppenderId> targets;
        if (const JsonAny *references = find_field(object, "appenders")) {
            if (!references->is_array()) {
                return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue,
                                                  child_field(path, "appenders"), "expected array"));
            }
            auto resolved = resolve_appender_references(references->as_array(), child_field(path, "appenders"),
                                                        appenders, used_appenders, false);
            if (!resolved) {
                return std::unexpected(std::move(resolved.error()));
            }
            targets = std::move(*resolved);
        }
        if (!level->has_value() && !verbosity_value->has_value() && targets.empty() && additive_value->value_or(true)) {
            return std::unexpected(
                    make_error(AiServerLoggingErrorCode::InvalidValue, path,
                               "logger rule does not change level, verbosity, appenders, or propagation"));
        }

        log::LoggerOptions options{
                .name = std::string(*name_value),
                .level = *level,
                .additive = additive_value->value_or(true),
        };
        if (verbosity_value->has_value()) {
            options.verbosity = static_cast<unsigned>(**verbosity_value);
        }
        auto added = builder.add_logger(std::move(options), std::move(targets));
        if (!added) {
            return std::unexpected(build_error(path, added.error()));
        }
    }
    return {};
}

std::expected<AiServerLogConfig, AiServerLoggingError> build_config(const JsonObject<JsonAny> &root,
                                                                    std::string_view source_path) {
    auto valid_root = validate_object(root, "", {"version", "queue", "appenders", "root_logger", "loggers", "audit"},
                                      {"version", "queue", "appenders", "root_logger", "loggers", "audit"});
    if (!valid_root) {
        return std::unexpected(std::move(valid_root.error()));
    }

    const JsonAny *version = find_field(root, "version");
    if (!version->is_integer() || version->as_integer() != 1) {
        return std::unexpected(make_error(AiServerLoggingErrorCode::UnsupportedVersion, "/version",
                                          "logging configuration version must be 1"));
    }

    auto queue = require_object(root, "queue", "");
    if (!queue) {
        return std::unexpected(std::move(queue.error()));
    }
    auto valid_queue = validate_object(**queue, "/queue", {"capacity_bytes"}, {"capacity_bytes"});
    if (!valid_queue) {
        return std::unexpected(std::move(valid_queue.error()));
    }
    auto queue_capacity = require_unsigned(**queue, "capacity_bytes", "/queue", false);
    if (!queue_capacity) {
        return std::unexpected(std::move(queue_capacity.error()));
    }
    if (*queue_capacity > std::numeric_limits<std::size_t>::max() ||
        *queue_capacity == std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidValue, "/queue/capacity_bytes",
                                          "queue capacity exceeds platform size limit"));
    }

    auto appender_definitions = require_array(root, "appenders", "");
    auto logger_rules = require_array(root, "loggers", "");
    auto root_logger = require_object(root, "root_logger", "");
    auto audit_object = require_object(root, "audit", "");
    if (!appender_definitions) {
        return std::unexpected(std::move(appender_definitions.error()));
    }
    if (!logger_rules) {
        return std::unexpected(std::move(logger_rules.error()));
    }
    if (!root_logger) {
        return std::unexpected(std::move(root_logger.error()));
    }
    if (!audit_object) {
        return std::unexpected(std::move(audit_object.error()));
    }

    log::LogConfigBuilder builder;
    AppenderMap appenders;
    StringSet file_paths;
    auto operational = add_operational_appenders(**appender_definitions, source_path, builder, appenders, file_paths);
    if (!operational) {
        return std::unexpected(std::move(operational.error()));
    }

    auto audit = parse_audit_options(**audit_object, source_path, file_paths);
    if (!audit) {
        return std::unexpected(std::move(audit.error()));
    }

    log::FileAppenderOptions audit_file{
            .name = std::string(kAuditAppenderName),
            .path = audit->path,
            .file_mode = 0600,
            .min_level = log::LogLevel::Info,
            .max_level = log::LogLevel::Info,
            .layout = log::FileAppenderLayout::MessageOnly,
            .no_follow = true,
            .regular_file_only = true,
            .enforce_file_mode = true,
            .truncate_incomplete_tail = true,
    };
    if (audit->rotate_bytes != 0) {
        audit_file.rotation = log::FileRotationOptions{
                .max_file_size = audit->rotate_bytes,
                .archive_name = "{base}.{utc}.{seq}",
                .max_archives = audit->max_archives,
        };
    }
    auto audit_appender = builder.add_file_appender(std::move(audit_file));
    if (!audit_appender) {
        return std::unexpected(build_error("/audit", audit_appender.error()));
    }

    StringSet used_appenders;
    auto named_loggers = add_logger_rules(**logger_rules, appenders, used_appenders, builder);
    if (!named_loggers) {
        return std::unexpected(std::move(named_loggers.error()));
    }

    auto valid_root_logger =
            validate_object(**root_logger, "/root_logger", {"level", "verbosity", "appenders"}, {"level", "appenders"});
    if (!valid_root_logger) {
        return std::unexpected(std::move(valid_root_logger.error()));
    }
    auto root_level_text = require_text(**root_logger, "level", "/root_logger");
    auto root_references = require_array(**root_logger, "appenders", "/root_logger");
    auto root_verbosity = optional_unsigned(**root_logger, "verbosity", "/root_logger", true);
    if (!root_level_text) {
        return std::unexpected(std::move(root_level_text.error()));
    }
    if (!root_references) {
        return std::unexpected(std::move(root_references.error()));
    }
    if (!root_verbosity) {
        return std::unexpected(std::move(root_verbosity.error()));
    }
    if (root_verbosity->value_or(0) > std::numeric_limits<unsigned>::max()) {
        return std::unexpected(
                make_error(AiServerLoggingErrorCode::InvalidValue, "/root_logger/verbosity", "verbosity is too large"));
    }
    auto root_level = parse_level(*root_level_text, "/root_logger/level");
    if (!root_level) {
        return std::unexpected(std::move(root_level.error()));
    }
    auto root_targets =
            resolve_appender_references(**root_references, "/root_logger/appenders", appenders, used_appenders, true);
    if (!root_targets) {
        return std::unexpected(std::move(root_targets.error()));
    }

    for (const auto &[name, id]: appenders) {
        (void) id;
        if (!used_appenders.contains(name)) {
            return std::unexpected(make_error(AiServerLoggingErrorCode::InvalidReference, "/appenders",
                                              "appender is not referenced by any logger: " + name));
        }
    }

    auto audit_logger = builder.add_logger(
            {
                    .name = std::string(kAiServerAuditLogger),
                    .level = log::LogLevel::Info,
                    .additive = false,
            },
            {*audit_appender});
    if (!audit_logger) {
        return std::unexpected(build_error("/audit", audit_logger.error()));
    }

    auto async = builder.set_async_options({
            .backlog_capacity = static_cast<std::size_t>(*queue_capacity),
            .full_policy = log::LogQueueFullPolicy::DropNewest,
    });
    if (!async) {
        return std::unexpected(build_error("/queue", async.error()));
    }

    auto configured_root = builder.set_root_logger(
            {
                    .level = *root_level,
                    .verbosity = static_cast<unsigned>(root_verbosity->value_or(0)),
            },
            std::move(*root_targets));
    if (!configured_root) {
        return std::unexpected(build_error("/root_logger", configured_root.error()));
    }

    auto config = builder.finish();
    if (!config) {
        return std::unexpected(build_error({}, config.error()));
    }
    return AiServerLogConfig{
            .config = std::move(*config),
            .audit = std::move(*audit),
            .audit_appender_id = *audit_appender,
    };
}

AiServerLoggingError json_error(std::string_view content, const json::ParseError &parse_error) {
    AiServerLoggingError error = make_error(AiServerLoggingErrorCode::InvalidJson, {},
                                            parse_error.message ? parse_error.message : "invalid JSON");
    error.offset = std::min(parse_error.offset, content.size());
    error.line = 1;
    error.column = 1;
    for (std::size_t index = 0; index < error.offset; ++index) {
        if (content[index] == '\n') {
            ++error.line;
            error.column = 1;
        } else {
            ++error.column;
        }
    }
    return error;
}

std::expected<std::string, AiServerLoggingError> read_config_file(std::string_view path) {
    const std::string owned_path(path);
    const int fd = ::open(owned_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return std::unexpected(make_error(AiServerLoggingErrorCode::OpenFailed, {},
                                          "failed to open logging configuration file", errno));
    }
    FileHandle handle(fd);

    struct stat status{};
    if (::fstat(handle.get(), &status) != 0) {
        return std::unexpected(make_error(AiServerLoggingErrorCode::ReadFailed, {},
                                          "failed to inspect logging configuration file", errno));
    }
    if (status.st_size > static_cast<off_t>(kMaxLoggingConfigBytes)) {
        return std::unexpected(
                make_error(AiServerLoggingErrorCode::FileTooLarge, {}, "logging configuration file exceeds 1 MiB"));
    }

    std::string content;
    if (status.st_size > 0) {
        content.reserve(static_cast<std::size_t>(status.st_size));
    }
    std::array<char, 8192> buffer{};
    for (;;) {
        const ssize_t count = ::read(handle.get(), buffer.data(), buffer.size());
        if (count == 0) {
            break;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(make_error(AiServerLoggingErrorCode::ReadFailed, {},
                                              "failed to read logging configuration file", errno));
        }
        if (content.size() > kMaxLoggingConfigBytes - static_cast<std::size_t>(count)) {
            return std::unexpected(
                    make_error(AiServerLoggingErrorCode::FileTooLarge, {}, "logging configuration file exceeds 1 MiB"));
        }
        content.append(buffer.data(), static_cast<std::size_t>(count));
    }
    return content;
}

} // namespace

std::expected<AiServerLogConfig, AiServerLoggingError> parse_ai_server_log_config(std::string_view content,
                                                                                  std::string_view source_path) {
    if (content.size() > kMaxLoggingConfigBytes) {
        return std::unexpected(
                make_error(AiServerLoggingErrorCode::FileTooLarge, {}, "logging configuration file exceeds 1 MiB"));
    }

    json::JsonParser parser;
    if (!parser.feed(content.data(), content.size())) {
        return std::unexpected(json_error(content, parser.error()));
    }
    parser.finish();
    mem::BufPool pool;
    JsonAny root;
    const auto status = json::parse_document(
            parser, pool, root, [](json::JsonParser &value_parser, mem::BufPool &value_pool, JsonAny &value) noexcept {
                return json::parse_any(value_parser, value_pool, value);
            });
    if (status != json::ParseStatus::Done) {
        return std::unexpected(json_error(content, parser.error()));
    }
    if (!root.is_object()) {
        return std::unexpected(
                make_error(AiServerLoggingErrorCode::InvalidValue, {}, "logging configuration must be an object"));
    }
    return build_config(root.as_object(), source_path);
}

std::expected<AiServerLogConfig, AiServerLoggingError> load_ai_server_log_config(std::string_view path) {
    auto resolved_path = resolve_path({}, path, {});
    if (!resolved_path) {
        return std::unexpected(std::move(resolved_path.error()));
    }
    auto content = read_config_file(*resolved_path);
    if (!content) {
        return std::unexpected(std::move(content.error()));
    }
    return parse_ai_server_log_config(*content, *resolved_path);
}

} // namespace fiber::ai_server
