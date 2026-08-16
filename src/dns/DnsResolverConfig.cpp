#include <fiber/dns/DnsResolverConfig.h>

#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>
#include <fiber/net/IpAddress.h>

namespace fiber::dns {

namespace {

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

std::string_view next_token(std::string_view &text) noexcept {
    text = trim(text);
    if (text.empty()) {
        return {};
    }
    std::size_t end = 0;
    while (end < text.size() && text[end] != ' ' && text[end] != '\t' && text[end] != '\r') {
        ++end;
    }
    const std::string_view token = text.substr(0, end);
    text.remove_prefix(end);
    return token;
}

bool parse_uint(std::string_view text, unsigned min_value, unsigned max_value, unsigned &out) noexcept {
    if (text.empty()) {
        return false;
    }
    unsigned value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value < min_value || value > max_value) {
        return false;
    }
    out = value;
    return true;
}

bool option_value(std::string_view option, std::string_view name, std::string_view &value) noexcept {
    if (option.size() <= name.size() || option.substr(0, name.size()) != name || option[name.size()] != ':') {
        return false;
    }
    value = option.substr(name.size() + 1);
    return true;
}

ResolverConfigError make_error(ResolverConfigErrorCode code, std::size_t line, std::size_t column = 1,
                               int system_error = 0) noexcept {
    return ResolverConfigError{.code = code, .line = line, .column = column, .system_error = system_error};
}

void mark_unsupported(SystemResolverConfig &config, ResolverUnsupportedFeature feature, std::size_t line) noexcept {
    config.unsupported |= feature;
    if (config.first_unsupported_line == 0) {
        config.first_unsupported_line = line;
    }
}

} // namespace

class ResolverConfigParser {
public:
    explicit ResolverConfigParser(std::string_view text) noexcept : remaining_(text) {}

    std::expected<SystemResolverConfig, ResolverConfigError> parse() noexcept {
        while (!remaining_.empty()) {
            ++line_number_;
            const std::size_t newline = remaining_.find('\n');
            std::string_view line = remaining_.substr(0, newline);
            if (newline == std::string_view::npos) {
                remaining_ = {};
            } else {
                remaining_.remove_prefix(newline + 1);
            }

            const std::size_t comment = line.find_first_of("#;");
            if (comment != std::string_view::npos) {
                line = line.substr(0, comment);
            }
            line = trim(line);
            if (line.empty()) {
                continue;
            }

            std::string_view arguments = line;
            const std::string_view directive = next_token(arguments);
            auto result = parse_directive(directive, arguments);
            if (!result) {
                return std::unexpected(result.error());
            }
        }

        if (config_.nameservers.empty()) {
            return std::unexpected(make_error(ResolverConfigErrorCode::NoNameserver, line_number_));
        }
        return config_;
    }

private:
    std::expected<void, ResolverConfigError> parse_directive(std::string_view directive,
                                                             std::string_view arguments) noexcept {
        if (directive == "nameserver") {
            return parse_nameserver(arguments);
        }
        if (directive == "search" || directive == "domain") {
            return parse_search(arguments, directive == "domain");
        }
        if (directive == "options") {
            return parse_options(arguments);
        }
        if (directive == "sortlist") {
            mark_unsupported(config_, ResolverUnsupportedFeature::SortList, line_number_);
            return {};
        }

        mark_unsupported(config_, ResolverUnsupportedFeature::Directive, line_number_);
        return {};
    }

    std::expected<void, ResolverConfigError> parse_nameserver(std::string_view arguments) noexcept {
        const std::string_view address_text = next_token(arguments);
        if (address_text.empty() || !trim(arguments).empty()) {
            return std::unexpected(make_error(ResolverConfigErrorCode::InvalidDirective, line_number_));
        }
        net::IpAddress address;
        if (!net::IpAddress::parse(address_text, address) || address.is_unspecified() || address.is_multicast()) {
            return std::unexpected(make_error(ResolverConfigErrorCode::InvalidNameserver, line_number_));
        }
        if (!config_.nameservers.add(net::SocketAddress(address, 53))) {
            return std::unexpected(make_error(ResolverConfigErrorCode::TooManyNameservers, line_number_));
        }
        return {};
    }

    std::expected<void, ResolverConfigError> parse_search(std::string_view arguments, bool domain) noexcept {
        config_.search.clear();
        std::size_t count = 0;
        while (true) {
            const std::string_view value = next_token(arguments);
            if (value.empty()) {
                break;
            }
            if (!config_.search.add(value)) {
                return std::unexpected(make_error(ResolverConfigErrorCode::SearchListTooLarge, line_number_));
            }
            ++count;
            if (domain && count > 1) {
                return std::unexpected(make_error(ResolverConfigErrorCode::InvalidDirective, line_number_));
            }
        }
        if (count == 0) {
            return std::unexpected(make_error(ResolverConfigErrorCode::InvalidDirective, line_number_));
        }
        mark_unsupported(config_, ResolverUnsupportedFeature::Search, line_number_);
        return {};
    }

    std::expected<void, ResolverConfigError> parse_options(std::string_view arguments) noexcept {
        while (true) {
            const std::string_view option = next_token(arguments);
            if (option.empty()) {
                break;
            }
            if (option == "rotate") {
                config_.rotate = true;
                continue;
            }

            std::string_view value;
            unsigned parsed = 0;
            if (option_value(option, "timeout", value)) {
                if (!parse_uint(value, 1, 30, parsed)) {
                    return std::unexpected(make_error(ResolverConfigErrorCode::InvalidOption, line_number_));
                }
                config_.timeout = std::chrono::seconds(parsed);
                continue;
            }
            if (option_value(option, "attempts", value)) {
                if (!parse_uint(value, 1, 5, parsed)) {
                    return std::unexpected(make_error(ResolverConfigErrorCode::InvalidOption, line_number_));
                }
                config_.attempts = static_cast<std::uint8_t>(parsed);
                continue;
            }
            if (option_value(option, "ndots", value)) {
                if (!parse_uint(value, 0, 15, parsed)) {
                    return std::unexpected(make_error(ResolverConfigErrorCode::InvalidOption, line_number_));
                }
                config_.ndots = static_cast<std::uint8_t>(parsed);
                mark_unsupported(config_, ResolverUnsupportedFeature::Ndots, line_number_);
                continue;
            }

            mark_unsupported(config_, ResolverUnsupportedFeature::Option, line_number_);
        }
        return {};
    }

    std::string_view remaining_{};
    SystemResolverConfig config_{};
    std::size_t line_number_ = 0;
};

bool DnsNameserverList::add(const net::SocketAddress &address) noexcept {
    if (count_ >= entries_.size()) {
        return false;
    }
    entries_[count_++] = address;
    return true;
}

const net::SocketAddress &DnsNameserverList::operator[](std::size_t index) const noexcept {
    FIBER_ASSERT(index < count_);
    return entries_[index];
}

void DnsSearchList::clear() noexcept {
    storage_size_ = 0;
    count_ = 0;
}

std::string_view DnsSearchList::operator[](std::size_t index) const noexcept {
    FIBER_ASSERT(index < count_);
    return std::string_view(storage_.data() + offsets_[index], lengths_[index]);
}

bool DnsSearchList::add(std::string_view domain) noexcept {
    if (domain.empty() || domain.size() > 255 || count_ >= offsets_.size() ||
        domain.size() > storage_.size() - storage_size_) {
        return false;
    }
    offsets_[count_] = storage_size_;
    lengths_[count_] = static_cast<std::uint16_t>(domain.size());
    std::memcpy(storage_.data() + storage_size_, domain.data(), domain.size());
    storage_size_ = static_cast<std::uint16_t>(storage_size_ + domain.size());
    ++count_;
    return true;
}

std::string_view resolver_config_error_name(ResolverConfigErrorCode code) noexcept {
    switch (code) {
        case ResolverConfigErrorCode::InvalidArgument:
            return "invalid argument";
        case ResolverConfigErrorCode::CalledFromEventLoop:
            return "resolver configuration load attempted on an EventLoop";
        case ResolverConfigErrorCode::OpenFailed:
            return "resolver configuration open failed";
        case ResolverConfigErrorCode::ReadFailed:
            return "resolver configuration read failed";
        case ResolverConfigErrorCode::FileTooLarge:
            return "resolver configuration file is too large";
        case ResolverConfigErrorCode::NoNameserver:
            return "resolver configuration has no nameserver";
        case ResolverConfigErrorCode::TooManyNameservers:
            return "resolver configuration has too many nameservers";
        case ResolverConfigErrorCode::InvalidNameserver:
            return "resolver configuration has an invalid nameserver";
        case ResolverConfigErrorCode::InvalidDirective:
            return "resolver configuration has an invalid directive";
        case ResolverConfigErrorCode::InvalidOption:
            return "resolver configuration has an invalid option";
        case ResolverConfigErrorCode::SearchListTooLarge:
            return "resolver configuration search list exceeds its limit";
    }
    return "unknown resolver configuration error";
}

std::expected<SystemResolverConfig, ResolverConfigError> parse_resolver_config(std::string_view text) noexcept {
    return ResolverConfigParser(text).parse();
}

std::expected<SystemResolverConfig, ResolverConfigError> load_system_resolver_config(const char *path) noexcept {
    if (path == nullptr || path[0] == '\0') {
        return std::unexpected(make_error(ResolverConfigErrorCode::InvalidArgument, 0));
    }
    if (event::EventLoop::current_or_null() != nullptr) {
        return std::unexpected(make_error(ResolverConfigErrorCode::CalledFromEventLoop, 0));
    }

    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return std::unexpected(make_error(ResolverConfigErrorCode::OpenFailed, 0, 0, errno));
    }

    struct stat stat_buf{};
    if (::fstat(fd, &stat_buf) != 0) {
        const int error = errno;
        ::close(fd);
        return std::unexpected(make_error(ResolverConfigErrorCode::ReadFailed, 0, 0, error));
    }
    if (stat_buf.st_size > static_cast<off_t>(kMaxResolverConfigFileSize)) {
        ::close(fd);
        return std::unexpected(make_error(ResolverConfigErrorCode::FileTooLarge, 0));
    }

    auto buffer = std::unique_ptr<char[]>(new (std::nothrow) char[kMaxResolverConfigFileSize]);
    if (!buffer) {
        ::close(fd);
        return std::unexpected(make_error(ResolverConfigErrorCode::ReadFailed, 0, 0, ENOMEM));
    }

    std::size_t size = 0;
    while (size < kMaxResolverConfigFileSize) {
        const ssize_t n = ::read(fd, buffer.get() + size, kMaxResolverConfigFileSize - size);
        if (n > 0) {
            size += static_cast<std::size_t>(n);
            continue;
        }
        if (n == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        const int error = errno;
        ::close(fd);
        return std::unexpected(make_error(ResolverConfigErrorCode::ReadFailed, 0, 0, error));
    }

    if (size == kMaxResolverConfigFileSize) {
        char extra = 0;
        ssize_t n = 0;
        do {
            n = ::read(fd, &extra, 1);
        } while (n < 0 && errno == EINTR);
        if (n != 0) {
            const int error = n < 0 ? errno : 0;
            ::close(fd);
            if (n < 0) {
                return std::unexpected(make_error(ResolverConfigErrorCode::ReadFailed, 0, 0, error));
            }
            return std::unexpected(make_error(ResolverConfigErrorCode::FileTooLarge, 0));
        }
    }
    ::close(fd);
    return parse_resolver_config(std::string_view(buffer.get(), size));
}

} // namespace fiber::dns
