#include "ProcessMetrics.h"

#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <limits>
#include <string_view>

#include <dirent.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

namespace fiber::ai_server {
namespace {

constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000;

[[nodiscard]] bool is_space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r';
}

[[nodiscard]] std::optional<std::string_view> next_token(std::string_view &input) noexcept {
    while (!input.empty() && is_space(input.front())) {
        input.remove_prefix(1);
    }
    if (input.empty()) {
        return std::nullopt;
    }
    std::size_t length = 0;
    while (length < input.size() && !is_space(input[length])) {
        ++length;
    }
    const std::string_view token = input.substr(0, length);
    input.remove_prefix(length);
    return token;
}

[[nodiscard]] std::optional<std::uint64_t> parse_uint(std::string_view value) noexcept {
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

template<std::size_t Size>
[[nodiscard]] std::optional<std::string_view> read_small_file(const char *path,
                                                              std::array<char, Size> &buffer) noexcept {
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return std::nullopt;
    }

    std::size_t size = 0;
    while (size < buffer.size()) {
        const ssize_t read_size = ::read(fd, buffer.data() + size, buffer.size() - size);
        if (read_size > 0) {
            size += static_cast<std::size_t>(read_size);
            continue;
        }
        if (read_size < 0 && errno == EINTR) {
            continue;
        }
        if (read_size < 0) {
            size = 0;
        }
        break;
    }
    const int close_result = ::close(fd);
    if (size == 0 || size == buffer.size() || close_result != 0) {
        return std::nullopt;
    }
    return std::string_view(buffer.data(), size);
}

void collect_process_memory(ProcessMetricsSnapshot &snapshot) noexcept {
    std::array<char, 128> buffer{};
    auto contents = read_small_file("/proc/self/statm", buffer);
    const long page_size = ::sysconf(_SC_PAGESIZE);
    if (!contents || page_size <= 0) {
        return;
    }

    auto virtual_pages_token = next_token(*contents);
    auto resident_pages_token = next_token(*contents);
    if (!virtual_pages_token || !resident_pages_token) {
        return;
    }
    auto virtual_pages = parse_uint(*virtual_pages_token);
    auto resident_pages = parse_uint(*resident_pages_token);
    const std::uint64_t page_bytes = static_cast<std::uint64_t>(page_size);
    if (!virtual_pages || !resident_pages || *virtual_pages > std::numeric_limits<std::uint64_t>::max() / page_bytes ||
        *resident_pages > std::numeric_limits<std::uint64_t>::max() / page_bytes) {
        return;
    }
    snapshot.virtual_memory_bytes = *virtual_pages * page_bytes;
    snapshot.resident_memory_bytes = *resident_pages * page_bytes;
}

[[nodiscard]] std::optional<std::uint64_t> read_process_start_ticks() noexcept {
    std::array<char, 1024> buffer{};
    auto contents = read_small_file("/proc/self/stat", buffer);
    if (!contents) {
        return std::nullopt;
    }
    const std::size_t command_end = contents->rfind(')');
    if (command_end == std::string_view::npos) {
        return std::nullopt;
    }

    std::string_view fields = contents->substr(command_end + 1);
    for (std::size_t field = 3; field <= 22; ++field) {
        auto token = next_token(fields);
        if (!token) {
            return std::nullopt;
        }
        if (field == 22) {
            return parse_uint(*token);
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::uint64_t> read_boot_time_seconds() noexcept {
    std::FILE *file = std::fopen("/proc/stat", "r");
    if (!file) {
        return std::nullopt;
    }

    std::optional<std::uint64_t> boot_time;
    std::array<char, 256> line{};
    while (std::fgets(line.data(), static_cast<int>(line.size()), file)) {
        std::string_view value(line.data());
        if (!value.starts_with("btime ")) {
            continue;
        }
        value.remove_prefix(6);
        while (!value.empty() && is_space(value.back())) {
            value.remove_suffix(1);
        }
        boot_time = parse_uint(value);
        break;
    }
    if (std::fclose(file) != 0) {
        return std::nullopt;
    }
    return boot_time;
}

void collect_process_cpu_and_start_time(ProcessMetricsSnapshot &snapshot) noexcept {
    timespec cpu_time{};
    if (::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_time) == 0 && cpu_time.tv_sec >= 0 && cpu_time.tv_nsec >= 0 &&
        cpu_time.tv_nsec < static_cast<long>(kNanosecondsPerSecond)) {
        const auto seconds = static_cast<std::uint64_t>(cpu_time.tv_sec);
        if (seconds <= std::numeric_limits<std::uint64_t>::max() / kNanosecondsPerSecond) {
            snapshot.cpu_time_nanoseconds =
                    seconds * kNanosecondsPerSecond + static_cast<std::uint64_t>(cpu_time.tv_nsec);
        }
    }

    const long clock_ticks = ::sysconf(_SC_CLK_TCK);
    auto start_ticks = read_process_start_ticks();
    auto boot_time = read_boot_time_seconds();
    if (clock_ticks <= 0 || !start_ticks || !boot_time) {
        return;
    }

    const std::uint64_t ticks_per_second = static_cast<std::uint64_t>(clock_ticks);
    const std::uint64_t start_seconds = *start_ticks / ticks_per_second;
    if (*boot_time > std::numeric_limits<std::uint64_t>::max() - start_seconds) {
        return;
    }
    const std::uint64_t epoch_seconds = *boot_time + start_seconds;
    if (epoch_seconds > std::numeric_limits<std::uint64_t>::max() / kNanosecondsPerSecond) {
        return;
    }
    const std::uint64_t remaining_ticks = *start_ticks % ticks_per_second;
    if (remaining_ticks > std::numeric_limits<std::uint64_t>::max() / kNanosecondsPerSecond) {
        return;
    }
    snapshot.start_time_nanoseconds =
            epoch_seconds * kNanosecondsPerSecond + remaining_ticks * kNanosecondsPerSecond / ticks_per_second;
}

void collect_process_fds(ProcessMetricsSnapshot &snapshot) noexcept {
    DIR *directory = ::opendir("/proc/self/fd");
    if (directory) {
        const int collection_fd = ::dirfd(directory);
        std::uint64_t count = 0;
        while (dirent *entry = ::readdir(directory)) {
            const std::string_view name(entry->d_name);
            auto fd = parse_uint(name);
            if (fd && (collection_fd < 0 || *fd != static_cast<std::uint64_t>(collection_fd))) {
                ++count;
            }
        }
        if (::closedir(directory) == 0) {
            snapshot.open_fds = count;
        }
    }

    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) == 0) {
        snapshot.max_fds = static_cast<std::uint64_t>(limit.rlim_cur);
    }
}

} // namespace

ProcessMetricsSnapshot collect_process_metrics() noexcept {
    ProcessMetricsSnapshot snapshot;
    collect_process_memory(snapshot);
    collect_process_cpu_and_start_time(snapshot);
    collect_process_fds(snapshot);
    return snapshot;
}

} // namespace fiber::ai_server
