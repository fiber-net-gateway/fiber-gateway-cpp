#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/event/EventLoop.h>
#include <fiber/http/Http1Server.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/net/detail/StreamFd.h>

extern char **environ;

namespace fs = std::filesystem;

namespace {

using fiber::common::IoErr;
using fiber::common::IoResult;

constexpr std::size_t kIoChunkSize = 16 * 1024;
constexpr std::size_t kMaxCgiHeaderBytes = 64 * 1024;
constexpr std::size_t kRequestBodyMemoryThreshold = 512 * 1024;

struct UniqueFd {
    int fd = -1;

    UniqueFd() = default;
    explicit UniqueFd(int value) : fd(value) {}

    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;

    UniqueFd(UniqueFd &&other) noexcept : fd(other.fd) { other.fd = -1; }

    UniqueFd &operator=(UniqueFd &&other) noexcept {
        if (this == &other) {
            return *this;
        }
        reset();
        fd = other.fd;
        other.fd = -1;
        return *this;
    }

    ~UniqueFd() { reset(); }

    [[nodiscard]] bool valid() const noexcept { return fd >= 0; }

    int release() noexcept {
        int value = fd;
        fd = -1;
        return value;
    }

    void reset(int value = -1) noexcept {
        if (fd >= 0) {
            ::close(fd);
        }
        fd = value;
    }
};

struct TempAliasRoot {
    fs::path path;

    TempAliasRoot() = default;
    TempAliasRoot(const TempAliasRoot &) = delete;
    TempAliasRoot &operator=(const TempAliasRoot &) = delete;

    TempAliasRoot(TempAliasRoot &&other) noexcept : path(std::move(other.path)) { other.path.clear(); }

    TempAliasRoot &operator=(TempAliasRoot &&other) noexcept {
        if (this == &other) {
            return *this;
        }
        if (!path.empty()) {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
        path = std::move(other.path);
        other.path.clear();
        return *this;
    }

    ~TempAliasRoot() {
        if (path.empty()) {
            return;
        }
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    [[nodiscard]] static std::optional<TempAliasRoot> create() {
        std::array<char, 64> templ{};
        const char *pattern = "/tmp/fiber-git-http-XXXXXX";
        std::copy(pattern, pattern + std::strlen(pattern), templ.begin());
        char *dir = ::mkdtemp(templ.data());
        if (!dir) {
            return std::nullopt;
        }
        TempAliasRoot root;
        root.path = dir;
        return root;
    }

    [[nodiscard]] bool ensure_repo_alias(std::string_view alias_name, const fs::path &target) const {
        if (path.empty()) {
            return false;
        }
        fs::path alias_path = path / std::string(alias_name);
        std::error_code ec;
        if (fs::exists(alias_path, ec)) {
            return !ec;
        }
        fs::create_directory_symlink(target, alias_path, ec);
        return !ec;
    }
};

struct RepoRoute {
    std::string repo_url_name;
    fs::path repo_target_path;
    std::string path_info;
};

struct BackendProcess {
    pid_t pid = -1;
    UniqueFd stdin_write;
    UniqueFd stdout_read;

    [[nodiscard]] bool valid() const noexcept { return pid > 0 && stdin_write.valid() && stdout_read.valid(); }
};

struct CgiResponse {
    int status_code = 200;
    std::optional<std::size_t> content_length;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<std::uint8_t> body_prefix;
};

struct RequestBodyStorage {
    std::size_t size = 0;
    std::vector<std::uint8_t> memory;
    UniqueFd temp_file;

    [[nodiscard]] bool uses_temp_file() const noexcept { return temp_file.valid(); }
};

struct ServerConfig {
    fs::path repositories_root;
    std::string backend_path;
    TempAliasRoot alias_root;
};

fiber::async::Task<IoResult<void>> send_final_header(fiber::http::HttpExchange &exchange, int status_code,
                                                     const fiber::http::HttpHeaders *headers,
                                                     fiber::http::HttpBodySpec body,
                                                     fiber::http::ResponseConnectionMode connection_mode,
                                                     bool end_stream) {
    co_return co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Final,
            .status_code = status_code,
            .headers = headers,
            .body = body,
            .connection_mode = connection_mode,
            .end_stream = end_stream,
    });
}

std::optional<std::uint16_t> parse_port(const char *text) {
    if (!text) {
        return std::nullopt;
    }
    char *end = nullptr;
    unsigned long value = std::strtoul(text, &end, 10);
    if (!end || *end != '\0' || value > 65535) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

IoResult<std::uint16_t> resolve_port(int fd) {
    sockaddr_storage bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    fiber::net::SocketAddress local;
    if (!fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<sockaddr *>(&bound), len, local)) {
        return std::unexpected(IoErr::NotSupported);
    }
    return local.port();
}

std::string trim_ascii(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

bool iequals_ascii(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        unsigned char a = static_cast<unsigned char>(lhs[i]);
        unsigned char b = static_cast<unsigned char>(rhs[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

bool contains_100_continue(std::string_view value) {
    std::string lower;
    lower.reserve(value.size());
    for (char ch: value) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return lower.find("100-continue") != std::string::npos;
}

std::optional<std::size_t> parse_size(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::size_t value = 0;
    for (char ch: text) {
        if (ch < '0' || ch > '9') {
            return std::nullopt;
        }
        std::size_t digit = static_cast<std::size_t>(ch - '0');
        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
            return std::nullopt;
        }
        value = value * 10 + digit;
    }
    return value;
}

bool is_valid_repo_name(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    for (char ch: name) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) != 0 || ch == '.' || ch == '_' || ch == '-') {
            continue;
        }
        return false;
    }
    return true;
}

std::optional<RepoRoute> resolve_repo_route(const fs::path &repositories_root, std::string_view request_path) {
    if (request_path.empty() || request_path.front() != '/') {
        return std::nullopt;
    }
    if (request_path.find("..") != std::string_view::npos || request_path.find('\\') != std::string_view::npos) {
        return std::nullopt;
    }

    std::string_view rest = request_path.substr(1);
    std::size_t slash = rest.find('/');
    std::string_view repo_url_name = slash == std::string_view::npos ? rest : rest.substr(0, slash);
    if (repo_url_name.size() <= 4 || !repo_url_name.ends_with(".git")) {
        return std::nullopt;
    }

    std::string_view repo_base_name = repo_url_name.substr(0, repo_url_name.size() - 4);
    if (!is_valid_repo_name(repo_base_name)) {
        return std::nullopt;
    }

    fs::path exact_path = repositories_root / std::string(repo_url_name);
    fs::path plain_path = repositories_root / std::string(repo_base_name);
    std::error_code ec;

    fs::path target;
    if (fs::is_directory(exact_path, ec)) {
        target = exact_path;
    } else {
        ec.clear();
        if (fs::is_directory(plain_path, ec)) {
            target = plain_path;
        } else {
            return std::nullopt;
        }
    }

    RepoRoute route;
    route.repo_url_name = std::string(repo_url_name);
    route.repo_target_path = std::move(target);
    route.path_info = std::string(request_path);
    return route;
}

std::string header_to_cgi_key(std::string_view lowcase_name) {
    std::string key;
    key.reserve(lowcase_name.size() + 5);
    key.append("HTTP_");
    for (char ch: lowcase_name) {
        if (ch == '-') {
            key.push_back('_');
        } else {
            key.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
        }
    }
    return key;
}

void set_env_entry(std::vector<std::string> &env, std::string key, std::string value) {
    std::string prefix = key;
    prefix.push_back('=');
    for (std::string &entry: env) {
        if (entry.size() >= prefix.size() && entry.compare(0, prefix.size(), prefix) == 0) {
            entry = std::move(prefix);
            entry.append(value);
            return;
        }
    }
    prefix.append(value);
    env.push_back(std::move(prefix));
}

std::string method_to_string(fiber::http::HttpMethod method) {
    switch (method) {
        case fiber::http::HttpMethod::Get:
            return "GET";
        case fiber::http::HttpMethod::Post:
            return "POST";
        case fiber::http::HttpMethod::Head:
            return "HEAD";
        default:
            return {};
    }
}

std::string extract_server_name(const fiber::http::HttpExchange &exchange) {
    std::string_view host = exchange.header("host");
    if (host.empty()) {
        return "localhost";
    }
    std::size_t colon = host.find(':');
    if (colon == std::string_view::npos) {
        return std::string(host);
    }
    return std::string(host.substr(0, colon));
}

std::vector<std::string> build_backend_env(const ServerConfig &config, const RepoRoute &route,
                                           fiber::http::HttpExchange &exchange, std::size_t body_size) {
    std::vector<std::string> env;
    for (char **current = environ; current && *current; ++current) {
        env.emplace_back(*current);
    }

    std::string method = method_to_string(exchange.method());
    std::string request_uri = route.path_info;
    if (!exchange.uri().query.empty()) {
        request_uri.push_back('?');
        request_uri.append(exchange.uri().query);
    }

    set_env_entry(env, "GIT_PROJECT_ROOT", config.alias_root.path.string());
    set_env_entry(env, "GIT_HTTP_EXPORT_ALL", "1");
    set_env_entry(env, "PATH_INFO", route.path_info);
    set_env_entry(env, "QUERY_STRING", std::string(exchange.uri().query));
    set_env_entry(env, "REQUEST_METHOD", method);
    set_env_entry(env, "REQUEST_URI", request_uri);
    set_env_entry(env, "CONTENT_TYPE", std::string(exchange.header("content-type")));
    set_env_entry(env, "CONTENT_LENGTH", std::to_string(body_size));
    set_env_entry(env, "SERVER_PROTOCOL",
                  exchange.version_view().empty() ? "HTTP/1.1" : std::string(exchange.version_view()));
    set_env_entry(env, "SERVER_SOFTWARE", "fiber-gateway-cpp-example");
    set_env_entry(env, "SERVER_NAME", extract_server_name(exchange));
    set_env_entry(env, "REQUEST_SCHEME", "http");
    set_env_entry(env, "REMOTE_ADDR", "127.0.0.1");
    set_env_entry(env, "REMOTE_USER", "fiber");
    set_env_entry(env, "AUTH_TYPE", "Basic");

    std::string_view git_protocol = exchange.header("git-protocol");
    if (!git_protocol.empty()) {
        set_env_entry(env, "GIT_PROTOCOL", std::string(git_protocol));
    }

    for (const auto &header: exchange.request_headers()) {
        if (iequals_ascii(header.lowcase_view(), "content-length") ||
            iequals_ascii(header.lowcase_view(), "content-type")) {
            continue;
        }
        set_env_entry(env, header_to_cgi_key(header.lowcase_view()), std::string(header.value_view()));
    }

    return env;
}

IoResult<void> make_pipe(UniqueFd &read_end, UniqueFd &write_end) {
    int fds[2] = {-1, -1};
    if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    read_end.reset(fds[0]);
    write_end.reset(fds[1]);
    return {};
}

IoResult<UniqueFd> create_temp_body_file() {
    char templ[] = "/tmp/fiber-git-body-XXXXXX";
    int fd = ::mkstemp(templ);
    if (fd < 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    (void) ::unlink(templ);
    int fd_flags = ::fcntl(fd, F_GETFD, 0);
    if (fd_flags >= 0) {
        (void) ::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
    }
    return UniqueFd(fd);
}

IoResult<void> write_fd_all(int fd, const std::uint8_t *data, std::size_t len) {
    std::size_t offset = 0;
    while (offset < len) {
        ssize_t rc = ::write(fd, data + offset, len - offset);
        if (rc > 0) {
            offset += static_cast<std::size_t>(rc);
            continue;
        }
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        return std::unexpected(rc < 0 ? fiber::common::io_err_from_errno(errno) : IoErr::BrokenPipe);
    }
    return {};
}

IoResult<void> spill_request_body_to_file(RequestBodyStorage &storage) {
    auto temp_file_result = create_temp_body_file();
    if (!temp_file_result) {
        return std::unexpected(temp_file_result.error());
    }
    storage.temp_file = std::move(*temp_file_result);
    if (!storage.memory.empty()) {
        auto write_result = write_fd_all(storage.temp_file.fd, storage.memory.data(), storage.memory.size());
        if (!write_result) {
            return std::unexpected(write_result.error());
        }
        storage.memory.clear();
        storage.memory.shrink_to_fit();
    }
    return {};
}

void clear_nonblock(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return;
    }
    (void) ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}

IoResult<BackendProcess> spawn_backend_process(const ServerConfig &config, const RepoRoute &route,
                                               fiber::http::HttpExchange &exchange, std::size_t body_size) {
    if (!config.alias_root.ensure_repo_alias(route.repo_url_name, route.repo_target_path)) {
        return std::unexpected(IoErr::Unknown);
    }

    std::vector<std::string> env = build_backend_env(config, route, exchange, body_size);

    UniqueFd child_stdin_read;
    UniqueFd child_stdin_write;
    UniqueFd child_stdout_read;
    UniqueFd child_stdout_write;

    if (auto pipe_result = make_pipe(child_stdin_read, child_stdin_write); !pipe_result) {
        return std::unexpected(pipe_result.error());
    }
    if (auto pipe_result = make_pipe(child_stdout_read, child_stdout_write); !pipe_result) {
        return std::unexpected(pipe_result.error());
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }

    if (pid == 0) {
        ::dup2(child_stdin_read.fd, STDIN_FILENO);
        ::dup2(child_stdout_write.fd, STDOUT_FILENO);
        clear_nonblock(STDIN_FILENO);
        clear_nonblock(STDOUT_FILENO);

        child_stdin_read.reset();
        child_stdin_write.reset();
        child_stdout_read.reset();
        child_stdout_write.reset();

        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(config.backend_path.c_str()));
        argv.push_back(nullptr);

        std::vector<char *> envp;
        envp.reserve(env.size() + 1);
        for (std::string &entry: env) {
            envp.push_back(entry.data());
        }
        envp.push_back(nullptr);

        ::execve(config.backend_path.c_str(), argv.data(), envp.data());
        ::_exit(127);
    }

    child_stdin_read.reset();
    child_stdout_write.reset();

    BackendProcess process;
    process.pid = pid;
    process.stdin_write = std::move(child_stdin_write);
    process.stdout_read = std::move(child_stdout_read);
    return process;
}

fiber::async::Task<IoResult<void>> write_all(fiber::net::detail::StreamFd &stream, const std::uint8_t *data,
                                             std::size_t len) {
    std::size_t offset = 0;
    while (offset < len) {
        iovec iov{
                .iov_base = const_cast<std::uint8_t *>(data + offset),
                .iov_len = len - offset,
        };
        auto write_result = co_await stream.writev(&iov, 1);
        if (!write_result) {
            co_return std::unexpected(write_result.error());
        }
        if (*write_result == 0) {
            co_return std::unexpected(IoErr::BrokenPipe);
        }
        offset += *write_result;
    }
    co_return IoResult<void>{};
}

fiber::async::Task<IoResult<std::size_t>> read_some(fiber::net::detail::StreamFd &stream, std::uint8_t *buf,
                                                    std::size_t len) {
    iovec iov{
            .iov_base = buf,
            .iov_len = len,
    };
    co_return co_await stream.readv(&iov, 1);
}

void append_chain_bytes(std::vector<std::uint8_t> &buffer, fiber::mem::IoBufChain &chain) {
    std::size_t total = chain.readable_bytes();
    if (total == 0) {
        return;
    }
    std::size_t base = buffer.size();
    buffer.resize(base + total);
    std::size_t copied = 0;
    while (auto *chunk = chain.front()) {
        std::size_t readable = chunk->readable();
        if (readable == 0) {
            chain.drop_empty_front();
            continue;
        }
        std::memcpy(buffer.data() + base + copied, chunk->readable_data(), readable);
        copied += readable;
        chain.consume_and_compact(readable);
    }
}

fiber::async::Task<IoResult<RequestBodyStorage>> collect_request_body(fiber::http::HttpExchange &exchange) {
    RequestBodyStorage body;

    std::string_view content_length = exchange.header("content-length");
    if (auto length = parse_size(trim_ascii(content_length)); length) {
        body.memory.reserve(std::min(*length, kRequestBodyMemoryThreshold));
    }

    for (;;) {
        auto read_result = co_await exchange.read_body(64 * 1024);
        if (!read_result) {
            co_return std::unexpected(read_result.error());
        }
        std::size_t chunk_bytes = read_result->readable_bytes();
        if (!body.uses_temp_file() && body.size + chunk_bytes > kRequestBodyMemoryThreshold) {
            auto spill_result = spill_request_body_to_file(body);
            if (!spill_result) {
                co_return std::unexpected(spill_result.error());
            }
        }
        if (body.uses_temp_file()) {
            std::vector<std::uint8_t> chunk;
            append_chain_bytes(chunk, *read_result);
            auto write_result = write_fd_all(body.temp_file.fd, chunk.data(), chunk.size());
            if (!write_result) {
                co_return std::unexpected(write_result.error());
            }
            body.size += chunk.size();
        } else {
            append_chain_bytes(body.memory, *read_result);
            body.size = body.memory.size();
        }
        if (read_result->complete()) {
            break;
        }
    }
    co_return body;
}

fiber::async::Task<IoResult<void>> stream_file_via_read(fiber::net::detail::StreamFd &stream, int file_fd,
                                                        std::size_t total_size) {
    std::array<std::uint8_t, kIoChunkSize> chunk{};
    std::size_t offset = 0;
    while (offset < total_size) {
        std::size_t to_read = std::min(chunk.size(), total_size - offset);
        ssize_t rc = ::pread(file_fd, chunk.data(), to_read, static_cast<off_t>(offset));
        if (rc > 0) {
            auto write_result = co_await write_all(stream, chunk.data(), static_cast<std::size_t>(rc));
            if (!write_result) {
                co_return std::unexpected(write_result.error());
            }
            offset += static_cast<std::size_t>(rc);
            continue;
        }
        if (rc == 0) {
            co_return std::unexpected(IoErr::BrokenPipe);
        }
        if (errno == EINTR) {
            continue;
        }
        co_return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    co_return IoResult<void>{};
}

fiber::async::Task<IoResult<void>> stream_file_to_backend(fiber::net::detail::StreamFd &stream, int file_fd,
                                                          std::size_t total_size) {
    off_t offset = 0;
    while (static_cast<std::size_t>(offset) < total_size) {
        std::size_t remaining = total_size - static_cast<std::size_t>(offset);
        ssize_t rc = ::sendfile(stream.fd(), file_fd, &offset, remaining);
        if (rc > 0) {
            continue;
        }
        if (rc == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            auto wait_result = co_await stream.wait_writable();
            if (!wait_result) {
                co_return std::unexpected(wait_result.error());
            }
            continue;
        }
        if (errno == EINVAL || errno == ENOSYS) {
            co_return co_await stream_file_via_read(stream, file_fd, total_size);
        }
        co_return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    if (static_cast<std::size_t>(offset) != total_size) {
        co_return std::unexpected(IoErr::BrokenPipe);
    }
    co_return IoResult<void>{};
}

fiber::async::Task<IoResult<void>> send_request_body_to_backend(fiber::net::detail::StreamFd &stdin_stream,
                                                                const RequestBodyStorage &body) {
    if (body.size == 0) {
        co_return IoResult<void>{};
    }
    if (body.uses_temp_file()) {
        co_return co_await stream_file_to_backend(stdin_stream, body.temp_file.fd, body.size);
    }
    co_return co_await write_all(stdin_stream, body.memory.data(), body.memory.size());
}

std::optional<std::pair<std::size_t, std::size_t>> find_header_terminator(const std::vector<std::uint8_t> &buffer) {
    if (buffer.size() < 2) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i + 1 < buffer.size(); ++i) {
        if (i + 3 < buffer.size() && buffer[i] == '\r' && buffer[i + 1] == '\n' && buffer[i + 2] == '\r' &&
            buffer[i + 3] == '\n') {
            return std::pair{i, std::size_t{4}};
        }
        if (buffer[i] == '\n' && buffer[i + 1] == '\n') {
            return std::pair{i, std::size_t{2}};
        }
    }
    return std::nullopt;
}

std::optional<int> parse_status_code(std::string_view value) {
    std::string_view trimmed = value;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front())) != 0) {
        trimmed.remove_prefix(1);
    }
    std::size_t digits = 0;
    while (digits < trimmed.size() && std::isdigit(static_cast<unsigned char>(trimmed[digits])) != 0) {
        ++digits;
    }
    if (digits == 0) {
        return std::nullopt;
    }
    auto code = parse_size(trimmed.substr(0, digits));
    if (!code || *code > 999) {
        return std::nullopt;
    }
    return static_cast<int>(*code);
}

std::optional<CgiResponse> parse_cgi_response_headers(const std::vector<std::uint8_t> &raw_headers,
                                                      const std::vector<std::uint8_t> &body_prefix) {
    CgiResponse response;
    response.body_prefix = body_prefix;

    std::string text(reinterpret_cast<const char *>(raw_headers.data()), raw_headers.size());
    std::size_t offset = 0;
    while (offset <= text.size()) {
        std::size_t line_end = text.find('\n', offset);
        std::string_view line = line_end == std::string::npos
                                        ? std::string_view(text).substr(offset)
                                        : std::string_view(text).substr(offset, line_end - offset);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        offset = line_end == std::string::npos ? text.size() + 1 : line_end + 1;

        if (line.empty()) {
            continue;
        }

        std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            return std::nullopt;
        }

        std::string name = trim_ascii(line.substr(0, colon));
        std::string value = trim_ascii(line.substr(colon + 1));
        if (name.empty()) {
            return std::nullopt;
        }

        if (iequals_ascii(name, "Status")) {
            auto code = parse_status_code(value);
            if (!code) {
                return std::nullopt;
            }
            response.status_code = *code;
            continue;
        }

        if (iequals_ascii(name, "Content-Length")) {
            auto content_length = parse_size(value);
            if (!content_length) {
                return std::nullopt;
            }
            response.content_length = *content_length;
            continue;
        }

        if (iequals_ascii(name, "Connection") || iequals_ascii(name, "Transfer-Encoding")) {
            continue;
        }

        response.headers.emplace_back(std::move(name), std::move(value));
    }

    return response;
}

fiber::async::Task<IoResult<CgiResponse>> read_cgi_response(fiber::net::detail::StreamFd &stdout_stream) {
    std::vector<std::uint8_t> buffer;
    buffer.reserve(4096);
    std::array<std::uint8_t, kIoChunkSize> chunk{};

    for (;;) {
        if (auto terminator = find_header_terminator(buffer); terminator) {
            std::vector<std::uint8_t> headers(buffer.begin(),
                                              buffer.begin() + static_cast<std::ptrdiff_t>(terminator->first));
            std::vector<std::uint8_t> body_prefix(
                    buffer.begin() + static_cast<std::ptrdiff_t>(terminator->first + terminator->second), buffer.end());
            auto parsed = parse_cgi_response_headers(headers, body_prefix);
            if (!parsed) {
                co_return std::unexpected(IoErr::Invalid);
            }
            co_return *parsed;
        }

        if (buffer.size() >= kMaxCgiHeaderBytes) {
            co_return std::unexpected(IoErr::MessageTooLarge);
        }

        auto read_result = co_await read_some(stdout_stream, chunk.data(), chunk.size());
        if (!read_result) {
            co_return std::unexpected(read_result.error());
        }
        if (*read_result == 0) {
            co_return std::unexpected(IoErr::Invalid);
        }
        buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(*read_result));
    }
}

fiber::async::Task<IoResult<void>> wait_for_pid(pid_t pid) {
    if (pid <= 0) {
        co_return IoResult<void>{};
    }
    for (;;) {
        int status = 0;
        pid_t rc = ::waitpid(pid, &status, WNOHANG);
        if (rc == pid) {
            co_return IoResult<void>{};
        }
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            co_return std::unexpected(fiber::common::io_err_from_errno(errno));
        }
        co_await fiber::async::sleep(std::chrono::milliseconds(5));
    }
}

fiber::async::Task<IoResult<void>> send_error_response(fiber::http::HttpExchange &exchange, int status_code,
                                                       std::string_view message) {
    fiber::http::HttpHeaders headers(exchange.pool());
    headers.set("Content-Type", "text/plain; charset=utf-8");
    auto header_result = co_await send_final_header(exchange, status_code, &headers,
                                                    fiber::http::HttpBodySpec::ContentLength(message.size()),
                                                    fiber::http::ResponseConnectionMode::Close, message.empty());
    if (!header_result) {
        co_return std::unexpected(header_result.error());
    }
    if (!message.empty()) {
        auto body_result = co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(message.data()),
                                                       message.size(), true);
        if (!body_result) {
            co_return std::unexpected(body_result.error());
        }
    }
    co_return IoResult<void>{};
}

fiber::async::Task<IoResult<void>> forward_backend_response(fiber::http::HttpExchange &exchange,
                                                            fiber::net::detail::StreamFd &stdout_stream) {
    auto response_result = co_await read_cgi_response(stdout_stream);
    if (!response_result) {
        co_return std::unexpected(response_result.error());
    }

    const CgiResponse &response = *response_result;
    fiber::http::HttpHeaders headers(exchange.pool());
    for (const auto &header: response.headers) {
        headers.add(header.first, header.second);
    }

    fiber::http::HttpBodySpec body = response.content_length
                                             ? fiber::http::HttpBodySpec::ContentLength(*response.content_length)
                                             : fiber::http::HttpBodySpec::Auto();
    bool header_end_stream = response.content_length && *response.content_length == 0;

    auto header_result = co_await send_final_header(exchange, response.status_code, &headers, body,
                                                    fiber::http::ResponseConnectionMode::Auto, header_end_stream);
    if (!header_result) {
        co_return std::unexpected(header_result.error());
    }

    if (header_end_stream) {
        if (!response.body_prefix.empty()) {
            co_return std::unexpected(IoErr::Invalid);
        }
        co_return IoResult<void>{};
    }

    std::array<std::uint8_t, kIoChunkSize> chunk{};
    if (response.content_length) {
        std::size_t remaining = *response.content_length;
        std::size_t prefix_size = response.body_prefix.size();
        if (prefix_size > remaining) {
            co_return std::unexpected(IoErr::Invalid);
        }
        if (prefix_size > 0) {
            remaining -= prefix_size;
            auto write_result = co_await exchange.write_all(response.body_prefix.data(), prefix_size, remaining == 0);
            if (!write_result) {
                co_return std::unexpected(write_result.error());
            }
        }
        while (remaining > 0) {
            auto read_result = co_await read_some(stdout_stream, chunk.data(), std::min(chunk.size(), remaining));
            if (!read_result) {
                co_return std::unexpected(read_result.error());
            }
            if (*read_result == 0) {
                co_return std::unexpected(IoErr::BrokenPipe);
            }
            remaining -= *read_result;
            auto write_result = co_await exchange.write_all(chunk.data(), *read_result, remaining == 0);
            if (!write_result) {
                co_return std::unexpected(write_result.error());
            }
        }
        co_return IoResult<void>{};
    }

    if (!response.body_prefix.empty()) {
        auto write_result =
                co_await exchange.write_all(response.body_prefix.data(), response.body_prefix.size(), false);
        if (!write_result) {
            co_return std::unexpected(write_result.error());
        }
    }

    for (;;) {
        auto read_result = co_await read_some(stdout_stream, chunk.data(), chunk.size());
        if (!read_result) {
            co_return std::unexpected(read_result.error());
        }
        if (*read_result == 0) {
            break;
        }
        auto write_result = co_await exchange.write_all(chunk.data(), *read_result, false);
        if (!write_result) {
            co_return std::unexpected(write_result.error());
        }
    }

    auto final_result = co_await exchange.write_all(nullptr, 0, true);
    if (!final_result) {
        co_return std::unexpected(final_result.error());
    }
    co_return IoResult<void>{};
}

std::string_view backend_lookup_env(const char *name) {
    const char *value = std::getenv(name);
    return value ? std::string_view(value) : std::string_view{};
}

std::string resolve_backend_path() {
    if (auto configured = backend_lookup_env("FIBER_GIT_HTTP_BACKEND"); !configured.empty()) {
        return std::string(configured);
    }

    constexpr const char *candidates[] = {
            "/usr/lib/git-core/git-http-backend",
            "/usr/libexec/git-core/git-http-backend",
    };

    for (const char *candidate: candidates) {
        if (::access(candidate, X_OK) == 0) {
            return candidate;
        }
    }
    return {};
}

fiber::async::Task<void> handle_git_http(const std::shared_ptr<ServerConfig> &config,
                                         fiber::http::HttpExchange &exchange) {
    std::string method = method_to_string(exchange.method());
    if (method.empty()) {
        (void) co_await send_error_response(exchange, 405, "method not allowed\n");
        co_return;
    }

    auto route = resolve_repo_route(config->repositories_root, exchange.uri().path);
    if (!route) {
        (void) co_await send_error_response(exchange, 404, "repository not found\n");
        co_return;
    }

    std::string_view expect = exchange.header("expect");
    if (!expect.empty() && contains_100_continue(expect)) {
        auto continue_result = co_await exchange.send_continue_header();
        if (!continue_result) {
            co_return;
        }
    }

    auto body_result = co_await collect_request_body(exchange);
    if (!body_result) {
        (void) co_await send_error_response(exchange, 400, "invalid request body\n");
        co_return;
    }

    auto process_result = spawn_backend_process(*config, *route, exchange, body_result->size);
    if (!process_result) {
        (void) co_await send_error_response(exchange, 500, "failed to start git-http-backend\n");
        co_return;
    }

    BackendProcess process = std::move(*process_result);
    fiber::net::detail::StreamFd stdin_stream(fiber::event::EventLoop::current(), process.stdin_write.release());
    fiber::net::detail::StreamFd stdout_stream(fiber::event::EventLoop::current(), process.stdout_read.release());

    IoErr write_error = IoErr::None;
    auto write_result = co_await send_request_body_to_backend(stdin_stream, *body_result);
    if (!write_result) {
        write_error = write_result.error();
    }
    stdin_stream.close();

    auto forward_result = co_await forward_backend_response(exchange, stdout_stream);
    stdout_stream.close();
    auto wait_result = co_await wait_for_pid(process.pid);

    if (!forward_result) {
        if (write_error != IoErr::None) {
            std::cerr << "backend stdin write failed: " << fiber::common::io_err_name(write_error) << '\n';
        }
        std::cerr << "backend response forwarding failed: " << fiber::common::io_err_name(forward_result.error())
                  << '\n';
        (void) co_await send_error_response(exchange, 502, "git-http-backend failed\n");
        co_return;
    }

    if (!wait_result) {
        std::cerr << "waitpid failed: " << fiber::common::io_err_name(wait_result.error()) << '\n';
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: git_http_repo_server <repositories-root> [port]\n";
        return 1;
    }

    fs::path repositories_root = fs::absolute(argv[1]);
    std::error_code ec;
    if (!fs::is_directory(repositories_root, ec)) {
        std::cerr << "repositories root is not a directory: " << repositories_root << '\n';
        return 1;
    }

    std::uint16_t port = 8080;
    if (argc == 3) {
        auto parsed = parse_port(argv[2]);
        if (!parsed) {
            std::cerr << "invalid port\n";
            return 1;
        }
        port = *parsed;
    }

    std::string backend_path = resolve_backend_path();
    if (backend_path.empty()) {
        std::cerr << "git-http-backend not found; set FIBER_GIT_HTTP_BACKEND\n";
        return 1;
    }

    auto alias_root = TempAliasRoot::create();
    if (!alias_root) {
        std::cerr << "failed to create temporary alias root\n";
        return 1;
    }

    auto config = std::make_shared<ServerConfig>();
    config->repositories_root = std::move(repositories_root);
    config->backend_path = std::move(backend_path);
    config->alias_root = std::move(*alias_root);

    fiber::event::EventLoop loop;
    fiber::http::Http1Server server(loop, [config](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        co_await handle_git_http(config, exchange);
    });

    fiber::net::ListenOptions options{};
    fiber::net::SocketAddress addr = fiber::net::SocketAddress::any_v4(port);
    auto bind_result = server.bind(addr, options);
    if (!bind_result) {
        std::cerr << "bind failed: " << fiber::common::io_err_name(bind_result.error()) << '\n';
        return 1;
    }

    auto bound_port_result = resolve_port(server.fd());
    if (bound_port_result) {
        std::cout << "listening on 0.0.0.0:" << *bound_port_result << " root=" << config->repositories_root
                  << " alias_root=" << config->alias_root.path << " backend=" << config->backend_path << '\n';
    } else {
        std::cout << "listening on 0.0.0.0 root=" << config->repositories_root << '\n';
    }

    fiber::async::spawn(loop, [&]() { return server.serve(); });
    loop.run();
    return 0;
}
