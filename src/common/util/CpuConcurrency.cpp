#include <fiber/common/util/CpuConcurrency.h>

#include <fiber/common/util/detail/CpuConcurrencyProbe.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string_view>
#include <thread>

#ifdef __linux__
#include <sched.h>
#endif

namespace fiber::util {
namespace {

constexpr std::size_t kPathCapacity = 4096;

enum class CgroupVersion : std::uint8_t {
    None,
    V1,
    V2,
};

enum class ReadStatus : std::uint8_t {
    Ok,
    Missing,
    Failed,
};

struct CgroupMembership {
    CgroupVersion version = CgroupVersion::None;
    std::array<char, kPathCapacity> path{};
};

struct CgroupMount {
    std::array<char, kPathCapacity> root{};
    std::array<char, kPathCapacity> point{};
    std::size_t score = 0;
    bool found = false;
};

struct CpuQuota {
    std::uint64_t quota_us = 0;
    std::uint64_t period_us = 0;
    CgroupVersion version = CgroupVersion::None;
};

struct CgroupQuotaProbe {
    CpuQuota quota;
    bool limited = false;
    bool failed = false;
};

bool copy_text(std::string_view input, char *output, std::size_t capacity) noexcept {
    if (input.size() >= capacity) {
        return false;
    }
    std::memcpy(output, input.data(), input.size());
    output[input.size()] = '\0';
    return true;
}

std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '\t' || value.front() == '\r' || value.front() == '\n')) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n')) {
        value.remove_suffix(1);
    }
    return value;
}

template<typename T>
bool parse_unsigned(std::string_view input, T &output) noexcept {
    input = trim(input);
    if (input.empty()) {
        return false;
    }
    T value = 0;
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size()) {
        return false;
    }
    output = value;
    return true;
}

bool list_contains(std::string_view list, std::string_view expected) noexcept {
    while (true) {
        const std::size_t comma = list.find(',');
        if (list.substr(0, comma) == expected) {
            return true;
        }
        if (comma == std::string_view::npos) {
            return false;
        }
        list.remove_prefix(comma + 1);
    }
}

bool path_has_prefix(std::string_view path, std::string_view prefix) noexcept {
    if (prefix == "/") {
        return path.starts_with('/');
    }
    return path == prefix || (path.size() > prefix.size() && path.starts_with(prefix) && path[prefix.size()] == '/');
}

bool decode_mount_path(std::string_view encoded, char *output, std::size_t capacity) noexcept {
    std::size_t output_size = 0;
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        char ch = encoded[i];
        if (ch == '\\' && i + 3 < encoded.size() && encoded[i + 1] >= '0' && encoded[i + 1] <= '7' &&
            encoded[i + 2] >= '0' && encoded[i + 2] <= '7' && encoded[i + 3] >= '0' && encoded[i + 3] <= '7') {
            ch = static_cast<char>((encoded[i + 1] - '0') * 64 + (encoded[i + 2] - '0') * 8 + (encoded[i + 3] - '0'));
            i += 3;
        }
        if (output_size + 1 >= capacity) {
            return false;
        }
        output[output_size++] = ch;
    }
    output[output_size] = '\0';
    return true;
}

char *next_token(char *&cursor) noexcept {
    while (*cursor == ' ' || *cursor == '\t') {
        ++cursor;
    }
    if (*cursor == '\0') {
        return nullptr;
    }
    char *token = cursor;
    while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' && *cursor != '\r' && *cursor != '\n') {
        ++cursor;
    }
    if (*cursor != '\0') {
        *cursor++ = '\0';
    }
    return token;
}

ReadStatus read_first_line(const char *path, char *output, std::size_t capacity) noexcept {
    errno = 0;
    FILE *file = std::fopen(path, "r");
    if (!file) {
        return errno == ENOENT || errno == ENOTDIR ? ReadStatus::Missing : ReadStatus::Failed;
    }
    const bool read = std::fgets(output, static_cast<int>(capacity), file) != nullptr;
    const bool failed = std::ferror(file) != 0;
    const bool complete = !read || std::strchr(output, '\n') != nullptr || std::feof(file) != 0;
    std::fclose(file);
    if (!read || failed || !complete) {
        return ReadStatus::Failed;
    }
    return ReadStatus::Ok;
}

bool parse_cgroup_membership_line(char *line, CgroupMembership &v1, CgroupMembership &v2) noexcept {
    char *first_colon = std::strchr(line, ':');
    if (!first_colon) {
        return false;
    }
    char *second_colon = std::strchr(first_colon + 1, ':');
    if (!second_colon) {
        return false;
    }
    *first_colon = '\0';
    *second_colon = '\0';
    std::string_view controllers(first_colon + 1);
    const std::string_view path = trim(second_colon + 1);
    if (path.empty() || path.front() != '/') {
        return false;
    }
    if (controllers.empty() && std::string_view(line) == "0") {
        v2.version = CgroupVersion::V2;
        return copy_text(path, v2.path.data(), v2.path.size());
    }
    if (list_contains(controllers, "cpu")) {
        v1.version = CgroupVersion::V1;
        return copy_text(path, v1.path.data(), v1.path.size());
    }
    return true;
}

bool read_cgroup_membership(const char *path, CgroupMembership &membership, bool &failed) noexcept {
    errno = 0;
    FILE *file = std::fopen(path, "r");
    if (!file) {
        failed = errno != ENOENT;
        return false;
    }
    CgroupMembership v1;
    CgroupMembership v2;
    char *line = nullptr;
    std::size_t capacity = 0;
    bool valid = true;
    while (::getline(&line, &capacity, file) >= 0) {
        valid = parse_cgroup_membership_line(line, v1, v2) && valid;
    }
    failed = std::ferror(file) != 0 || !valid;
    std::free(line);
    std::fclose(file);
    if (v1.version != CgroupVersion::None) {
        membership = v1;
        return true;
    }
    if (v2.version != CgroupVersion::None) {
        membership = v2;
        return true;
    }
    return false;
}

std::size_t mount_match_score(std::string_view membership, std::string_view root) noexcept {
    if (path_has_prefix(membership, root)) {
        return root.size() + 1;
    }
    return membership.starts_with('/') ? 1 : 0;
}

bool parse_mountinfo_line(char *line, const CgroupMembership &membership, CgroupMount &best) noexcept {
    char *separator = std::strstr(line, " - ");
    if (!separator) {
        return false;
    }
    *separator = '\0';
    char *left = line;
    char *fields[5]{};
    for (char *&field: fields) {
        field = next_token(left);
        if (!field) {
            return false;
        }
    }
    char *right = separator + 3;
    const char *filesystem = next_token(right);
    const char *source = next_token(right);
    const char *super_options = next_token(right);
    if (!filesystem || !source || !super_options) {
        return false;
    }
    const bool matching_v2 = membership.version == CgroupVersion::V2 && std::string_view(filesystem) == "cgroup2";
    const bool matching_v1 = membership.version == CgroupVersion::V1 && std::string_view(filesystem) == "cgroup" &&
                             (list_contains(source, "cpu") || list_contains(super_options, "cpu"));
    if (!matching_v1 && !matching_v2) {
        return true;
    }

    std::array<char, kPathCapacity> root{};
    std::array<char, kPathCapacity> point{};
    if (!decode_mount_path(fields[3], root.data(), root.size()) ||
        !decode_mount_path(fields[4], point.data(), point.size())) {
        return false;
    }
    const std::size_t score = mount_match_score(membership.path.data(), root.data());
    if (score == 0 || (best.found && score <= best.score)) {
        return true;
    }
    best.root = root;
    best.point = point;
    best.score = score;
    best.found = true;
    return true;
}

bool read_cgroup_mount(const char *path, const CgroupMembership &membership, CgroupMount &mount,
                       bool &failed) noexcept {
    errno = 0;
    FILE *file = std::fopen(path, "r");
    if (!file) {
        failed = errno != ENOENT;
        return false;
    }
    char *line = nullptr;
    std::size_t capacity = 0;
    bool valid = true;
    while (::getline(&line, &capacity, file) >= 0) {
        valid = parse_mountinfo_line(line, membership, mount) && valid;
    }
    failed = std::ferror(file) != 0 || !valid;
    std::free(line);
    std::fclose(file);
    return mount.found;
}

bool build_cgroup_path(const CgroupMembership &membership, const CgroupMount &mount, char *output,
                       std::size_t capacity) noexcept {
    const std::string_view member_path(membership.path.data());
    const std::string_view mount_root(mount.root.data());
    std::string_view relative = member_path;
    if (mount_root != "/" && path_has_prefix(member_path, mount_root)) {
        relative.remove_prefix(mount_root.size());
    }
    while (relative.starts_with('/')) {
        relative.remove_prefix(1);
    }
    const std::string_view mount_point(mount.point.data());
    const int written =
            relative.empty()
                    ? std::snprintf(output, capacity, "%.*s", static_cast<int>(mount_point.size()), mount_point.data())
                    : std::snprintf(output, capacity, "%.*s/%.*s", static_cast<int>(mount_point.size()),
                                    mount_point.data(), static_cast<int>(relative.size()), relative.data());
    return written >= 0 && static_cast<std::size_t>(written) < capacity;
}

bool parse_v2_cpu_max(std::string_view input, CpuQuota &quota, bool &limited) noexcept {
    input = trim(input);
    const std::size_t separator = input.find_first_of(" \t");
    if (separator == std::string_view::npos) {
        return false;
    }
    const std::string_view quota_text = input.substr(0, separator);
    const std::string_view period_text = trim(input.substr(separator + 1));
    std::uint64_t period = 0;
    if (!parse_unsigned(period_text, period) || period == 0) {
        return false;
    }
    if (quota_text == "max") {
        limited = false;
        return true;
    }
    std::uint64_t quota_value = 0;
    if (!parse_unsigned(quota_text, quota_value) || quota_value == 0) {
        return false;
    }
    quota = CpuQuota{
            .quota_us = quota_value,
            .period_us = period,
            .version = CgroupVersion::V2,
    };
    limited = true;
    return true;
}

bool parse_v1_cpu_max(std::string_view quota_text, std::string_view period_text, CpuQuota &quota,
                      bool &limited) noexcept {
    quota_text = trim(quota_text);
    period_text = trim(period_text);
    if (quota_text == "-1") {
        std::uint64_t period = 0;
        limited = false;
        return parse_unsigned(period_text, period) && period > 0;
    }
    std::uint64_t quota_value = 0;
    std::uint64_t period = 0;
    if (!parse_unsigned(quota_text, quota_value) || quota_value == 0 || !parse_unsigned(period_text, period) ||
        period == 0) {
        return false;
    }
    quota = CpuQuota{
            .quota_us = quota_value,
            .period_us = period,
            .version = CgroupVersion::V1,
    };
    limited = true;
    return true;
}

bool quota_is_tighter(const CpuQuota &candidate, const CpuQuota &current) noexcept {
    using Wide = unsigned __int128;
    return static_cast<Wide>(candidate.quota_us) * current.period_us <
           static_cast<Wide>(current.quota_us) * candidate.period_us;
}

void consider_quota(CgroupQuotaProbe &probe, const CpuQuota &quota) noexcept {
    if (!probe.limited || quota_is_tighter(quota, probe.quota)) {
        probe.quota = quota;
        probe.limited = true;
    }
}

void probe_v2_path(const char *path, CgroupQuotaProbe &probe) noexcept {
    std::array<char, kPathCapacity> file_path{};
    const int written = std::snprintf(file_path.data(), file_path.size(), "%s/cpu.max", path);
    if (written < 0 || static_cast<std::size_t>(written) >= file_path.size()) {
        probe.failed = true;
        return;
    }
    std::array<char, 256> line{};
    const ReadStatus status = read_first_line(file_path.data(), line.data(), line.size());
    if (status == ReadStatus::Missing) {
        return;
    }
    if (status == ReadStatus::Failed) {
        probe.failed = true;
        return;
    }
    CpuQuota quota;
    bool limited = false;
    if (!parse_v2_cpu_max(line.data(), quota, limited)) {
        probe.failed = true;
    } else if (limited) {
        consider_quota(probe, quota);
    }
}

void probe_v1_path(const char *path, CgroupQuotaProbe &probe) noexcept {
    std::array<char, kPathCapacity> quota_path{};
    std::array<char, kPathCapacity> period_path{};
    const int quota_written = std::snprintf(quota_path.data(), quota_path.size(), "%s/cpu.cfs_quota_us", path);
    const int period_written = std::snprintf(period_path.data(), period_path.size(), "%s/cpu.cfs_period_us", path);
    if (quota_written < 0 || static_cast<std::size_t>(quota_written) >= quota_path.size() || period_written < 0 ||
        static_cast<std::size_t>(period_written) >= period_path.size()) {
        probe.failed = true;
        return;
    }
    std::array<char, 256> quota_line{};
    std::array<char, 256> period_line{};
    const ReadStatus quota_status = read_first_line(quota_path.data(), quota_line.data(), quota_line.size());
    const ReadStatus period_status = read_first_line(period_path.data(), period_line.data(), period_line.size());
    if (quota_status == ReadStatus::Missing && period_status == ReadStatus::Missing) {
        return;
    }
    if (quota_status != ReadStatus::Ok || period_status != ReadStatus::Ok) {
        probe.failed = true;
        return;
    }
    CpuQuota quota;
    bool limited = false;
    if (!parse_v1_cpu_max(quota_line.data(), period_line.data(), quota, limited)) {
        probe.failed = true;
    } else if (limited) {
        consider_quota(probe, quota);
    }
}

CgroupQuotaProbe detect_cgroup_quota(const detail::CpuConcurrencyProbeOptions &options) noexcept {
    CgroupQuotaProbe probe;
    if (!options.proc_self_cgroup_path || !options.proc_self_mountinfo_path) {
        probe.failed = true;
        return probe;
    }
    CgroupMembership membership;
    bool membership_failed = false;
    if (!read_cgroup_membership(options.proc_self_cgroup_path, membership, membership_failed)) {
        probe.failed = membership_failed;
        return probe;
    }
    CgroupMount mount;
    bool mount_failed = false;
    if (!read_cgroup_mount(options.proc_self_mountinfo_path, membership, mount, mount_failed)) {
        probe.failed = mount_failed;
        return probe;
    }
    std::array<char, kPathCapacity> current{};
    if (!build_cgroup_path(membership, mount, current.data(), current.size())) {
        probe.failed = true;
        return probe;
    }
    const std::string_view mount_point(mount.point.data());
    if (!path_has_prefix(current.data(), mount_point)) {
        probe.failed = true;
        return probe;
    }
    while (true) {
        if (membership.version == CgroupVersion::V2) {
            probe_v2_path(current.data(), probe);
        } else {
            probe_v1_path(current.data(), probe);
        }
        if (std::string_view(current.data()) == mount_point) {
            break;
        }
        char *slash = std::strrchr(current.data(), '/');
        if (mount_point == "/" && slash == current.data()) {
            current[1] = '\0';
            continue;
        }
        if (!slash || slash < current.data() + mount_point.size()) {
            probe.failed = true;
            break;
        }
        *slash = '\0';
    }
    return probe;
}

std::size_t quota_cpu_count(const CpuQuota &quota) noexcept {
    const std::uint64_t whole = quota.quota_us / quota.period_us;
    const std::uint64_t rounded = whole + (quota.quota_us % quota.period_us != 0 ? 1 : 0);
    if (rounded > std::numeric_limits<std::size_t>::max()) {
        return std::numeric_limits<std::size_t>::max();
    }
    return std::max<std::size_t>(1, static_cast<std::size_t>(rounded));
}

std::size_t detect_affinity_count() noexcept {
#ifdef __linux__
    std::size_t cpu_capacity = CPU_SETSIZE;
    while (cpu_capacity <= (1U << 20U)) {
        const std::size_t allocation_size = CPU_ALLOC_SIZE(cpu_capacity);
        cpu_set_t *affinity = CPU_ALLOC(cpu_capacity);
        if (!affinity) {
            return 0;
        }
        CPU_ZERO_S(allocation_size, affinity);
        if (::sched_getaffinity(0, allocation_size, affinity) == 0) {
            const int count = CPU_COUNT_S(allocation_size, affinity);
            CPU_FREE(affinity);
            return count > 0 ? static_cast<std::size_t>(count) : 0;
        }
        const int error = errno;
        CPU_FREE(affinity);
        if (error != EINVAL) {
            return 0;
        }
        cpu_capacity *= 2;
    }
#endif
    return 0;
}

} // namespace

namespace detail {

CpuConcurrency detect_cpu_concurrency(const CpuConcurrencyProbeOptions &options) noexcept {
    CpuConcurrency result;
    const std::size_t affinity_count =
            options.use_supplied_cpu_counts ? options.affinity_count : detect_affinity_count();
    const std::size_t hardware_count = options.use_supplied_cpu_counts
                                               ? options.hardware_concurrency
                                               : static_cast<std::size_t>(std::thread::hardware_concurrency());
    result.affinity_count = affinity_count;
    if (affinity_count > 0) {
        result.effective_count = affinity_count;
        result.source = CpuConcurrencySource::Affinity;
    } else if (hardware_count > 0) {
        result.effective_count = hardware_count;
        result.source = CpuConcurrencySource::HardwareConcurrency;
    }

#ifdef __linux__
    const CgroupQuotaProbe quota = detect_cgroup_quota(options);
    result.cgroup_probe_failed = quota.failed;
    if (quota.limited) {
        result.quota_count = quota_cpu_count(quota.quota);
        result.quota_us = quota.quota.quota_us;
        result.period_us = quota.quota.period_us;
        if (result.quota_count <= result.effective_count) {
            result.effective_count = result.quota_count;
            result.source = quota.quota.version == CgroupVersion::V2 ? CpuConcurrencySource::CgroupV2Quota
                                                                     : CpuConcurrencySource::CgroupV1Quota;
        }
    }
#endif
    return result;
}

} // namespace detail

CpuConcurrency detect_cpu_concurrency() noexcept { return detail::detect_cpu_concurrency({}); }

std::string_view cpu_concurrency_source_name(CpuConcurrencySource source) noexcept {
    switch (source) {
        case CpuConcurrencySource::Fallback:
            return "fallback";
        case CpuConcurrencySource::HardwareConcurrency:
            return "hardware_concurrency";
        case CpuConcurrencySource::Affinity:
            return "affinity";
        case CpuConcurrencySource::CgroupV1Quota:
            return "cgroup_v1_quota";
        case CpuConcurrencySource::CgroupV2Quota:
            return "cgroup_v2_quota";
    }
    return "unknown";
}

} // namespace fiber::util
