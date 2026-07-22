#include "CatSystemStats.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string_view>
#include <unistd.h>

namespace fiber::cat::detail {

namespace {

bool checked_add(std::uint64_t value, std::uint64_t &total) noexcept {
    if (value > std::numeric_limits<std::uint64_t>::max() - total) {
        return false;
    }
    total += value;
    return true;
}

bool checked_multiply(std::uint64_t left, std::uint64_t right, std::uint64_t &result) noexcept {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

bool next_token(std::string_view text, std::size_t &position, std::string_view &token) noexcept {
    while (position < text.size() &&
           (text[position] == ' ' || text[position] == '\t' || text[position] == '\n' || text[position] == '\r')) {
        ++position;
    }
    if (position == text.size()) {
        return false;
    }
    const std::size_t begin = position;
    while (position < text.size() && text[position] != ' ' && text[position] != '\t' && text[position] != '\n' &&
           text[position] != '\r') {
        ++position;
    }
    token = text.substr(begin, position - begin);
    return true;
}

bool parse_uint(std::string_view token, std::uint64_t &value) noexcept {
    token = trim(token);
    if (token.empty()) {
        return false;
    }
    auto result = std::from_chars(token.data(), token.data() + token.size(), value);
    return result.ec == std::errc{} && result.ptr == token.data() + token.size();
}

bool parse_double(std::string_view token, double &value) noexcept {
    token = trim(token);
    if (token.empty()) {
        return false;
    }
    auto result = std::from_chars(token.data(), token.data() + token.size(), value);
    return result.ec == std::errc{} && result.ptr == token.data() + token.size() && std::isfinite(value) &&
           value >= 0.0;
}

bool parse_uint_after_prefix(std::string_view line, std::string_view prefix, std::uint64_t &value) noexcept {
    if (!line.starts_with(prefix)) {
        return false;
    }
    std::size_t position = prefix.size();
    std::string_view token;
    return next_token(line, position, token) && parse_uint(token, value);
}

bool cpu_total(const SystemCpuCounters &cpu, std::uint64_t &total) noexcept {
    total = 0;
    return checked_add(cpu.user, total) && checked_add(cpu.nice, total) && checked_add(cpu.system, total) &&
           checked_add(cpu.idle, total) && checked_add(cpu.iowait, total) && checked_add(cpu.irq, total) &&
           checked_add(cpu.softirq, total);
}

bool cpu_delta(const SystemCpuCounters &current, const SystemCpuCounters &previous, SystemCpuCounters &delta) noexcept {
    if (current.user < previous.user || current.nice < previous.nice || current.system < previous.system ||
        current.idle < previous.idle || current.iowait < previous.iowait || current.irq < previous.irq ||
        current.softirq < previous.softirq) {
        return false;
    }
    delta = {
            .user = current.user - previous.user,
            .nice = current.nice - previous.nice,
            .system = current.system - previous.system,
            .idle = current.idle - previous.idle,
            .iowait = current.iowait - previous.iowait,
            .irq = current.irq - previous.irq,
            .softirq = current.softirq - previous.softirq,
    };
    return true;
}

double percentage(std::uint64_t part, std::uint64_t total) noexcept {
    return total == 0 ? 0.0 : static_cast<double>(static_cast<long double>(part) * 100.0L / total);
}

bool parse_cpu_line(std::string_view line, SystemCpuCounters &cpu) noexcept {
    if (!line.starts_with("cpu ") && !line.starts_with("cpu\t")) {
        return false;
    }
    std::size_t position = 3;
    std::array<std::uint64_t *, 7> values{
            &cpu.user, &cpu.nice, &cpu.system, &cpu.idle, &cpu.iowait, &cpu.irq, &cpu.softirq,
    };
    for (std::uint64_t *value: values) {
        std::string_view token;
        if (!next_token(line, position, token) || !parse_uint(token, *value)) {
            return false;
        }
    }
    return true;
}

struct ProcStatState {
    bool cpu = false;
    bool context_switches = false;
    bool interrupts = false;
    bool processes_running = false;
    bool processes_blocked = false;
};

void parse_proc_stat_line(std::string_view line, RawSystemStats &stats, ProcStatState &state) noexcept {
    if (!state.cpu && parse_cpu_line(line, stats.cpu)) {
        state.cpu = true;
    } else if (!state.context_switches && parse_uint_after_prefix(line, "ctxt", stats.context_switches)) {
        state.context_switches = true;
    } else if (!state.interrupts && parse_uint_after_prefix(line, "intr", stats.interrupts)) {
        state.interrupts = true;
    } else if (!state.processes_running && parse_uint_after_prefix(line, "procs_running", stats.processes_running)) {
        state.processes_running = true;
    } else if (!state.processes_blocked && parse_uint_after_prefix(line, "procs_blocked", stats.processes_blocked)) {
        state.processes_blocked = true;
    }
}

bool finalize_proc_stat(RawSystemStats &stats, const ProcStatState &state) noexcept {
    stats.cpu_valid = state.cpu;
    stats.scheduler_valid =
            state.context_switches && state.interrupts && state.processes_running && state.processes_blocked;
    return stats.cpu_valid && stats.scheduler_valid;
}

bool parse_kib_line(std::string_view line, std::string_view key, std::uint64_t &bytes) noexcept {
    if (!line.starts_with(key)) {
        return false;
    }
    std::size_t position = key.size();
    std::string_view token;
    std::uint64_t kib = 0;
    if (!next_token(line, position, token) || !parse_uint(token, kib)) {
        return false;
    }
    if (!next_token(line, position, token) || token != "kB") {
        return false;
    }
    return checked_multiply(kib, 1024, bytes);
}

template<std::size_t Capacity>
bool read_file(std::string_view path, std::array<char, Capacity> &storage, std::string_view &result) noexcept {
    std::array<char, 128> terminated_path{};
    if (path.size() >= terminated_path.size()) {
        return false;
    }
    std::copy(path.begin(), path.end(), terminated_path.begin());
    std::FILE *file = std::fopen(terminated_path.data(), "r");
    if (!file) {
        return false;
    }
    const std::size_t size = std::fread(storage.data(), 1, storage.size(), file);
    const bool failed = std::ferror(file) != 0;
    bool truncated = false;
    if (!failed && size == storage.size()) {
        truncated = std::fgetc(file) != EOF;
    }
    std::fclose(file);
    if (failed || truncated) {
        return false;
    }
    result = {storage.data(), size};
    return true;
}

bool read_proc_stat(RawSystemStats &stats) noexcept {
    std::FILE *file = std::fopen("/proc/stat", "r");
    if (!file) {
        return false;
    }
    ProcStatState state;
    std::array<char, 1024> line{};
    while (std::fgets(line.data(), static_cast<int>(line.size()), file)) {
        parse_proc_stat_line(std::string_view(line.data()), stats, state);
    }
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    return !failed && finalize_proc_stat(stats, state);
}

RawSystemStats read_raw_system_stats() noexcept {
    RawSystemStats stats;
    std::array<char, 256> load_storage{};
    std::array<char, 8192> memory_storage{};
    std::array<char, 4096> process_storage{};
    std::array<char, 256> process_memory_storage{};
    std::string_view text;

    const bool load_ok = read_file("/proc/loadavg", load_storage, text) && parse_load_average(text, stats);
    const bool cpu_ok = read_proc_stat(stats);
    const bool memory_ok = read_file("/proc/meminfo", memory_storage, text) && parse_meminfo(text, stats);
    const bool process_cpu_ok = read_file("/proc/self/stat", process_storage, text) && parse_process_stat(text, stats);
    const long page_size = ::sysconf(_SC_PAGESIZE);
    const bool process_memory_ok = page_size > 0 && read_file("/proc/self/statm", process_memory_storage, text) &&
                                   parse_process_statm(text, static_cast<std::uint64_t>(page_size), stats);
    stats.provider_failure = !(load_ok && cpu_ok && memory_ok && process_cpu_ok && process_memory_ok);
    return stats;
}

} // namespace

bool parse_load_average(std::string_view text, RawSystemStats &stats) noexcept {
    std::size_t position = 0;
    std::string_view token;
    double load_1min = 0.0;
    double load_5min = 0.0;
    double load_15min = 0.0;
    if (!next_token(text, position, token) || !parse_double(token, load_1min) || !next_token(text, position, token) ||
        !parse_double(token, load_5min) || !next_token(text, position, token) || !parse_double(token, load_15min)) {
        return false;
    }
    stats.load_1min = load_1min;
    stats.load_5min = load_5min;
    stats.load_15min = load_15min;
    stats.load_valid = true;
    return true;
}

bool parse_proc_stat(std::string_view text, RawSystemStats &stats) noexcept {
    ProcStatState state;
    std::size_t position = 0;
    while (position < text.size()) {
        const std::size_t end = text.find('\n', position);
        const std::size_t length = end == std::string_view::npos ? text.size() - position : end - position;
        parse_proc_stat_line(text.substr(position, length), stats, state);
        if (end == std::string_view::npos) {
            break;
        }
        position = end + 1;
    }
    return finalize_proc_stat(stats, state);
}

bool parse_meminfo(std::string_view text, RawSystemStats &stats) noexcept {
    bool total = false;
    bool free = false;
    bool cached = false;
    bool swap_total = false;
    bool swap_free = false;
    std::size_t position = 0;
    while (position < text.size()) {
        const std::size_t end = text.find('\n', position);
        const std::size_t length = end == std::string_view::npos ? text.size() - position : end - position;
        const std::string_view line = text.substr(position, length);
        if (!total && parse_kib_line(line, "MemTotal:", stats.memory_total_bytes)) {
            total = true;
        } else if (!free && parse_kib_line(line, "MemFree:", stats.memory_free_bytes)) {
            free = true;
        } else if (!cached && parse_kib_line(line, "Cached:", stats.memory_cached_bytes)) {
            cached = true;
        } else if (!swap_total && parse_kib_line(line, "SwapTotal:", stats.swap_total_bytes)) {
            swap_total = true;
        } else if (!swap_free && parse_kib_line(line, "SwapFree:", stats.swap_free_bytes)) {
            swap_free = true;
        }
        if (end == std::string_view::npos) {
            break;
        }
        position = end + 1;
    }
    stats.memory_valid = total && free && cached && swap_total && swap_free && stats.memory_total_bytes != 0 &&
                         stats.memory_free_bytes <= stats.memory_total_bytes;
    return stats.memory_valid;
}

bool parse_process_stat(std::string_view text, RawSystemStats &stats) noexcept {
    const std::size_t command_end = text.rfind(')');
    if (command_end == std::string_view::npos || command_end + 1 >= text.size()) {
        return false;
    }
    std::size_t position = command_end + 1;
    std::string_view token;
    if (!next_token(text, position, token)) {
        return false;
    }
    std::uint64_t user = 0;
    std::uint64_t system = 0;
    for (unsigned field = 4; field <= 15; ++field) {
        if (!next_token(text, position, token)) {
            return false;
        }
        if (field == 14 && !parse_uint(token, user)) {
            return false;
        }
        if (field == 15 && !parse_uint(token, system)) {
            return false;
        }
    }
    stats.process_cpu = {.user = user, .system = system};
    stats.process_cpu_valid = true;
    return true;
}

bool parse_process_statm(std::string_view text, std::uint64_t page_size, RawSystemStats &stats) noexcept {
    if (page_size == 0) {
        return false;
    }
    std::size_t position = 0;
    std::string_view token;
    std::uint64_t virtual_pages = 0;
    std::uint64_t resident_pages = 0;
    if (!next_token(text, position, token) || !parse_uint(token, virtual_pages) || !next_token(text, position, token) ||
        !parse_uint(token, resident_pages) ||
        !checked_multiply(virtual_pages, page_size, stats.process_virtual_bytes) ||
        !checked_multiply(resident_pages, page_size, stats.process_rss_bytes)) {
        return false;
    }
    stats.process_memory_valid = true;
    return true;
}

HeartbeatSystemStats SystemStatsCollector::collect() noexcept { return observe(read_raw_system_stats()); }

HeartbeatSystemStats SystemStatsCollector::observe(const RawSystemStats &current) noexcept {
    HeartbeatSystemStats result{
            .load_1min = current.load_1min,
            .load_5min = current.load_5min,
            .load_15min = current.load_15min,
            .processes_running = current.processes_running,
            .processes_blocked = current.processes_blocked,
            .memory_total_bytes = current.memory_total_bytes,
            .memory_free_bytes = current.memory_free_bytes,
            .memory_cached_bytes = current.memory_cached_bytes,
            .swap_total_bytes = current.swap_total_bytes,
            .swap_free_bytes = current.swap_free_bytes,
            .process_virtual_bytes = current.process_virtual_bytes,
            .process_rss_bytes = current.process_rss_bytes,
            .load_valid = current.load_valid,
            .scheduler_valid = current.scheduler_valid,
            .memory_valid = current.memory_valid,
            .process_memory_valid = current.process_memory_valid,
            .provider_failure = current.provider_failure,
    };

    if (current.memory_valid) {
        result.memory_free_percent = percentage(current.memory_free_bytes, current.memory_total_bytes);
        result.memory_used_percent = 100.0 - result.memory_free_percent;
    }

    if (has_previous_ && current.cpu_valid && previous_.cpu_valid &&
        cpu_delta(current.cpu, previous_.cpu, result.cpu_delta)) {
        std::uint64_t total = 0;
        if (cpu_total(result.cpu_delta, total) && total != 0) {
            result.cpu_user_percent = percentage(result.cpu_delta.user, total);
            result.cpu_nice_percent = percentage(result.cpu_delta.nice, total);
            result.cpu_system_percent = percentage(result.cpu_delta.system, total);
            result.cpu_idle_percent = percentage(result.cpu_delta.idle, total);
            result.cpu_iowait_percent = percentage(result.cpu_delta.iowait, total);
            result.cpu_irq_percent = percentage(result.cpu_delta.irq, total);
            result.cpu_softirq_percent = percentage(result.cpu_delta.softirq, total);
            result.cpu_valid = true;

            if (current.process_cpu_valid && previous_.process_cpu_valid &&
                current.process_cpu.user >= previous_.process_cpu.user &&
                current.process_cpu.system >= previous_.process_cpu.system) {
                const std::uint64_t process_user = current.process_cpu.user - previous_.process_cpu.user;
                const std::uint64_t process_system = current.process_cpu.system - previous_.process_cpu.system;
                std::uint64_t process_total = process_user;
                if (checked_add(process_system, process_total) && process_total <= total) {
                    result.process_cpu_user_percent = percentage(process_user, total);
                    result.process_cpu_system_percent = percentage(process_system, total);
                    result.process_cpu_total_percent = percentage(process_total, total);
                    result.process_cpu_valid = true;
                }
            }
        }
    }

    if (has_previous_ && current.scheduler_valid && previous_.scheduler_valid &&
        current.context_switches >= previous_.context_switches && current.interrupts >= previous_.interrupts) {
        result.context_switches_delta = current.context_switches - previous_.context_switches;
        result.interrupts_delta = current.interrupts - previous_.interrupts;
        result.scheduler_delta_valid = true;
    }

    previous_ = current;
    has_previous_ = true;
    return result;
}

} // namespace fiber::cat::detail
