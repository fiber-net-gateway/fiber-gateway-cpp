#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <zlib.h>

#include <openssl/sha.h>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/common/IoError.h>
#include <fiber/common/util/Base64.h>
#include <fiber/dns/DnsResolverConfig.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/ClientHttp2Exchange.h>
#include <fiber/http/ClientHttp3Exchange.h>
#include <fiber/http/Http2ClientConnection.h>
#include <fiber/http/Http3Client.h>
#include <fiber/http/Http3Server.h>
#include <fiber/http/HttpResponseWriter.h>
#include <fiber/http/HttpServer.h>
#include <fiber/log/LoggerManager.h>
#include <fiber/net/TlsContext.h>
#include "config/ConfigLoader.h"
#include "logging/LoggingBuilder.h"
#include "runtime/GzipResponseWriter.h"
#include "runtime/RuntimeBuilder.h"
#include "runtime/ServerLauncher.h"

namespace {

using namespace std::chrono_literals;
using fiber::lite_nginx::config::ConfigLoader;
using fiber::lite_nginx::runtime::RuntimeBuilder;

const char kSelfSignedCertPem[] = R"(-----BEGIN CERTIFICATE-----
MIIDCTCCAfGgAwIBAgIUEDCdxH6aX38+fEeFx3nlY3pJwdkwDQYJKoZIhvcNAQEL
BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDExNzEzMDcwNVoXDTI3MDEx
NzEzMDcwNVowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF
AAOCAQ8AMIIBCgKCAQEA4+tN+7EU3WmwFfjE4bn720reQJkTnAOUOYXg9zejQ75q
vHOpFxLU9z866mVpT7jVYAupmKfXrJ9U5Vd9znrWFzZt9rTdg+hISdujXjaEfEf+
GQ+66xthO2tAF3c6XokoqRpJR0GVInJoWaHBpV0PcvRb9AhRfuk+ja3W1dfdHnE8
LWutJCVK0HOWifIBGqpED3YMBNKZxFSKTCKLiqbxmnd6TT1fh8UI+AibEKhuJX4A
m3enMonO1PHeSOUY1dfXpZfdRdnYgjiyVyEw7oQL11r6O2LJZMJsoW912uIUnYrs
A4bDbMMfDgHe+PiyERCG62xydAlj1phGVlbGI/8HOQIDAQABo1MwUTAdBgNVHQ4E
FgQUvM4+Ad+L+GYd6i4nZgRFaPkRo7UwHwYDVR0jBBgwFoAUvM4+Ad+L+GYd6i4n
ZgRFaPkRo7UwDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOCAQEAxo8i
jbyceTsjxiMDoXd/OPtPCD2CcpWOUxMb4hdGk3pMK6xFq8c7bdMcn6oZMF7xpdHg
jDTrfa8TlPITcG/34MtvPS3hq7klCPi948Z9wbtJWGfKAl3rHYK7PIIj3wNipTcQ
IkfIlO/t6VKPSx1S9HQA6nCDOvCufOL54Mfz0vI9Y47c4O1TNtbJiiWUkP/pEjEw
RMeULfoobqmMYTjbjQ8nKC25cQAmhQ0koOqJPquPtAHvaowqBT6jDLEL+8vR4Kfc
9UqEtfRr0+7LgbcofOsseDFYMPBW2GdpPMJ2PMYsQtFMXRoomlhjdpIct6e3rRnd
GiDzEZ0VwkYlJDwF4w==
-----END CERTIFICATE-----
)";

const char kSelfSignedKeyPem[] = R"(-----BEGIN PRIVATE KEY-----
MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQDj6037sRTdabAV
+MThufvbSt5AmROcA5Q5heD3N6NDvmq8c6kXEtT3PzrqZWlPuNVgC6mYp9esn1Tl
V33OetYXNm32tN2D6EhJ26NeNoR8R/4ZD7rrG2E7a0AXdzpeiSipGklHQZUicmhZ
ocGlXQ9y9Fv0CFF+6T6NrdbV190ecTwta60kJUrQc5aJ8gEaqkQPdgwE0pnEVIpM
IouKpvGad3pNPV+HxQj4CJsQqG4lfgCbd6cyic7U8d5I5RjV19ell91F2diCOLJX
ITDuhAvXWvo7Yslkwmyhb3Xa4hSdiuwDhsNswx8OAd74+LIREIbrbHJ0CWPWmEZW
VsYj/wc5AgMBAAECggEAHomvmDKg1g3MHxWG46u0uCwu3T7lZrkACjkK7HTS9ke0
K23f0Qyf5kTdkvxlgN4GEOlfHuoWNrXefSAc5iaFOvT7BNw09fCQhvzbxcrOM4y9
2gPGiqvPelOjccFy26nK/eVcviRmZAgqPSA0PwDaCg/9phPbP4Lm87rAF0TmBqbq
n5s+7MXf4iFTbRIec2zTikWfbUglhNmKr3eC/4+K+hk3TX95Wltvz6dGz+godV/L
FilwLEa+e0cSTUA8FYzYtoEUiV7/8dl8VBIvQWtx8sRNNihCmnlYrJ3N8tw/hO6F
PKpfoOo+L9uRJG4LGtAkM0Pqs9U9uN5v7F5HNMxO1QKBgQD61LhiF/ftPlTRFQm2
CrnIN4PcQtIDRar/cuwgyq3F8AAfJ5PSYD/GvitaQYxa9Ya1IM3T7UPx6L3OmJl6
updR3Mh/+6BtAYwSwoWLv0tHQ01xOe9pwML52JShVocVXQFE/UXNtuffuUpXVeWk
miVen8SI4CHLeFU+6Dfcp0l3owKBgQDonbYbB9bRVzG0gbgdp2K1pxvMQizR8IkU
GsYaT/LMooBpRBOHrane+9KCztkghjmTyDKEl7jwt65fvFl0ttkipq1ISTepV6Rt
Cmdc5PnBc+ON49/6ivTGFAdU5CY3sE/7L6ngPqZq6bq8nBJ0NPcjpfEl2JfBeND8
NisrSQEjcwKBgQDlcp1QLji/LtuLf0Eo41rbCd13KTDPiXVIw6m4vW6EuGyEE0In
mZ/9f4xMvdVUh3C4U8+04z/aFFs8l18eY310hxBp8pXn4RhvOL3M/iowgCJhRuv4
wzoYLsSXaX2cTz2QDFdEPOKTRv34Mj0le1Rf4Kp5wv1nESZ5qxceo3CTHQKBgFWb
jSR/ixB57YIH53GKY6qEuJdAl2wgAOLUQ6n1WF71Qxr6gdGCGS1GMiAP7hqpK1F2
8RiZGegFQXhcQfPRQzIcc1NSFtkMtyemF4o5fq0ycEGM5qY3M4QeZOBaIrKGAblo
vjUX+XkJUb8OFUCNKZMGBCywfJEoXIklilegw3l/AoGBALtmVrX28WQ42DOYWdKD
dmDMBg1+21d8wIWs4k5bu1LdlY8XqMnV9TAHwOwGcleK2uM3AfoLOho6HwFwdyhJ
x20XBogOziImjh+cvWNpm951EC3oWHOFYPsMjX1mRCye88LQHwm3gQ8iCIOzPj+8
RB6SahiCZEhAtLq/9Q/O1bL5
-----END PRIVATE KEY-----
)";

class TestPemFile {
public:
    TestPemFile(std::string_view tag, std::string_view data) {
        static std::atomic<std::uint32_t> next_id{1};
        path_ = "/tmp/lite_nginx_";
        path_.append(tag);
        path_.push_back('_');
        path_.append(std::to_string(static_cast<long>(::getpid())));
        path_.push_back('_');
        path_.append(std::to_string(next_id.fetch_add(1, std::memory_order_relaxed)));
        path_.append(".pem");

        std::ofstream out(path_, std::ios::binary);
        if (!out) {
            path_.clear();
            return;
        }
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        ok_ = out.good();
        if (!ok_) {
            path_.clear();
        }
    }

    ~TestPemFile() {
        if (!path_.empty()) {
            ::unlink(path_.c_str());
        }
    }

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] const std::string &path() const noexcept { return path_; }

private:
    std::string path_;
    bool ok_ = false;
};

class TestLogFile {
public:
    TestLogFile() {
        char pattern[] = "/tmp/lite_nginx_access_XXXXXX";
        const int fd = ::mkstemp(pattern);
        if (fd >= 0) {
            ::close(fd);
            path_ = pattern;
        }
    }

    ~TestLogFile() {
        if (!path_.empty()) {
            ::unlink(path_.c_str());
        }
    }

    [[nodiscard]] bool valid() const noexcept { return !path_.empty(); }
    [[nodiscard]] const std::string &path() const noexcept { return path_; }

    [[nodiscard]] std::string read() const {
        std::ifstream input(path_, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

private:
    std::string path_;
};

class TestScriptFile {
public:
    TestScriptFile(std::string_view tag, std::string_view source) {
        static std::atomic<std::uint32_t> next_id{1};
        path_ = "/tmp/lite_nginx_";
        path_.append(tag);
        path_.push_back('_');
        path_.append(std::to_string(static_cast<long>(::getpid())));
        path_.push_back('_');
        path_.append(std::to_string(next_id.fetch_add(1, std::memory_order_relaxed)));
        path_.append(".js");

        std::ofstream out(path_, std::ios::binary);
        if (!out) {
            path_.clear();
            return;
        }
        out.write(source.data(), static_cast<std::streamsize>(source.size()));
        ok_ = out.good();
        if (!ok_) {
            ::unlink(path_.c_str());
            path_.clear();
        }
    }

    ~TestScriptFile() {
        if (!path_.empty()) {
            ::unlink(path_.c_str());
        }
    }

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] const std::string &path() const noexcept { return path_; }

private:
    std::string path_;
    bool ok_ = false;
};

class TestLoggingScope {
public:
    TestLoggingScope() { fiber::log::LoggerManager::global().shutdown(); }
    ~TestLoggingScope() { fiber::log::LoggerManager::global().shutdown(); }
};

struct ShutdownOp {
    fiber::event::EventLoop::NotifyEntry entry;
    fiber::lite_nginx::runtime::ServerLauncher *launcher = nullptr;
    fiber::event::EventLoop *loop = nullptr;

    static void on_run(ShutdownOp *self) noexcept {
        if (self->launcher) {
            self->launcher->close();
        }
        if (self->loop) {
            self->loop->stop();
        }
    }
};

std::string recv_http_response(int fd) {
    std::string out;
    std::array<char, 4096> buf{};
    std::size_t header_end = std::string::npos;
    std::size_t content_length = 0;

    for (;;) {
        if (header_end != std::string::npos && out.size() >= header_end + content_length) {
            return out.substr(0, header_end + content_length);
        }

        ssize_t rc = ::recv(fd, buf.data(), buf.size(), 0);
        if (rc <= 0) {
            return out;
        }
        out.append(buf.data(), static_cast<std::size_t>(rc));

        if (header_end != std::string::npos) {
            continue;
        }

        std::size_t pos = out.find("\r\n\r\n");
        if (pos == std::string::npos) {
            continue;
        }
        header_end = pos + 4;

        std::size_t cl_pos = out.find("Content-Length:");
        if (cl_pos == std::string::npos) {
            return out;
        }
        cl_pos += sizeof("Content-Length:") - 1;
        while (cl_pos < pos && out[cl_pos] == ' ') {
            ++cl_pos;
        }
        std::size_t cl_end = out.find("\r\n", cl_pos);
        if (cl_end == std::string::npos) {
            return out;
        }
        content_length = static_cast<std::size_t>(std::stoul(out.substr(cl_pos, cl_end - cl_pos)));
    }
}

std::string recv_until_close(int fd) {
    std::string out;
    std::array<char, 4096> buf{};
    for (;;) {
        const ssize_t rc = ::recv(fd, buf.data(), buf.size(), 0);
        if (rc <= 0) {
            return out;
        }
        out.append(buf.data(), static_cast<std::size_t>(rc));
    }
}

std::string decode_chunked_body(std::string_view response) {
    const std::size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        return {};
    }
    std::string decoded;
    std::size_t offset = header_end + 4;
    while (offset < response.size()) {
        const std::size_t line_end = response.find("\r\n", offset);
        if (line_end == std::string_view::npos) {
            return {};
        }
        std::size_t chunk_size = 0;
        auto parsed = std::from_chars(response.data() + offset, response.data() + line_end, chunk_size, 16);
        if (parsed.ec != std::errc() || parsed.ptr != response.data() + line_end) {
            return {};
        }
        offset = line_end + 2;
        if (chunk_size == 0) {
            return decoded;
        }
        if (chunk_size > response.size() - offset || response.size() - offset - chunk_size < 2 ||
            response.substr(offset + chunk_size, 2) != "\r\n") {
            return {};
        }
        decoded.append(response.substr(offset, chunk_size));
        offset += chunk_size + 2;
    }
    return {};
}

std::string gunzip_body(std::string_view compressed) {
    z_stream stream{};
    if (inflateInit2(&stream, MAX_WBITS + 16) != Z_OK) {
        return {};
    }
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(compressed.data()));
    stream.avail_in = static_cast<uInt>(compressed.size());
    std::string output;
    std::array<char, 4096> buffer{};
    int result = Z_OK;
    while (result == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef *>(buffer.data());
        stream.avail_out = static_cast<uInt>(buffer.size());
        result = inflate(&stream, Z_NO_FLUSH);
        output.append(buffer.data(), buffer.size() - stream.avail_out);
    }
    const int end_result = inflateEnd(&stream);
    return result == Z_STREAM_END && end_result == Z_OK ? output : std::string{};
}

std::uint16_t reserve_loopback_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return 0;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return 0;
    }

    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
        ::close(fd);
        return 0;
    }

    std::uint16_t port = ntohs(bound.sin_port);
    ::close(fd);
    return port;
}

int connect_client(std::uint16_t port) {
    int client = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (client < 0) {
        return -1;
    }

    timeval tv{};
    tv.tv_sec = 3;
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(client, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(client);
        return -1;
    }
    return client;
}

std::string read_http_request(int fd) {
    std::string out;
    std::array<char, 4096> buf{};
    std::size_t header_end = std::string::npos;
    std::size_t content_length = 0;

    for (;;) {
        if (header_end != std::string::npos && out.size() >= header_end + content_length) {
            return out.substr(0, header_end + content_length);
        }

        ssize_t rc = ::recv(fd, buf.data(), buf.size(), 0);
        if (rc <= 0) {
            return out;
        }
        out.append(buf.data(), static_cast<std::size_t>(rc));

        if (header_end != std::string::npos) {
            continue;
        }

        std::size_t pos = out.find("\r\n\r\n");
        if (pos == std::string::npos) {
            continue;
        }
        header_end = pos + 4;

        std::size_t cl_pos = out.find("Content-Length:");
        if (cl_pos == std::string::npos) {
            return out;
        }
        cl_pos += sizeof("Content-Length:") - 1;
        while (cl_pos < pos && out[cl_pos] == ' ') {
            ++cl_pos;
        }
        std::size_t cl_end = out.find("\r\n", cl_pos);
        if (cl_end == std::string::npos) {
            return out;
        }
        content_length = static_cast<std::size_t>(std::stoul(out.substr(cl_pos, cl_end - cl_pos)));
    }
}

bool send_all(int fd, std::string_view data) {
    while (!data.empty()) {
        const ssize_t rc = ::send(fd, data.data(), data.size(), 0);
        if (rc <= 0) {
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(rc));
    }
    return true;
}

std::string recv_until_contains(int fd, std::string_view expected) {
    std::string out;
    std::array<char, 4096> buf{};
    while (out.find(expected) == std::string::npos) {
        const ssize_t rc = ::recv(fd, buf.data(), buf.size(), 0);
        if (rc <= 0) {
            break;
        }
        out.append(buf.data(), static_cast<std::size_t>(rc));
    }
    return out;
}

bool socket_readable(int fd, std::chrono::milliseconds timeout) {
    pollfd descriptor{
            .fd = fd,
            .events = POLLIN,
            .revents = 0,
    };
    int rc;
    do {
        rc = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
    } while (rc < 0 && errno == EINTR);
    return rc > 0 && (descriptor.revents & (POLLIN | POLLHUP)) != 0;
}

std::string recv_available(int fd) {
    std::array<char, 4096> buf{};
    const ssize_t rc = ::recv(fd, buf.data(), buf.size(), MSG_DONTWAIT);
    return rc > 0 ? std::string(buf.data(), static_cast<std::size_t>(rc)) : std::string{};
}

std::string recv_exact(int fd, std::size_t expected) {
    std::string out;
    out.resize(expected);
    std::size_t offset = 0;
    while (offset < expected) {
        const ssize_t rc = ::recv(fd, out.data() + offset, expected - offset, 0);
        if (rc <= 0) {
            out.resize(offset);
            break;
        }
        offset += static_cast<std::size_t>(rc);
    }
    return out;
}

std::string http_header_value(std::string_view message, std::string_view name) {
    std::string prefix(name);
    prefix.append(":");
    std::size_t pos = message.find(prefix);
    if (pos == std::string_view::npos) {
        return {};
    }
    pos += prefix.size();
    while (pos < message.size() && (message[pos] == ' ' || message[pos] == '\t')) {
        ++pos;
    }
    std::size_t end = message.find("\r\n", pos);
    if (end == std::string_view::npos) {
        return {};
    }
    return std::string(message.substr(pos, end - pos));
}

std::string websocket_accept(std::string_view key) {
    std::string source(key);
    source.append("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    std::array<std::uint8_t, SHA_DIGEST_LENGTH> digest{};
    if (SHA1(reinterpret_cast<const std::uint8_t *>(source.data()), source.size(), digest.data()) == nullptr) {
        return {};
    }
    return fiber::util::base64_encode(digest.data(), digest.size());
}

class WebSocketUpstream {
public:
    WebSocketUpstream(std::promise<std::string> *request_promise, std::promise<std::string> *body_promise) :
        request_promise_(request_promise), body_promise_(body_promise) {
        listener_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        EXPECT_GE(listener_fd_, 0);
        if (listener_fd_ < 0) {
            return;
        }

        int yes = 1;
        ::setsockopt(listener_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(0);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listener_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
            ::listen(listener_fd_, 16) != 0) {
            ADD_FAILURE() << "websocket upstream listen failed: " << errno;
            ::close(listener_fd_);
            listener_fd_ = -1;
            return;
        }

        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        if (::getsockname(listener_fd_, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
            ADD_FAILURE() << "websocket upstream getsockname failed: " << errno;
            ::close(listener_fd_);
            listener_fd_ = -1;
            return;
        }
        port_ = ntohs(bound.sin_port);

        thread_ = std::thread([this]() {
            int client = ::accept4(listener_fd_, nullptr, nullptr, SOCK_CLOEXEC);
            if (client < 0) {
                publish({}, {});
                return;
            }
            timeval tv{};
            tv.tv_sec = 3;
            ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            std::string request = read_http_request(client);
            if (request_promise_) {
                request_promise_->set_value(request);
                request_promise_ = nullptr;
            }

            const std::string accept = websocket_accept(http_header_value(request, "Sec-WebSocket-Key"));
            std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                                   "Connection: Upgrade\r\n"
                                   "Upgrade: websocket\r\n"
                                   "Sec-WebSocket-Accept: ";
            response.append(accept);
            response.append("\r\nSec-WebSocket-Protocol: chat\r\n\r\nserver-frame");
            (void) send_all(client, response);
            std::string body = recv_exact(client, std::string_view("client-frame").size());
            if (body_promise_) {
                body_promise_->set_value(body);
                body_promise_ = nullptr;
            }
            ::shutdown(client, SHUT_RDWR);
            ::close(client);
        });
    }

    ~WebSocketUpstream() {
        if (listener_fd_ >= 0) {
            ::shutdown(listener_fd_, SHUT_RDWR);
            ::close(listener_fd_);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        publish({}, {});
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    void publish(std::string request, std::string body) {
        if (request_promise_) {
            request_promise_->set_value(std::move(request));
            request_promise_ = nullptr;
        }
        if (body_promise_) {
            body_promise_->set_value(std::move(body));
            body_promise_ = nullptr;
        }
    }

    int listener_fd_ = -1;
    std::uint16_t port_ = 0;
    std::promise<std::string> *request_promise_ = nullptr;
    std::promise<std::string> *body_promise_ = nullptr;
    std::thread thread_{};
};

class SingleRequestUpstream {
public:
    SingleRequestUpstream(std::string response, std::promise<std::string> *request_promise) :
        response_(std::move(response)), request_promise_(request_promise) {
        listener_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        EXPECT_GE(listener_fd_, 0);
        if (listener_fd_ < 0) {
            return;
        }

        int yes = 1;
        ::setsockopt(listener_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(0);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listener_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
            ADD_FAILURE() << "bind failed: " << errno;
            ::close(listener_fd_);
            listener_fd_ = -1;
            return;
        }
        if (::listen(listener_fd_, 16) != 0) {
            ADD_FAILURE() << "listen failed: " << errno;
            ::close(listener_fd_);
            listener_fd_ = -1;
            return;
        }

        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        if (::getsockname(listener_fd_, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
            ADD_FAILURE() << "getsockname failed: " << errno;
            ::close(listener_fd_);
            listener_fd_ = -1;
            return;
        }
        port_ = ntohs(bound.sin_port);

        thread_ = std::thread([this]() {
            int client = ::accept4(listener_fd_, nullptr, nullptr, SOCK_CLOEXEC);
            if (client < 0) {
                if (request_promise_) {
                    request_promise_->set_value({});
                }
                return;
            }

            std::string request = read_http_request(client);
            if (request_promise_) {
                request_promise_->set_value(request);
            }

            const char *data = response_.data();
            std::size_t remaining = response_.size();
            while (remaining > 0) {
                ssize_t rc = ::send(client, data, remaining, 0);
                if (rc <= 0) {
                    break;
                }
                data += static_cast<std::size_t>(rc);
                remaining -= static_cast<std::size_t>(rc);
            }
            ::shutdown(client, SHUT_RDWR);
            ::close(client);
        });
    }

    ~SingleRequestUpstream() {
        if (listener_fd_ >= 0) {
            ::shutdown(listener_fd_, SHUT_RDWR);
            ::close(listener_fd_);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    int listener_fd_ = -1;
    std::uint16_t port_ = 0;
    std::string response_;
    std::promise<std::string> *request_promise_ = nullptr;
    std::thread thread_{};
};

class StagedResponseUpstream {
public:
    StagedResponseUpstream(std::string first_segment, std::string tail) :
        first_segment_(std::move(first_segment)), tail_(std::move(tail)),
        first_sent_future_(first_sent_promise_.get_future()), release_future_(release_promise_.get_future().share()) {
        listener_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        EXPECT_GE(listener_fd_, 0);
        if (listener_fd_ < 0) {
            return;
        }

        int yes = 1;
        ::setsockopt(listener_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(0);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listener_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
            ::listen(listener_fd_, 16) != 0) {
            ADD_FAILURE() << "staged upstream listen failed: " << errno;
            ::close(listener_fd_);
            listener_fd_ = -1;
            return;
        }

        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        if (::getsockname(listener_fd_, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
            ADD_FAILURE() << "staged upstream getsockname failed: " << errno;
            ::close(listener_fd_);
            listener_fd_ = -1;
            return;
        }
        port_ = ntohs(bound.sin_port);

        thread_ = std::thread([this]() {
            const int client = ::accept4(listener_fd_, nullptr, nullptr, SOCK_CLOEXEC);
            if (client < 0) {
                first_sent_promise_.set_value(false);
                return;
            }
            client_fd_.store(client, std::memory_order_release);
            (void) read_http_request(client);

            const bool first_sent = send_all(client, first_segment_);
            first_sent_promise_.set_value(first_sent);
            if (first_sent) {
                release_future_.wait();
                (void) send_all(client, tail_);
            }

            if (client_fd_.exchange(-1, std::memory_order_acq_rel) == client) {
                ::shutdown(client, SHUT_RDWR);
                ::close(client);
            }
        });
    }

    ~StagedResponseUpstream() {
        release_tail();
        if (listener_fd_ >= 0) {
            ::shutdown(listener_fd_, SHUT_RDWR);
            ::close(listener_fd_);
        }
        const int client = client_fd_.exchange(-1, std::memory_order_acq_rel);
        if (client >= 0) {
            ::shutdown(client, SHUT_RDWR);
            ::close(client);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    bool wait_for_first_segment(std::chrono::milliseconds timeout) {
        return first_sent_future_.wait_for(timeout) == std::future_status::ready && first_sent_future_.get();
    }

    void release_tail() {
        if (!tail_released_) {
            tail_released_ = true;
            release_promise_.set_value();
        }
    }

private:
    int listener_fd_ = -1;
    std::atomic<int> client_fd_{-1};
    std::uint16_t port_ = 0;
    std::string first_segment_;
    std::string tail_;
    std::promise<bool> first_sent_promise_;
    std::future<bool> first_sent_future_;
    std::promise<void> release_promise_;
    std::shared_future<void> release_future_;
    bool tail_released_ = false;
    std::thread thread_{};
};

class AbortAwareUpstream {
public:
    AbortAwareUpstream(std::promise<void> *accepted_promise, std::promise<bool> *closed_promise) :
        accepted_promise_(accepted_promise), closed_promise_(closed_promise) {
        listener_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        EXPECT_GE(listener_fd_, 0);
        if (listener_fd_ < 0) {
            publish(false);
            return;
        }

        int yes = 1;
        ::setsockopt(listener_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(0);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listener_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
            ::listen(listener_fd_, 16) != 0) {
            ADD_FAILURE() << "abort-aware upstream listen failed: " << errno;
            ::close(listener_fd_);
            listener_fd_ = -1;
            publish(false);
            return;
        }

        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        if (::getsockname(listener_fd_, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
            ADD_FAILURE() << "abort-aware upstream getsockname failed: " << errno;
            ::close(listener_fd_);
            listener_fd_ = -1;
            publish(false);
            return;
        }
        port_ = ntohs(bound.sin_port);

        thread_ = std::thread([this]() {
            const int client = ::accept4(listener_fd_, nullptr, nullptr, SOCK_CLOEXEC);
            if (client < 0) {
                publish(false);
                return;
            }
            client_fd_.store(client, std::memory_order_release);
            if (accepted_promise_) {
                accepted_promise_->set_value();
                accepted_promise_ = nullptr;
            }

            timeval tv{};
            tv.tv_sec = 5;
            ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            (void) read_http_request(client);

            char byte = 0;
            const ssize_t rc = ::recv(client, &byte, 1, 0);
            publish(rc == 0);

            if (client_fd_.exchange(-1, std::memory_order_acq_rel) == client) {
                ::shutdown(client, SHUT_RDWR);
                ::close(client);
            }
        });
    }

    ~AbortAwareUpstream() {
        if (listener_fd_ >= 0) {
            ::shutdown(listener_fd_, SHUT_RDWR);
            ::close(listener_fd_);
        }
        const int client = client_fd_.exchange(-1, std::memory_order_acq_rel);
        if (client >= 0) {
            ::shutdown(client, SHUT_RDWR);
            ::close(client);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        publish(false);
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    void publish(bool closed) {
        if (accepted_promise_) {
            accepted_promise_->set_value();
            accepted_promise_ = nullptr;
        }
        if (closed_promise_) {
            closed_promise_->set_value(closed);
            closed_promise_ = nullptr;
        }
    }

    int listener_fd_ = -1;
    std::atomic<int> client_fd_{-1};
    std::uint16_t port_ = 0;
    std::promise<void> *accepted_promise_ = nullptr;
    std::promise<bool> *closed_promise_ = nullptr;
    std::thread thread_{};
};

class KeepAliveUpstream {
public:
    KeepAliveUpstream(std::array<std::string, 2> responses,
                      std::array<std::promise<std::string> *, 2> request_promises) :
        responses_(std::move(responses)), request_promises_(request_promises) {
        listener_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        EXPECT_GE(listener_fd_, 0);
        if (listener_fd_ < 0) {
            return;
        }

        int yes = 1;
        ::setsockopt(listener_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(0);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listener_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
            ADD_FAILURE() << "bind failed: " << errno;
            ::close(listener_fd_);
            listener_fd_ = -1;
            return;
        }
        if (::listen(listener_fd_, 16) != 0) {
            ADD_FAILURE() << "listen failed: " << errno;
            ::close(listener_fd_);
            listener_fd_ = -1;
            return;
        }

        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        if (::getsockname(listener_fd_, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
            ADD_FAILURE() << "getsockname failed: " << errno;
            ::close(listener_fd_);
            listener_fd_ = -1;
            return;
        }
        port_ = ntohs(bound.sin_port);

        thread_ = std::thread([this]() {
            std::size_t served = 0;
            while (listener_fd_ >= 0 && served < responses_.size()) {
                int client = ::accept4(listener_fd_, nullptr, nullptr, SOCK_CLOEXEC);
                if (client < 0) {
                    break;
                }
                ++accept_count_;

                while (served < responses_.size()) {
                    std::string request = read_http_request(client);
                    if (request.empty()) {
                        break;
                    }
                    if (request_promises_[served]) {
                        request_promises_[served]->set_value(request);
                    }

                    const char *data = responses_[served].data();
                    std::size_t remaining = responses_[served].size();
                    while (remaining > 0) {
                        ssize_t rc = ::send(client, data, remaining, 0);
                        if (rc <= 0) {
                            break;
                        }
                        data += static_cast<std::size_t>(rc);
                        remaining -= static_cast<std::size_t>(rc);
                    }
                    ++served;
                    if (remaining > 0) {
                        break;
                    }
                }

                ::shutdown(client, SHUT_RDWR);
                ::close(client);
            }
        });
    }

    ~KeepAliveUpstream() {
        if (listener_fd_ >= 0) {
            ::shutdown(listener_fd_, SHUT_RDWR);
            ::close(listener_fd_);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] int accept_count() const noexcept { return accept_count_.load(); }

private:
    int listener_fd_ = -1;
    std::uint16_t port_ = 0;
    std::array<std::string, 2> responses_{};
    std::array<std::promise<std::string> *, 2> request_promises_{};
    std::atomic<int> accept_count_{0};
    std::thread thread_{};
};

class TlsSingleRequestUpstream {
public:
    TlsSingleRequestUpstream() : cert_("upstream_cert", kSelfSignedCertPem), key_("upstream_key", kSelfSignedKeyPem) {
        EXPECT_TRUE(cert_.ok());
        EXPECT_TRUE(key_.ok());
        if (!cert_.ok() || !key_.ok()) {
            return;
        }

        fiber::net::TlsOptions tls_material{};
        tls_material.certificate_chain = fiber::net::TlsPemSource::from_file(cert_.path());
        tls_material.private_key = fiber::net::TlsPemSource::from_file(key_.path());
        auto tls_context = fiber::net::TlsContext::create(tls_material);
        EXPECT_TRUE(tls_context);
        if (!tls_context) {
            return;
        }
        tls_context_ = std::move(*tls_context);

        group_.start();
        std::promise<std::uint16_t> ready;
        auto ready_future = ready.get_future();
        fiber::async::spawn(group_.at(0), [this, &ready]() -> fiber::async::DetachedTask {
            fiber::http::HttpServerOptions options;
            options.tls.default_context = tls_context_.get();

            auto handler = [](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
                static constexpr std::string_view kBody = "secure";
                auto header_result = co_await exchange.send_header({
                        .kind = fiber::http::OutgoingHeaderKind::Final,
                        .status_code = 200,
                        .body = fiber::http::ResponseBodySpec::ContentLength(kBody.size()),
                        .end_stream = false,
                });
                if (header_result) {
                    (void) co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(kBody.data()),
                                                       kBody.size(), true, 3s);
                }
            };

            server_ = new fiber::http::HttpServer(group_.at(0), std::move(handler), std::move(options));
            fiber::net::ListenOptions listen_options;
            auto bind_result =
                    server_->bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), listen_options);
            if (!bind_result) {
                delete server_;
                server_ = nullptr;
                ready.set_value(0);
                co_return;
            }

            sockaddr_in bound{};
            socklen_t len = sizeof(bound);
            if (::getsockname(server_->fd(), reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
                server_->close();
                delete server_;
                server_ = nullptr;
                ready.set_value(0);
                co_return;
            }
            ready.set_value(ntohs(bound.sin_port));
            fiber::async::spawn(group_.at(0), [this]() { return server_->serve(); });
        });
        port_ = ready_future.get();
    }

    ~TlsSingleRequestUpstream() {
        if (!group_.running()) {
            return;
        }

        std::promise<void> stopped;
        auto stopped_future = stopped.get_future();
        fiber::async::spawn(group_.at(0), [this, &stopped]() -> fiber::async::DetachedTask {
            if (server_) {
                co_await server_->shutdown_and_wait();
            }
            group_.at(0).stop();
            stopped.set_value();
            co_return;
        });
        stopped_future.wait();
        group_.join();
        delete server_;
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    TestPemFile cert_;
    TestPemFile key_;
    std::unique_ptr<fiber::net::TlsContext> tls_context_;
    fiber::event::EventLoopGroup group_{1};
    fiber::http::HttpServer *server_ = nullptr;
    std::uint16_t port_ = 0;
};

struct Http2WebSocketOutcome {
    fiber::common::IoErr error = fiber::common::IoErr::None;
    int status_code = 0;
    std::string connection;
    std::string upgrade;
    std::string accept;
    std::string protocol;
    std::string body;
    bool extended_connect_enabled = false;
};

struct Http2GzipOutcome {
    fiber::common::IoErr error = fiber::common::IoErr::None;
    int status_code = 0;
    std::string content_encoding;
    std::string content_length;
    std::string body;
    bool body_complete = false;
};

struct Http2RunState {
    std::atomic_bool done{false};
};

fiber::async::DetachedTask run_http2_connection(std::shared_ptr<fiber::http::Http2ClientConnection> connection,
                                                std::shared_ptr<Http2RunState> state) {
    (void) co_await connection->wait_closed();
    state->done.store(true, std::memory_order_release);
}

std::string chain_to_string(fiber::mem::IoBufChain chain) {
    std::string out;
    out.reserve(chain.readable_bytes());
    while (auto *front = chain.first_readable()) {
        out.append(reinterpret_cast<const char *>(front->readable_data()), front->readable());
        chain.consume_and_compact(front->readable());
    }
    return out;
}

fiber::async::DetachedTask run_http2_websocket_client(fiber::event::EventLoop *loop, std::uint16_t port,
                                                      std::promise<Http2WebSocketOutcome> *promise) {
    Http2WebSocketOutcome outcome;
    auto tls_context = fiber::net::TlsContext::create({});
    if (!tls_context) {
        outcome.error = tls_context.error();
        promise->set_value(std::move(outcome));
        co_return;
    }
    fiber::http::Http2ClientConnection::Options options;
    options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);
    options.tls.context = tls_context->get();
    options.tls.sni_name = "localhost";

    auto connection = std::make_shared<fiber::http::Http2ClientConnection>(*loop, std::move(options));
    auto connect_result = co_await connection->connect(5s);
    if (!connect_result) {
        outcome.error = connect_result.error();
        promise->set_value(std::move(outcome));
        co_return;
    }

    auto run_state = std::make_shared<Http2RunState>();
    fiber::async::spawn(*loop, [connection, run_state]() { return run_http2_connection(connection, run_state); });

    fiber::mem::BufPool pool;
    fiber::http::ClientHttp2Exchange exchange(*connection, pool);
    for (int i = 0; i < 500; ++i) {
        if (exchange.extended_connect_support() == fiber::http::Http2ExtendedConnectSupport::Enabled) {
            outcome.extended_connect_enabled = true;
            break;
        }
        co_await fiber::async::sleep(1ms);
    }

    if (!outcome.extended_connect_enabled) {
        outcome.error = fiber::common::IoErr::NotSupported;
    } else {
        fiber::http::HttpHeaders headers(pool);
        headers.set("Sec-WebSocket-Version", "13");
        headers.set("Sec-WebSocket-Protocol", "chat");
        auto send_result = co_await exchange.send_request_header(
                {
                        .method = fiber::http::HttpMethod::Connect,
                        .scheme = "https",
                        .authority = "localhost",
                        .path = "/chat",
                        .protocol = "websocket",
                        .headers = &headers,
                },
                false, 2s);
        if (!send_result) {
            outcome.error = send_result.error();
        } else {
            auto header_result = co_await exchange.read_header(2s);
            if (!header_result) {
                outcome.error = header_result.error();
            } else {
                outcome.status_code = (*header_result)->status_code;
                outcome.connection = std::string((*header_result)->headers.get("connection"));
                outcome.upgrade = std::string((*header_result)->headers.get("upgrade"));
                outcome.accept = std::string((*header_result)->headers.get("sec-websocket-accept"));
                outcome.protocol = std::string((*header_result)->headers.get("sec-websocket-protocol"));

                static constexpr std::string_view kClientFrame = "client-frame";
                auto write_result = co_await exchange.write_all(
                        reinterpret_cast<const std::uint8_t *>(kClientFrame.data()), kClientFrame.size(), false, 2s);
                if (!write_result) {
                    outcome.error = write_result.error();
                } else {
                    while (outcome.body.size() < std::string_view("server-frame").size()) {
                        auto body_result = co_await exchange.read_body(64, 2s);
                        if (!body_result) {
                            outcome.error = body_result.error();
                            break;
                        }
                        const bool complete = body_result->complete();
                        outcome.body.append(chain_to_string(std::move(*body_result)));
                        if (complete) {
                            break;
                        }
                    }
                }
            }
        }
    }

    if (exchange.valid()) {
        (void) exchange.abort();
    }
    connection->shutdown();
    for (int i = 0; i < 500 && !run_state->done.load(std::memory_order_acquire); ++i) {
        co_await fiber::async::sleep(1ms);
    }
    if (!run_state->done.load(std::memory_order_acquire) && outcome.error == fiber::common::IoErr::None) {
        outcome.error = fiber::common::IoErr::TimedOut;
    }
    promise->set_value(std::move(outcome));
}

fiber::async::DetachedTask run_http2_gzip_client(fiber::event::EventLoop *loop, std::uint16_t port,
                                                 std::promise<Http2GzipOutcome> *promise) {
    Http2GzipOutcome outcome;
    auto tls_context = fiber::net::TlsContext::create({});
    if (!tls_context) {
        outcome.error = tls_context.error();
        promise->set_value(std::move(outcome));
        co_return;
    }
    fiber::http::Http2ClientConnection::Options options;
    options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);
    options.tls.context = tls_context->get();
    options.tls.sni_name = "localhost";

    auto connection = std::make_shared<fiber::http::Http2ClientConnection>(*loop, std::move(options));
    auto connect_result = co_await connection->connect(5s);
    if (!connect_result) {
        outcome.error = connect_result.error();
        promise->set_value(std::move(outcome));
        co_return;
    }

    auto run_state = std::make_shared<Http2RunState>();
    fiber::async::spawn(*loop, [connection, run_state]() { return run_http2_connection(connection, run_state); });

    fiber::mem::BufPool pool;
    fiber::http::HttpHeaders headers(pool);
    headers.set("Accept-Encoding", "gzip");
    fiber::http::ClientHttp2Exchange exchange(*connection, pool);
    auto send_result = co_await exchange.send_request_header(
            {
                    .method = fiber::http::HttpMethod::Get,
                    .scheme = "https",
                    .authority = "localhost",
                    .path = "/gzip",
                    .headers = &headers,
            },
            true, 2s);
    if (!send_result) {
        outcome.error = send_result.error();
    } else {
        auto header_result = co_await exchange.read_header(2s);
        if (!header_result) {
            outcome.error = header_result.error();
        } else {
            outcome.status_code = (*header_result)->status_code;
            outcome.content_encoding = std::string((*header_result)->headers.get("content-encoding"));
            outcome.content_length = std::string((*header_result)->headers.get("content-length"));
            while (!outcome.body_complete) {
                auto body_result = co_await exchange.read_body(64 * 1024, 2s);
                if (!body_result) {
                    outcome.error = body_result.error();
                    break;
                }
                outcome.body_complete = body_result->complete();
                outcome.body.append(chain_to_string(std::move(*body_result)));
            }
        }
    }

    connection->shutdown();
    for (int i = 0; i < 500 && !run_state->done.load(std::memory_order_acquire); ++i) {
        co_await fiber::async::sleep(1ms);
    }
    if (!run_state->done.load(std::memory_order_acquire) && outcome.error == fiber::common::IoErr::None) {
        outcome.error = fiber::common::IoErr::TimedOut;
    }
    promise->set_value(std::move(outcome));
}

struct Http3GzipOutcome {
    fiber::common::IoErr error = fiber::common::IoErr::None;
    int status_code = 0;
    std::string content_encoding;
    std::string content_length;
    std::string body;
    fiber::http::Http3RequestOutcome request_outcome = fiber::http::Http3RequestOutcome::NotSent;
    bool body_complete = false;
};

fiber::async::DetachedTask run_http3_gzip_client(fiber::quic::QuicUdpEndpoint *endpoint,
                                                 const fiber::net::SocketAddress *server_addr,
                                                 const std::string *cert_path,
                                                 std::promise<Http3GzipOutcome> *promise) {
    Http3GzipOutcome outcome;
    fiber::net::TlsOptions tls_material{};
    tls_material.trust_store = fiber::net::TlsTrustStoreSource::from_file(*cert_path);
    auto tls_context = fiber::net::TlsContext::create(tls_material);
    if (!tls_context) {
        outcome.error = tls_context.error();
        promise->set_value(std::move(outcome));
        co_return;
    }
    fiber::http::Http3Client::Options client_options;
    client_options.tls.context = tls_context->get();
    client_options.tls.verify_peer = true;
    fiber::http::Http3Client client(*endpoint, std::move(client_options));

    auto endpoint_started = endpoint->start();
    if (!endpoint_started) {
        outcome.error = endpoint_started.error();
        promise->set_value(std::move(outcome));
        co_return;
    }
    auto initialized = client.init();
    if (!initialized) {
        outcome.error = initialized.error();
        promise->set_value(std::move(outcome));
        co_return;
    }

    fiber::quic::QuicClientConnectOptions connect_options;
    connect_options.remote_addr = *server_addr;
    connect_options.server_name = "localhost";
    connect_options.handshake_timeout = 2s;
    auto connected = co_await client.connect(std::move(connect_options));
    if (!connected) {
        outcome.error = connected.error().io_error;
        promise->set_value(std::move(outcome));
        co_return;
    }

    {
        fiber::mem::BufPool pool;
        fiber::http::HttpHeaders headers(pool);
        headers.set("Accept-Encoding", "gzip");
        fiber::http::ClientHttp3Exchange exchange = connected->open_exchange(pool);
        auto send_result = co_await exchange.send_request_header(
                {
                        .method = fiber::http::HttpMethod::Get,
                        .scheme = "https",
                        .authority = "localhost",
                        .path = "/gzip",
                        .headers = &headers,
                },
                true, 2s);
        if (!send_result) {
            outcome.error = send_result.error();
        } else {
            auto header_result = co_await exchange.read_header(2s);
            if (!header_result || *header_result == nullptr) {
                outcome.error = header_result ? fiber::common::IoErr::Invalid : header_result.error();
            } else {
                outcome.status_code = (*header_result)->status_code;
                outcome.content_encoding = std::string((*header_result)->headers.get("content-encoding"));
                outcome.content_length = std::string((*header_result)->headers.get("content-length"));
            }
        }
        while (outcome.error == fiber::common::IoErr::None && !outcome.body_complete) {
            auto body_result = co_await exchange.read_body(64 * 1024, 2s);
            if (!body_result) {
                outcome.error = body_result.error();
                break;
            }
            outcome.body_complete = body_result->complete();
            outcome.body.append(chain_to_string(std::move(*body_result)));
        }
        outcome.request_outcome = exchange.outcome();
    }

    connected->shutdown(fiber::http::Http3ErrorCode::NoError);
    *connected = fiber::http::Http3ClientConnection{};
    endpoint->close();
    promise->set_value(std::move(outcome));
}

class RuntimeHarness {
public:
    explicit RuntimeHarness(const fiber::lite_nginx::runtime::RuntimeConfig &runtime) : launcher_(loop_) {
        auto resolver_config = fiber::dns::load_system_resolver_config();
        if (!resolver_config) {
            ADD_FAILURE() << fiber::dns::resolver_config_error_name(resolver_config.error().code);
            return;
        }
        auto start_result = launcher_.start(runtime, *resolver_config);
        if (!start_result.has_value()) {
            ADD_FAILURE() << start_result.error().message;
            return;
        }
        if (launcher_.bound_listeners().size() != 1U) {
            ADD_FAILURE() << "unexpected listener count";
            return;
        }
        thread_ = std::thread([this]() { loop_.run(); });
    }

    ~RuntimeHarness() {
        ShutdownOp shutdown{
                .launcher = &launcher_,
                .loop = &loop_,
        };
        loop_.post<ShutdownOp, &ShutdownOp::entry, &ShutdownOp::on_run>(shutdown);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::uint16_t port() const { return launcher_.bound_listeners().front().address.port(); }

private:
    fiber::event::EventLoop loop_;
    fiber::lite_nginx::runtime::ServerLauncher launcher_;
    std::thread thread_{};
};

struct StagedProxyPassOutcome {
    std::string error;
    std::string response;
    bool first_body_arrived_before_tail = false;
};

StagedProxyPassOutcome run_staged_proxy_pass(std::string_view options, bool gzip = false) {
    StagedProxyPassOutcome outcome;
    std::string source = "directive svc = http \"@backend\";\nsvc.proxyPass(";
    source.append(options);
    source.append(");");
    TestScriptFile script("http_proxy_pass_buffering", source);
    if (!script.ok()) {
        outcome.error = "create script failed";
        return outcome;
    }

    StagedResponseUpstream upstream("HTTP/1.1 200 OK\r\nContent-Length: 11\r\nContent-Type: text/plain\r\n\r\nfirst",
                                    "second");
    if (upstream.port() == 0) {
        outcome.error = "create staged upstream failed";
        return outcome;
    }

    const std::uint16_t port = reserve_loopback_port();
    if (port == 0) {
        outcome.error = "reserve listener port failed";
        return outcome;
    }

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    connection_pool { keepalive_size 8; keepalive_timeout 30s; }
    upstream backend { server 127.0.0.1:UPSTREAM_PORT; }
    server {
        server_name localhost;
        location /* {
            gzip GZIP_ENABLED;
            gzip_types text/plain;
            gzip_min_length 1;
            script_file SCRIPT_PATH;
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("UPSTREAM_PORT"), sizeof("UPSTREAM_PORT") - 1,
                        std::to_string(upstream.port()));
    config_text.replace(config_text.find("SCRIPT_PATH"), sizeof("SCRIPT_PATH") - 1, script.path());
    config_text.replace(config_text.find("GZIP_ENABLED"), sizeof("GZIP_ENABLED") - 1, gzip ? "on" : "off");

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "proxy_pass_buffering.conf");
    if (!config) {
        outcome.error = config.error().message;
        return outcome;
    }
    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    if (!runtime) {
        outcome.error = runtime.error().message;
        return outcome;
    }

    RuntimeHarness harness(*runtime);
    const int client = connect_client(harness.port());
    if (client < 0) {
        outcome.error = "connect downstream failed";
        return outcome;
    }

    const std::string_view request = gzip ? "GET /stream HTTP/1.1\r\n"
                                            "Host: localhost\r\n"
                                            "Accept-Encoding: gzip\r\n"
                                            "Connection: close\r\n\r\n"
                                          : "GET /stream HTTP/1.1\r\n"
                                            "Host: localhost\r\n"
                                            "Connection: close\r\n\r\n";
    if (!send_all(client, request)) {
        outcome.error = "send downstream request failed";
        ::close(client);
        return outcome;
    }
    if (!upstream.wait_for_first_segment(3s)) {
        outcome.error = "upstream first segment was not sent";
        upstream.release_tail();
        ::close(client);
        return outcome;
    }

    outcome.response = recv_until_contains(client, "\r\n\r\n");
    const std::size_t header_end = outcome.response.find("\r\n\r\n");
    if ((header_end == std::string::npos || outcome.response.size() == header_end + 4) &&
        socket_readable(client, 500ms)) {
        outcome.response.append(recv_available(client));
    }
    outcome.first_body_arrived_before_tail =
            header_end != std::string::npos && outcome.response.size() > header_end + 4;

    upstream.release_tail();
    if (gzip) {
        outcome.response.append(recv_until_close(client));
    } else {
        outcome.response.append(recv_until_contains(client, "second"));
    }
    ::close(client);
    return outcome;
}

StagedProxyPassOutcome run_staged_direct_chunked_proxy(bool buffering) {
    StagedProxyPassOutcome outcome;
    StagedResponseUpstream upstream(
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Type: text/plain\r\n\r\n5\r\nfirst\r\n",
            "6\r\nsecond\r\n0\r\n\r\n");
    if (upstream.port() == 0) {
        outcome.error = "create staged upstream failed";
        return outcome;
    }

    const std::uint16_t port = reserve_loopback_port();
    if (port == 0) {
        outcome.error = "reserve listener port failed";
        return outcome;
    }

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    server {
        server_name localhost;
        location /* {
            gzip on;
            gzip_types text/plain;
            gzip_min_length 1;
            proxy_buffering BUFFERING;
            proxy_pass http://127.0.0.1:UPSTREAM_PORT;
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("UPSTREAM_PORT"), sizeof("UPSTREAM_PORT") - 1,
                        std::to_string(upstream.port()));
    config_text.replace(config_text.find("BUFFERING"), sizeof("BUFFERING") - 1, buffering ? "on" : "off");

    auto config = ConfigLoader::load_from_string(config_text, "gzip_staged_direct_proxy.conf");
    if (!config) {
        outcome.error = config.error().message;
        return outcome;
    }
    auto runtime = RuntimeBuilder::build(*config);
    if (!runtime) {
        outcome.error = runtime.error().message;
        return outcome;
    }

    RuntimeHarness harness(*runtime);
    const int client = connect_client(harness.port());
    if (client < 0) {
        outcome.error = "connect downstream failed";
        return outcome;
    }
    static constexpr std::string_view kRequest = "GET /stream HTTP/1.1\r\n"
                                                 "Host: localhost\r\n"
                                                 "Accept-Encoding: gzip\r\n"
                                                 "Connection: close\r\n\r\n";
    if (!send_all(client, kRequest)) {
        outcome.error = "send downstream request failed";
        ::close(client);
        return outcome;
    }
    if (!upstream.wait_for_first_segment(3s)) {
        outcome.error = "upstream first segment was not sent";
        upstream.release_tail();
        ::close(client);
        return outcome;
    }

    outcome.response = recv_until_contains(client, "\r\n\r\n");
    const std::size_t header_end = outcome.response.find("\r\n\r\n");
    if ((header_end == std::string::npos || outcome.response.size() == header_end + 4) &&
        socket_readable(client, 500ms)) {
        outcome.response.append(recv_available(client));
    }
    outcome.first_body_arrived_before_tail =
            header_end != std::string::npos && outcome.response.size() > header_end + 4;

    upstream.release_tail();
    outcome.response.append(recv_until_close(client));
    ::close(client);
    return outcome;
}

TEST(LiteNginxRuntimeTest, RejectsDuplicateServerNames) {
    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(R"(
worker_processes 1;
http {
    listen 127.0.0.1:8080;

    server {
        server_name same.test;
        location /* {
            proxy_pass http://127.0.0.1:9001;
        }
    }

    server {
        server_name same.test;
        location /* {
            proxy_pass http://127.0.0.1:9002;
        }
    }
}
)",
                                                                            "dup_server_name.conf");
    ASSERT_TRUE(config.has_value());

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_FALSE(runtime.has_value());
    EXPECT_NE(runtime.error().message.find("duplicate server_name"), std::string::npos);
}

TEST(LiteNginxRuntimeTest, BuildsHttp3ListenerRuntime) {
    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(R"(
worker_processes 1;
http {
    listen 127.0.0.1:8443 ssl http3;

    server {
        server_name localhost;
        certificate /tmp/localhost.crt;
        certificate_key /tmp/localhost.key;
        location /* {
            proxy_pass http://127.0.0.1:9001;
        }
    }
}
)",
                                                                            "http3_listener.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;
    ASSERT_EQ(runtime->listeners.size(), 1u);
    EXPECT_TRUE(runtime->listeners[0].tls);
    EXPECT_TRUE(runtime->listeners[0].http3);
    EXPECT_EQ(runtime->listeners[0].http3_alt_svc, "h3=\":8443\"; ma=86400");
}

TEST(LiteNginxRuntimeTest, BuildsProxyBufferingRuntimeAndSixtySecondDefaults) {
    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(R"(
http {
    listen 127.0.0.1:8080;

    server {
        server_name localhost;
        proxy_buffering 128k 32k;

        location /buffered {
            proxy_pass http://127.0.0.1:9001;
        }

        location /stream {
            proxy_pass http://127.0.0.1:9001;
            proxy_buffering off;
            client_max_body_size 0;
        }

        client_max_body_size 2m;
    }
}
)",
                                                                            "buffering_runtime.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;
    ASSERT_EQ(runtime->servers.size(), 1u);
    ASSERT_EQ(runtime->servers[0].locations.size(), 2u);

    const auto &buffered = runtime->servers[0].locations[0];
    EXPECT_TRUE(buffered.buffering.enabled());
    EXPECT_EQ(buffered.buffering.buffer_size, 128u * 1024u);
    EXPECT_EQ(buffered.buffering.low_water, 32u * 1024u);
    EXPECT_EQ(buffered.read_timeout, std::chrono::seconds(60));
    EXPECT_EQ(buffered.send_timeout, std::chrono::seconds(60));
    EXPECT_EQ(buffered.client_max_body_size, 2u * 1024u * 1024u);

    const auto &stream = runtime->servers[0].locations[1];
    EXPECT_FALSE(stream.buffering.enabled());
    EXPECT_EQ(stream.buffering.low_water, 0u);
    EXPECT_EQ(stream.client_max_body_size, 0u);
}

TEST(LiteNginxRuntimeTest, CompilesAccessLogInheritance) {
    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(R"(
http {
    listen 127.0.0.1:8080;
    access_log http.access "http ${$req.method}";
    server {
        server_name localhost;
        access_log off;
        location /quiet { proxy_pass http://127.0.0.1:9001; }
        location /logged/:id {
            access_log location.access "location=${$access.location} id=${$path.id}";
            proxy_pass http://127.0.0.1:9001;
        }
    }
}
)",
                                                                            "access_inheritance.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;
    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;
    ASSERT_EQ(runtime->access_logs.size(), 2u);
    EXPECT_EQ(runtime->access_log, 0u);
    EXPECT_EQ(runtime->access_logs[0].logger_name, "http.access");
    EXPECT_TRUE(runtime->access_logs[0].template_script != nullptr);
    ASSERT_EQ(runtime->servers.size(), 1u);
    EXPECT_EQ(runtime->servers[0].access_log, fiber::lite_nginx::runtime::kDisabledAccessLog);
    ASSERT_EQ(runtime->servers[0].locations.size(), 2u);
    EXPECT_EQ(runtime->servers[0].locations[0].access_log, fiber::lite_nginx::runtime::kDisabledAccessLog);
    EXPECT_EQ(runtime->servers[0].locations[1].access_log, 1u);
    EXPECT_EQ(runtime->access_logs[1].logger_name, "location.access");
    EXPECT_TRUE(runtime->access_logs[1].template_script != nullptr);
}

TEST(LiteNginxRuntimeTest, RejectsAsyncAccessLogTemplate) {
    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(R"(
http {
    listen 127.0.0.1:8080;
    access_log http.access "body=${req.readJson()}";
    server {
        server_name localhost;
        location / { proxy_pass http://127.0.0.1:9001; }
    }
}
)",
                                                                            "async_access_log.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;
    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_FALSE(runtime.has_value());
    EXPECT_NE(runtime.error().message.find("must be synchronous"), std::string::npos);
}

TEST(LiteNginxRuntimeTest, ProxiesDirectRouteMatcherLocation) {
    std::promise<std::string> upstream_request;
    auto upstream_future = upstream_request.get_future();
    SingleRequestUpstream upstream("HTTP/1.1 200 OK\r\nContent-Length: 7\r\nContent-Type: text/plain\r\n\r\nproxied",
                                   &upstream_request);
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;

    server {
        server_name localhost;
        location /api/:id {
            proxy_pass http://127.0.0.1:UPSTREAM_PORT;
            proxy_set_header Host backend.internal;
            proxy_buffering 64k 48k;
        }
    }
}
)";
    std::size_t listen_marker = config_text.find("LISTEN_PORT");
    ASSERT_NE(listen_marker, std::string::npos);
    config_text.replace(listen_marker, sizeof("LISTEN_PORT") - 1, std::to_string(port));

    std::size_t upstream_marker = config_text.find("UPSTREAM_PORT");
    ASSERT_NE(upstream_marker, std::string::npos);
    config_text.replace(upstream_marker, sizeof("UPSTREAM_PORT") - 1, std::to_string(upstream.port()));

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "runtime_proxy.conf");
    ASSERT_TRUE(config.has_value());

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);

    const char request[] = "GET /api/42?x=1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));

    std::string response = recv_http_response(client);
    ::close(client);

    ASSERT_EQ(upstream_future.wait_for(3s), std::future_status::ready);
    std::string proxied_request = upstream_future.get();

    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(response.find("proxied"), std::string::npos);
    EXPECT_NE(proxied_request.find("GET /api/42?x=1 HTTP/1.1\r\n"), std::string::npos);
    EXPECT_NE(proxied_request.find("Host: backend.internal\r\n"), std::string::npos);
    EXPECT_EQ(proxied_request.find("Connection: close\r\n"), std::string::npos);
}

TEST(LiteNginxRuntimeTest, RewritePathSupportsLiteralAndTemplateWhilePreservingQuery) {
    std::promise<std::string> literal_upstream_request;
    std::promise<std::string> template_upstream_request;
    auto literal_future = literal_upstream_request.get_future();
    auto template_future = template_upstream_request.get_future();
    SingleRequestUpstream literal_upstream("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", &literal_upstream_request);
    SingleRequestUpstream template_upstream("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok",
                                            &template_upstream_request);
    ASSERT_NE(literal_upstream.port(), 0);
    ASSERT_NE(template_upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);
    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    server {
        server_name localhost;
        location /literal/*tail {
            proxy_pass http://127.0.0.1:LITERAL_PORT;
            rewrite_path /v2/static;
        }
        location /template/*tail {
            proxy_pass http://127.0.0.1:TEMPLATE_PORT;
            rewrite_path "/v3/${$path.tail}";
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("LITERAL_PORT"), sizeof("LITERAL_PORT") - 1,
                        std::to_string(literal_upstream.port()));
    config_text.replace(config_text.find("TEMPLATE_PORT"), sizeof("TEMPLATE_PORT") - 1,
                        std::to_string(template_upstream.port()));

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "rewrite_path.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;
    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;
    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    static constexpr std::string_view kLiteralRequest =
            "GET /literal/ignored?first=1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_TRUE(send_all(client, kLiteralRequest));
    const std::string literal_response = recv_http_response(client);
    ::close(client);

    client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    static constexpr std::string_view kTemplateRequest =
            "GET /template/a/b?x=%2F&empty= HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_TRUE(send_all(client, kTemplateRequest));
    const std::string template_response = recv_http_response(client);
    ::close(client);

    ASSERT_EQ(literal_future.wait_for(3s), std::future_status::ready);
    ASSERT_EQ(template_future.wait_for(3s), std::future_status::ready);
    const std::string literal_request = literal_future.get();
    const std::string template_request = template_future.get();

    EXPECT_NE(literal_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << literal_response;
    EXPECT_NE(template_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << template_response;
    EXPECT_NE(literal_request.find("GET /v2/static?first=1 HTTP/1.1\r\n"), std::string::npos) << literal_request;
    EXPECT_NE(template_request.find("GET /v3/a/b?x=%2F&empty= HTTP/1.1\r\n"), std::string::npos) << template_request;
}

TEST(LiteNginxRuntimeTest, RejectsInvalidDynamicRewriteBeforeConnectingUpstream) {
    std::promise<std::string> upstream_request;
    SingleRequestUpstream upstream("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", &upstream_request);
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);
    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    server {
        server_name localhost;
        location / {
            proxy_pass http://127.0.0.1:UPSTREAM_PORT;
            rewrite_path "${$header.x_path}";
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("UPSTREAM_PORT"), sizeof("UPSTREAM_PORT") - 1,
                        std::to_string(upstream.port()));

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "invalid_rewrite_path.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;
    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;
    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    static constexpr std::string_view kRequest =
            "GET / HTTP/1.1\r\nHost: localhost\r\nX-Path: relative\r\nConnection: close\r\n\r\n";
    ASSERT_TRUE(send_all(client, kRequest));
    const std::string response = recv_http_response(client);
    ::close(client);

    EXPECT_NE(response.find("HTTP/1.1 500 Internal Server Error\r\n"), std::string::npos) << response;
}

TEST(LiteNginxRuntimeTest, ClosesUpstreamWhenClientAbortsIfConfigured) {
    std::promise<void> upstream_accepted;
    std::promise<bool> upstream_closed;
    auto accepted_future = upstream_accepted.get_future();
    auto closed_future = upstream_closed.get_future();
    AbortAwareUpstream upstream(&upstream_accepted, &upstream_closed);
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);
    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    server {
        server_name localhost;
        proxy_close_on_client_abort on;
        location /* {
            proxy_pass http://127.0.0.1:UPSTREAM_PORT;
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("UPSTREAM_PORT"), sizeof("UPSTREAM_PORT") - 1,
                        std::to_string(upstream.port()));

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "client_abort.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;
    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;
    EXPECT_TRUE(runtime->servers[0].locations[0].close_on_client_abort);
    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    static constexpr std::string_view kRequest = "GET /slow HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_TRUE(send_all(client, kRequest));
    if (accepted_future.wait_for(3s) != std::future_status::ready) {
        const std::string response = recv_http_response(client);
        FAIL() << "upstream was not connected; downstream response: " << response;
    }

    linger reset_on_close{1, 0};
    ASSERT_EQ(::setsockopt(client, SOL_SOCKET, SO_LINGER, &reset_on_close, sizeof(reset_on_close)), 0);
    ::close(client);

    ASSERT_EQ(closed_future.wait_for(3s), std::future_status::ready);
    EXPECT_TRUE(closed_future.get());
}

TEST(LiteNginxRuntimeTest, ProxiesHttp1WebSocketUpgradeByDefault) {
    std::promise<std::string> upstream_request;
    std::promise<std::string> upstream_body;
    auto request_future = upstream_request.get_future();
    auto body_future = upstream_body.get_future();
    WebSocketUpstream upstream(&upstream_request, &upstream_body);
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);
    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    server {
        server_name localhost;
        location /* {
            proxy_pass http://127.0.0.1:UPSTREAM_PORT;
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("UPSTREAM_PORT"), sizeof("UPSTREAM_PORT") - 1,
                        std::to_string(upstream.port()));

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "websocket_proxy.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;
    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);
    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);

    static constexpr std::string_view kRequest = "GET /chat HTTP/1.1\r\n"
                                                 "Host: localhost\r\n"
                                                 "Connection: keep-alive, Upgrade\r\n"
                                                 "Upgrade: WebSocket\r\n"
                                                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                                 "Sec-WebSocket-Version: 13\r\n"
                                                 "Sec-WebSocket-Protocol: chat\r\n"
                                                 "\r\n";
    ASSERT_TRUE(send_all(client, kRequest));

    std::string response = recv_until_contains(client, "server-frame");
    EXPECT_NE(response.find("HTTP/1.1 101 Switching Protocols\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("Connection: Upgrade\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("Upgrade: websocket\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("Sec-WebSocket-Protocol: chat\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("\r\n\r\nserver-frame"), std::string::npos) << response;

    ASSERT_TRUE(send_all(client, "client-frame"));
    ASSERT_EQ(::shutdown(client, SHUT_WR), 0);

    ASSERT_EQ(request_future.wait_for(3s), std::future_status::ready);
    ASSERT_EQ(body_future.wait_for(3s), std::future_status::ready);
    const std::string proxied_request = request_future.get();
    EXPECT_EQ(body_future.get(), "client-frame");
    EXPECT_NE(proxied_request.find("GET /chat HTTP/1.1\r\n"), std::string::npos) << proxied_request;
    EXPECT_NE(proxied_request.find("Connection: Upgrade\r\n"), std::string::npos) << proxied_request;
    EXPECT_NE(proxied_request.find("Upgrade: websocket\r\n"), std::string::npos) << proxied_request;
    EXPECT_NE(proxied_request.find("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"), std::string::npos)
            << proxied_request;
    EXPECT_EQ(proxied_request.find("keep-alive"), std::string::npos) << proxied_request;

    ::close(client);
}

TEST(LiteNginxRuntimeTest, ScriptProxyPassProxiesHttp1WebSocketWhenEnabled) {
    const std::string script_path = "/tmp/lite_nginx_http1_websocket_script_proxy_test.js";
    {
        std::ofstream file(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.good());
        file << "directive svc = http \"@backend\";\n"
                "svc.proxyPass({websocket: true, "
                "headers: {Connection: null, Upgrade: \"broken\", \"Sec-WebSocket-Key\": \"broken\"}, "
                "responseHeaders: {Connection: null, Upgrade: \"broken\", "
                "\"Sec-WebSocket-Accept\": \"broken\", \"X-Script-Proxy\": \"yes\"}});";
    }

    std::promise<std::string> upstream_request;
    std::promise<std::string> upstream_body;
    auto request_future = upstream_request.get_future();
    auto body_future = upstream_body.get_future();
    WebSocketUpstream upstream(&upstream_request, &upstream_body);
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);
    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    connection_pool { keepalive_size 8; keepalive_timeout 30s; }
    upstream backend { server 127.0.0.1:UPSTREAM_PORT; }
    server {
        server_name localhost;
        location /* { script_file SCRIPT_PATH; }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("UPSTREAM_PORT"), sizeof("UPSTREAM_PORT") - 1,
                        std::to_string(upstream.port()));
    config_text.replace(config_text.find("SCRIPT_PATH"), sizeof("SCRIPT_PATH") - 1, script_path);

    auto config =
            fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "http1_websocket_script_proxy.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;
    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);
    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);

    static constexpr std::string_view kRequest = "GET /chat HTTP/1.1\r\n"
                                                 "Host: localhost\r\n"
                                                 "Connection: keep-alive, Upgrade\r\n"
                                                 "Upgrade: WebSocket\r\n"
                                                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                                 "Sec-WebSocket-Version: 13\r\n"
                                                 "Sec-WebSocket-Protocol: chat\r\n"
                                                 "\r\n";
    ASSERT_TRUE(send_all(client, kRequest));

    std::string response = recv_until_contains(client, "server-frame");
    EXPECT_NE(response.find("HTTP/1.1 101 Switching Protocols\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("Connection: Upgrade\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("Upgrade: websocket\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("X-Script-Proxy: yes\r\n"), std::string::npos) << response;

    ASSERT_TRUE(send_all(client, "client-frame"));
    ASSERT_EQ(::shutdown(client, SHUT_WR), 0);

    ASSERT_EQ(request_future.wait_for(3s), std::future_status::ready);
    ASSERT_EQ(body_future.wait_for(3s), std::future_status::ready);
    const std::string proxied_request = request_future.get();
    EXPECT_EQ(body_future.get(), "client-frame");
    EXPECT_NE(proxied_request.find("GET /chat HTTP/1.1\r\n"), std::string::npos) << proxied_request;
    EXPECT_NE(proxied_request.find("Connection: Upgrade\r\n"), std::string::npos) << proxied_request;
    EXPECT_NE(proxied_request.find("Upgrade: websocket\r\n"), std::string::npos) << proxied_request;
    EXPECT_NE(proxied_request.find("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"), std::string::npos)
            << proxied_request;
    EXPECT_EQ(proxied_request.find("Upgrade: broken\r\n"), std::string::npos) << proxied_request;

    ::close(client);
    ::unlink(script_path.c_str());
}

TEST(LiteNginxRuntimeTest, ProxiesHttp2WebSocketExtendedConnectByDefault) {
    TestPemFile cert("cert", kSelfSignedCertPem);
    TestPemFile key("key", kSelfSignedKeyPem);
    ASSERT_TRUE(cert.ok());
    ASSERT_TRUE(key.ok());

    std::promise<std::string> upstream_request;
    std::promise<std::string> upstream_body;
    auto request_future = upstream_request.get_future();
    auto body_future = upstream_body.get_future();
    WebSocketUpstream upstream(&upstream_request, &upstream_body);
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);
    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT ssl;
    server {
        server_name localhost;
        certificate CERT_FILE;
        certificate_key KEY_FILE;
        location /* {
            proxy_pass http://127.0.0.1:UPSTREAM_PORT;
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("UPSTREAM_PORT"), sizeof("UPSTREAM_PORT") - 1,
                        std::to_string(upstream.port()));
    config_text.replace(config_text.find("CERT_FILE"), sizeof("CERT_FILE") - 1, cert.path());
    config_text.replace(config_text.find("KEY_FILE"), sizeof("KEY_FILE") - 1, key.path());

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "h2_websocket_proxy.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;
    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);
    fiber::event::EventLoopGroup client_group(1);
    client_group.start();
    std::promise<Http2WebSocketOutcome> client_promise;
    auto client_future = client_promise.get_future();
    fiber::async::spawn(client_group.at(0), [&]() {
        return run_http2_websocket_client(&client_group.at(0), harness.port(), &client_promise);
    });

    const std::future_status client_status = client_future.wait_for(5s);
    EXPECT_EQ(client_status, std::future_status::ready);
    Http2WebSocketOutcome outcome;
    if (client_status == std::future_status::ready) {
        outcome = client_future.get();
    }
    client_group.stop();
    client_group.join();

    ASSERT_EQ(client_status, std::future_status::ready);
    EXPECT_EQ(outcome.error, fiber::common::IoErr::None);
    EXPECT_TRUE(outcome.extended_connect_enabled);
    EXPECT_EQ(outcome.status_code, 200);
    EXPECT_TRUE(outcome.connection.empty());
    EXPECT_TRUE(outcome.upgrade.empty());
    EXPECT_TRUE(outcome.accept.empty());
    EXPECT_EQ(outcome.protocol, "chat");
    EXPECT_EQ(outcome.body, "server-frame");

    ASSERT_EQ(request_future.wait_for(3s), std::future_status::ready);
    ASSERT_EQ(body_future.wait_for(3s), std::future_status::ready);
    const std::string proxied_request = request_future.get();
    EXPECT_EQ(body_future.get(), "client-frame");
    EXPECT_NE(proxied_request.find("GET /chat HTTP/1.1\r\n"), std::string::npos) << proxied_request;
    EXPECT_NE(proxied_request.find("Connection: Upgrade\r\n"), std::string::npos) << proxied_request;
    EXPECT_NE(proxied_request.find("Upgrade: websocket\r\n"), std::string::npos) << proxied_request;
    EXPECT_NE(proxied_request.find("Sec-WebSocket-Version: 13\r\n"), std::string::npos) << proxied_request;
    const std::string generated_key = http_header_value(proxied_request, "Sec-WebSocket-Key");
    EXPECT_EQ(generated_key.size(), 24U) << proxied_request;
}

TEST(LiteNginxRuntimeTest, ScriptProxyPassTranslatesHttp2WebSocketExtendedConnectWhenEnabled) {
    const std::string script_path = "/tmp/lite_nginx_http2_websocket_script_proxy_test.js";
    {
        std::ofstream file(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.good());
        file << "directive svc = http \"@backend\";\n"
                "svc.proxyPass({websocket: true, "
                "responseHeaders: {Connection: \"broken\", Upgrade: \"broken\", "
                "\"Sec-WebSocket-Accept\": \"broken\"}});";
    }

    TestPemFile cert("cert", kSelfSignedCertPem);
    TestPemFile key("key", kSelfSignedKeyPem);
    ASSERT_TRUE(cert.ok());
    ASSERT_TRUE(key.ok());

    std::promise<std::string> upstream_request;
    std::promise<std::string> upstream_body;
    auto request_future = upstream_request.get_future();
    auto body_future = upstream_body.get_future();
    WebSocketUpstream upstream(&upstream_request, &upstream_body);
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);
    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT ssl;
    connection_pool { keepalive_size 8; keepalive_timeout 30s; }
    upstream backend { server 127.0.0.1:UPSTREAM_PORT; }
    server {
        server_name localhost;
        certificate CERT_FILE;
        certificate_key KEY_FILE;
        location /* { script_file SCRIPT_PATH; }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("UPSTREAM_PORT"), sizeof("UPSTREAM_PORT") - 1,
                        std::to_string(upstream.port()));
    config_text.replace(config_text.find("CERT_FILE"), sizeof("CERT_FILE") - 1, cert.path());
    config_text.replace(config_text.find("KEY_FILE"), sizeof("KEY_FILE") - 1, key.path());
    config_text.replace(config_text.find("SCRIPT_PATH"), sizeof("SCRIPT_PATH") - 1, script_path);

    auto config =
            fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "http2_websocket_script_proxy.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;
    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);
    fiber::event::EventLoopGroup client_group(1);
    client_group.start();
    std::promise<Http2WebSocketOutcome> client_promise;
    auto client_future = client_promise.get_future();
    fiber::async::spawn(client_group.at(0), [&]() {
        return run_http2_websocket_client(&client_group.at(0), harness.port(), &client_promise);
    });

    const std::future_status client_status = client_future.wait_for(5s);
    EXPECT_EQ(client_status, std::future_status::ready);
    Http2WebSocketOutcome outcome;
    if (client_status == std::future_status::ready) {
        outcome = client_future.get();
    }
    client_group.stop();
    client_group.join();

    ASSERT_EQ(client_status, std::future_status::ready);
    EXPECT_EQ(outcome.error, fiber::common::IoErr::None);
    EXPECT_TRUE(outcome.extended_connect_enabled);
    EXPECT_EQ(outcome.status_code, 200);
    EXPECT_TRUE(outcome.connection.empty());
    EXPECT_TRUE(outcome.upgrade.empty());
    EXPECT_TRUE(outcome.accept.empty());
    EXPECT_EQ(outcome.protocol, "chat");
    EXPECT_EQ(outcome.body, "server-frame");

    ASSERT_EQ(request_future.wait_for(3s), std::future_status::ready);
    ASSERT_EQ(body_future.wait_for(3s), std::future_status::ready);
    const std::string proxied_request = request_future.get();
    EXPECT_EQ(body_future.get(), "client-frame");
    EXPECT_NE(proxied_request.find("GET /chat HTTP/1.1\r\n"), std::string::npos) << proxied_request;
    EXPECT_NE(proxied_request.find("Connection: Upgrade\r\n"), std::string::npos) << proxied_request;
    EXPECT_NE(proxied_request.find("Upgrade: websocket\r\n"), std::string::npos) << proxied_request;
    EXPECT_NE(proxied_request.find("Sec-WebSocket-Version: 13\r\n"), std::string::npos) << proxied_request;
    EXPECT_EQ(http_header_value(proxied_request, "Sec-WebSocket-Key").size(), 24U) << proxied_request;

    ::unlink(script_path.c_str());
}

TEST(LiteNginxRuntimeTest, ProxyRegeneratesRequestContentLength) {
    std::promise<std::string> upstream_request;
    auto upstream_future = upstream_request.get_future();
    SingleRequestUpstream upstream("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", &upstream_request);
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;

    server {
        server_name localhost;
        location /* {
            proxy_pass http://127.0.0.1:UPSTREAM_PORT;
            proxy_set_header Content-Length 99;
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("UPSTREAM_PORT"), sizeof("UPSTREAM_PORT") - 1,
                        std::to_string(upstream.port()));

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "request_framing.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);
    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);

    const char request[] = "POST /upload HTTP/1.1\r\n"
                           "Host: localhost\r\n"
                           "Content-Length: 5\r\n"
                           "Connection: close\r\n"
                           "\r\n"
                           "hello";
    ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));

    std::string response = recv_http_response(client);
    ::close(client);

    ASSERT_EQ(upstream_future.wait_for(3s), std::future_status::ready);
    const std::string proxied_request = upstream_future.get();
    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << response;
    EXPECT_NE(proxied_request.find("\r\n\r\nhello"), std::string::npos) << proxied_request;
    const std::size_t content_length = proxied_request.find("Content-Length: 5\r\n");
    ASSERT_NE(content_length, std::string::npos) << proxied_request;
    EXPECT_EQ(proxied_request.find("Content-Length:", content_length + 1), std::string::npos) << proxied_request;
    EXPECT_EQ(proxied_request.find("Content-Length: 99\r\n"), std::string::npos) << proxied_request;
    EXPECT_EQ(proxied_request.find("Transfer-Encoding:"), std::string::npos) << proxied_request;
}

TEST(LiteNginxRuntimeTest, ProxyTerminatesExpectContinueAndAllowsBodyAtExactLimit) {
    std::promise<std::string> upstream_request;
    auto upstream_future = upstream_request.get_future();
    SingleRequestUpstream upstream("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", &upstream_request);
    ASSERT_NE(upstream.port(), 0);

    const std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    client_max_body_size 5;

    server {
        server_name localhost;
        location /* {
            proxy_pass http://127.0.0.1:UPSTREAM_PORT;
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("UPSTREAM_PORT"), sizeof("UPSTREAM_PORT") - 1,
                        std::to_string(upstream.port()));

    auto config = ConfigLoader::load_from_string(config_text, "expect_continue.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;
    auto runtime = RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);
    const int client = connect_client(harness.port());
    ASSERT_GE(client, 0);

    const char headers[] = "POST /upload HTTP/1.1\r\n"
                           "Host: localhost\r\n"
                           "Expect: 100-continue\r\n"
                           "Content-Length: 5\r\n"
                           "Connection: close\r\n"
                           "\r\n";
    ASSERT_EQ(::send(client, headers, sizeof(headers) - 1, 0), static_cast<ssize_t>(sizeof(headers) - 1));
    const std::string informational = recv_until_contains(client, "\r\n\r\n");
    ASSERT_EQ(informational, "HTTP/1.1 100 Continue\r\n\r\n");

    ASSERT_TRUE(send_all(client, "hello"));
    const std::string response = recv_http_response(client);
    ::close(client);

    ASSERT_EQ(upstream_future.wait_for(3s), std::future_status::ready);
    const std::string proxied_request = upstream_future.get();
    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << response;
    EXPECT_NE(proxied_request.find("\r\n\r\nhello"), std::string::npos) << proxied_request;
    EXPECT_EQ(proxied_request.find("Expect:"), std::string::npos) << proxied_request;
}

TEST(LiteNginxRuntimeTest, RejectsKnownOversizedBodyBeforeContinueOrUpstreamConnect) {
    const std::uint16_t port = reserve_loopback_port();
    const std::uint16_t unavailable_upstream_port = reserve_loopback_port();
    ASSERT_NE(port, 0);
    ASSERT_NE(unavailable_upstream_port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;

    server {
        server_name localhost;
        location /* {
            client_max_body_size 5;
            proxy_pass http://127.0.0.1:UPSTREAM_PORT;
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("UPSTREAM_PORT"), sizeof("UPSTREAM_PORT") - 1,
                        std::to_string(unavailable_upstream_port));

    auto config = ConfigLoader::load_from_string(config_text, "known_oversized_body.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;
    auto runtime = RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);
    const int client = connect_client(harness.port());
    ASSERT_GE(client, 0);

    const char request[] = "POST /upload HTTP/1.1\r\n"
                           "Host: localhost\r\n"
                           "Expect: 100-continue\r\n"
                           "Content-Length: 6\r\n"
                           "\r\n";
    ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));

    const std::string response = recv_http_response(client);
    ::close(client);

    EXPECT_NE(response.find("HTTP/1.1 413 Payload Too Large\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("Connection: close\r\n"), std::string::npos) << response;
    EXPECT_EQ(response.find("100 Continue"), std::string::npos) << response;
    EXPECT_EQ(response.find("502 Bad Gateway"), std::string::npos) << response;
}

TEST(LiteNginxRuntimeTest, RejectsOversizedChunkedProxyBodyIncrementally) {
    std::promise<std::string> upstream_request;
    auto upstream_future = upstream_request.get_future();
    SingleRequestUpstream upstream("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", &upstream_request);
    ASSERT_NE(upstream.port(), 0);

    const std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);
    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    server {
        server_name localhost;
        location /* {
            client_max_body_size 5;
            proxy_pass http://127.0.0.1:UPSTREAM_PORT;
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("UPSTREAM_PORT"), sizeof("UPSTREAM_PORT") - 1,
                        std::to_string(upstream.port()));

    auto config = ConfigLoader::load_from_string(config_text, "chunked_oversized_body.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;
    auto runtime = RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);
    const int client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char request[] = "POST /upload HTTP/1.1\r\n"
                           "Host: localhost\r\n"
                           "Transfer-Encoding: chunked\r\n"
                           "Connection: close\r\n"
                           "\r\n"
                           "6\r\nabcdef\r\n"
                           "0\r\n\r\n";
    ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));

    const std::string response = recv_http_response(client);
    ::close(client);

    ASSERT_EQ(upstream_future.wait_for(3s), std::future_status::ready);
    const std::string proxied_request = upstream_future.get();
    EXPECT_NE(response.find("HTTP/1.1 413 Payload Too Large\r\n"), std::string::npos) << response;
    EXPECT_EQ(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << response;
    EXPECT_EQ(proxied_request.find("Expect:"), std::string::npos) << proxied_request;
}

TEST(LiteNginxRuntimeTest, SuppressesOverriddenAndConnectionDeclaredRequestHeaders) {
    std::promise<std::string> upstream_request;
    auto upstream_future = upstream_request.get_future();
    SingleRequestUpstream upstream("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Type: text/plain\r\n\r\nok",
                                   &upstream_request);
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;

    server {
        server_name localhost;
        location /* {
            proxy_pass http://127.0.0.1:UPSTREAM_PORT;
            proxy_set_header Host backend.internal;
            proxy_set_header X-Test replaced;
        }
    }
}
)";
    std::size_t marker = config_text.find("LISTEN_PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("LISTEN_PORT") - 1, std::to_string(port));

    marker = config_text.find("UPSTREAM_PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("UPSTREAM_PORT") - 1, std::to_string(upstream.port()));

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "runtime_skip_headers.conf");
    ASSERT_TRUE(config.has_value());

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);

    const char request[] = "GET / HTTP/1.1\r\n"
                           "Host: localhost\r\n"
                           "Connection: close, x-hop\r\n"
                           "X-Hop: drop-me\r\n"
                           "X-Test: original\r\n"
                           "X-Preserve: keep-me\r\n"
                           "\r\n";
    ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));

    std::string response = recv_http_response(client);
    ::close(client);

    ASSERT_EQ(upstream_future.wait_for(3s), std::future_status::ready);
    std::string proxied_request = upstream_future.get();

    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(proxied_request.find("Host: backend.internal\r\n"), std::string::npos);
    EXPECT_NE(proxied_request.find("X-Test: replaced\r\n"), std::string::npos);
    EXPECT_NE(proxied_request.find("X-Preserve: keep-me\r\n"), std::string::npos);
    EXPECT_EQ(proxied_request.find("Host: localhost\r\n"), std::string::npos);
    EXPECT_EQ(proxied_request.find("Connection: close"), std::string::npos);
    EXPECT_EQ(proxied_request.find("X-Hop: drop-me\r\n"), std::string::npos);
    EXPECT_EQ(proxied_request.find("X-Test: original\r\n"), std::string::npos);
}

TEST(LiteNginxRuntimeTest, RoutesNamedUpstreamAndSelectsServerByHost) {
    std::promise<std::string> api_request;
    auto api_future = api_request.get_future();
    SingleRequestUpstream api_upstream("HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Type: text/plain\r\n\r\napi",
                                       &api_request);
    ASSERT_NE(api_upstream.port(), 0);

    std::promise<std::string> other_request;
    auto other_future = other_request.get_future();
    SingleRequestUpstream other_upstream(
            "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain\r\n\r\nother", &other_request);
    ASSERT_NE(other_upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;

    upstream backend {
        server 127.0.0.1:API_UPSTREAM_PORT;
    }

    server {
        server_name api.local;
        location /files/*tail {
            proxy_pass upstream://backend;
        }
    }

    server {
        server_name other.local;
        location /* {
            proxy_pass http://127.0.0.1:OTHER_UPSTREAM_PORT;
        }
    }
}
)";
    std::size_t marker = config_text.find("LISTEN_PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("LISTEN_PORT") - 1, std::to_string(port));

    marker = config_text.find("API_UPSTREAM_PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("API_UPSTREAM_PORT") - 1, std::to_string(api_upstream.port()));

    marker = config_text.find("OTHER_UPSTREAM_PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("OTHER_UPSTREAM_PORT") - 1, std::to_string(other_upstream.port()));

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "runtime_named_upstream.conf");
    ASSERT_TRUE(config.has_value());

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char api_request_text[] = "GET /files/a/b HTTP/1.1\r\nHost: api.local\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, api_request_text, sizeof(api_request_text) - 1, 0),
              static_cast<ssize_t>(sizeof(api_request_text) - 1));
    std::string api_response = recv_http_response(client);
    ::close(client);

    client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char other_request_text[] = "GET /whatever HTTP/1.1\r\nHost: other.local\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, other_request_text, sizeof(other_request_text) - 1, 0),
              static_cast<ssize_t>(sizeof(other_request_text) - 1));
    std::string other_response = recv_http_response(client);
    ::close(client);

    ASSERT_EQ(api_future.wait_for(3s), std::future_status::ready);
    ASSERT_EQ(other_future.wait_for(3s), std::future_status::ready);
    std::string proxied_api_request = api_future.get();
    std::string proxied_other_request = other_future.get();

    EXPECT_NE(api_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(api_response.find("api"), std::string::npos);
    EXPECT_NE(other_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(other_response.find("other"), std::string::npos);
    EXPECT_NE(proxied_api_request.find("GET /files/a/b HTTP/1.1\r\n"), std::string::npos);
    EXPECT_NE(proxied_other_request.find("GET /whatever HTTP/1.1\r\n"), std::string::npos);
}

TEST(LiteNginxRuntimeTest, Returns404WhenNoRouteMatches) {
    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:PORT;

    server {
        server_name localhost;
        location /api/:id {
            proxy_pass http://127.0.0.1:9001;
        }
    }
}
)";
    std::size_t marker = config_text.find("PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, 4, std::to_string(port));

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "runtime_404.conf");
    ASSERT_TRUE(config.has_value());

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);

    const char request[] = "GET /miss HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));

    std::string response = recv_http_response(client);
    ::close(client);

    EXPECT_NE(response.find("HTTP/1.1 404 Not Found\r\n"), std::string::npos);
    EXPECT_NE(response.find("404 Not Found\n"), std::string::npos);
}

TEST(LiteNginxRuntimeTest, WritesStructuredAccessLogForCompletedRequest) {
    TestLoggingScope logging_scope;
    TestLogFile access_file;
    ASSERT_TRUE(access_file.valid());
    const std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
logging {
    appender access_file {
        type file;
        path ACCESS_PATH;
        min_level info;
        max_level info;
    }
    logger lite_nginx.access {
        level info;
        appender access_file;
        additive off;
    }
    root_logger {
        level warn;
        appender access_file;
    }
}
http {
    listen 127.0.0.1:PORT;
    access_log lite_nginx.access "request_id=${$access.request_id} remote_addr=\"${$conn.remote_addr}\" remote_port=${$conn.remote_port} method=\"${$req.method}\" path=\"${$req.path}\" protocol=${$conn.http_version} scheme=${$conn.scheme} tls=${$conn.tls} server=\"${$access.server}\" location=\"${$access.location}\" status=${$access.status} body_bytes_sent=${$access.body_bytes_sent} request_time_us=${$access.request_time_us} outcome=${$access.outcome} gzip_status=${$gzip.status} gzip_used=${$gzip.used} gzip_input_bytes=${$gzip.input_bytes} gzip_output_bytes=${$gzip.output_bytes} gzip_ratio=${$gzip.ratio}";
    server {
        server_name localhost;
        location /api/:id { proxy_pass http://127.0.0.1:9001; }
    }
}
)";
    std::size_t marker = config_text.find("ACCESS_PATH");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("ACCESS_PATH") - 1, access_file.path());
    marker = config_text.find("PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("PORT") - 1, std::to_string(port));

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "access_runtime.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;
    auto log_config = fiber::lite_nginx::logging::LoggingBuilder::build(*config);
    ASSERT_TRUE(log_config.has_value()) << log_config.error().message;
    auto initialized = fiber::log::LoggerManager::global().initialize(std::move(*log_config));
    ASSERT_TRUE(initialized.has_value()) << initialized.error().message;
    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    {
        RuntimeHarness harness(*runtime);
        int client = connect_client(harness.port());
        ASSERT_GE(client, 0);
        const char request[] = "GET /miss?secret=hidden HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));
        std::string response = recv_http_response(client);
        ::close(client);
        EXPECT_NE(response.find("HTTP/1.1 404 Not Found\r\n"), std::string::npos);
    }

    const std::string access = access_file.read();
    EXPECT_NE(access.find("lite_nginx.access"), std::string::npos);
    EXPECT_NE(access.find("remote_addr=\"127.0.0.1\""), std::string::npos);
    EXPECT_NE(access.find("method=\"GET\""), std::string::npos);
    EXPECT_NE(access.find("path=\"/miss\""), std::string::npos);
    EXPECT_EQ(access.find("secret=hidden"), std::string::npos);
    EXPECT_NE(access.find("protocol=HTTP/1.1"), std::string::npos);
    EXPECT_NE(access.find("scheme=http"), std::string::npos);
    EXPECT_NE(access.find("tls=false"), std::string::npos);
    EXPECT_NE(access.find("server=\"localhost\""), std::string::npos);
    EXPECT_NE(access.find("location=\"null\""), std::string::npos);
    EXPECT_NE(access.find("status=404"), std::string::npos);
    EXPECT_NE(access.find("body_bytes_sent=14"), std::string::npos);
    EXPECT_NE(access.find("outcome=ok"), std::string::npos);
    EXPECT_NE(access.find("gzip_status=off"), std::string::npos);
    EXPECT_NE(access.find("gzip_used=false"), std::string::npos);
    EXPECT_NE(access.find("gzip_input_bytes=0"), std::string::npos);
    EXPECT_NE(access.find("gzip_output_bytes=0"), std::string::npos);
    EXPECT_NE(access.find("gzip_ratio=null"), std::string::npos);
}

TEST(LiteNginxRuntimeTest, ReusesNamedUpstreamConnectionsWithKeepalive) {
    std::promise<std::string> first_upstream_request;
    std::promise<std::string> second_upstream_request;
    auto first_future = first_upstream_request.get_future();
    auto second_future = second_upstream_request.get_future();
    KeepAliveUpstream upstream(
            {
                    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain\r\nConnection: "
                    "keep-alive\r\n\r\nfirst",
                    "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nContent-Type: text/plain\r\nConnection: "
                    "keep-alive\r\n\r\nsecond",
            },
            {&first_upstream_request, &second_upstream_request});
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;

    upstream backend {
        server 127.0.0.1:UPSTREAM_PORT;
    }

    connection_pool {
        keepalive_size 2;
        keepalive_timeout 30s;
    }

    server {
        server_name localhost;
        location /* {
            proxy_pass upstream://backend;
        }
    }
}
)";
    std::size_t marker = config_text.find("LISTEN_PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("LISTEN_PORT") - 1, std::to_string(port));

    marker = config_text.find("UPSTREAM_PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("UPSTREAM_PORT") - 1, std::to_string(upstream.port()));

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "runtime_keepalive.conf");
    ASSERT_TRUE(config.has_value());

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char first_request[] = "GET /first HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, first_request, sizeof(first_request) - 1, 0),
              static_cast<ssize_t>(sizeof(first_request) - 1));
    std::string first_response = recv_http_response(client);
    ::close(client);

    client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char second_request[] = "GET /second HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, second_request, sizeof(second_request) - 1, 0),
              static_cast<ssize_t>(sizeof(second_request) - 1));
    std::string second_response = recv_http_response(client);
    ::close(client);

    ASSERT_EQ(first_future.wait_for(3s), std::future_status::ready);
    ASSERT_EQ(second_future.wait_for(3s), std::future_status::ready);
    std::string first_proxied_request = first_future.get();
    std::string second_proxied_request = second_future.get();

    EXPECT_NE(first_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(first_response.find("first"), std::string::npos);
    EXPECT_NE(second_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(second_response.find("second"), std::string::npos);
    EXPECT_NE(first_proxied_request.find("GET /first HTTP/1.1\r\n"), std::string::npos);
    EXPECT_NE(second_proxied_request.find("GET /second HTTP/1.1\r\n"), std::string::npos);
    EXPECT_EQ(upstream.accept_count(), 1);
}

TEST(LiteNginxRuntimeTest, ProxiesToHttpsUpstreamOverTls) {
    TlsSingleRequestUpstream upstream;
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;

    upstream backend {
        server https://127.0.0.1:UPSTREAM_PORT;
    }

    server {
        server_name localhost;
        location /* {
            proxy_pass upstream://backend;
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("UPSTREAM_PORT"), sizeof("UPSTREAM_PORT") - 1,
                        std::to_string(upstream.port()));

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "https_upstream.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;
    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);
    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char request[] = "GET /secure HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));
    std::string response = recv_http_response(client);
    ::close(client);

    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("\r\n\r\nsecure"), std::string::npos) << response;
}

TEST(LiteNginxRuntimeTest, ProxiesDirectHttpsTargetOverTls) {
    TlsSingleRequestUpstream upstream;
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);
    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    server {
        server_name localhost;
        location /* {
            proxy_pass https://127.0.0.1:UPSTREAM_PORT;
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("UPSTREAM_PORT"), sizeof("UPSTREAM_PORT") - 1,
                        std::to_string(upstream.port()));

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "direct_https_upstream.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;
    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);
    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    static constexpr std::string_view kRequest = "GET /secure HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_TRUE(send_all(client, kRequest));
    const std::string response = recv_http_response(client);
    ::close(client);

    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("\r\n\r\nsecure"), std::string::npos) << response;
}

// steal off must still pool per-loop: with worker_processes 1 there is one loop, so two sequential
// requests reuse one upstream connection (accept_count == 1). This exercises the LocalHttp1ConnectionPoolSet
// wiring through the unified acquire_and_connect path.
TEST(LiteNginxRuntimeTest, ReusesConnectionsWithStealOff) {
    std::promise<std::string> first_upstream_request;
    std::promise<std::string> second_upstream_request;
    auto first_future = first_upstream_request.get_future();
    auto second_future = second_upstream_request.get_future();
    KeepAliveUpstream upstream(
            {
                    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain\r\nConnection: "
                    "keep-alive\r\n\r\nfirst",
                    "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nContent-Type: text/plain\r\nConnection: "
                    "keep-alive\r\n\r\nsecond",
            },
            {&first_upstream_request, &second_upstream_request});
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;

    upstream backend {
        server 127.0.0.1:UPSTREAM_PORT;
    }

    connection_pool {
        keepalive_size 2;
        keepalive_timeout 30s;
        steal off;
    }

    server {
        server_name localhost;
        location /* {
            proxy_pass upstream://backend;
        }
    }
}
)";
    std::size_t marker = config_text.find("LISTEN_PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("LISTEN_PORT") - 1, std::to_string(port));

    marker = config_text.find("UPSTREAM_PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("UPSTREAM_PORT") - 1, std::to_string(upstream.port()));

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "runtime_steal_off.conf");
    ASSERT_TRUE(config.has_value());

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;
    EXPECT_FALSE(runtime->connection_pool.steal);

    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char first_request[] = "GET /first HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, first_request, sizeof(first_request) - 1, 0),
              static_cast<ssize_t>(sizeof(first_request) - 1));
    std::string first_response = recv_http_response(client);
    ::close(client);

    client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char second_request[] = "GET /second HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, second_request, sizeof(second_request) - 1, 0),
              static_cast<ssize_t>(sizeof(second_request) - 1));
    std::string second_response = recv_http_response(client);
    ::close(client);

    ASSERT_EQ(first_future.wait_for(3s), std::future_status::ready);
    ASSERT_EQ(second_future.wait_for(3s), std::future_status::ready);
    first_future.get();
    second_future.get();

    EXPECT_NE(first_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(second_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_EQ(upstream.accept_count(), 1);
}

TEST(LiteNginxRuntimeTest, PropagatesConnectionPoolSizingToRuntime) {
    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:8080;

    connection_pool {
        keepalive_size 4;
        keepalive_timeout 30s;
        max_idle_total 128;
        initial_group_capacity 8;
    }

    server {
        server_name localhost;
        location /* {
            proxy_pass http://127.0.0.1:9001;
        }
    }
}
)";
    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "runtime_sizing.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;
    EXPECT_EQ(runtime->connection_pool.keepalive_size, 4u);
    EXPECT_EQ(runtime->connection_pool.max_idle_total, 128u);
    EXPECT_EQ(runtime->connection_pool.initial_group_capacity, 8u);
}

TEST(LiteNginxRuntimeTest, PoolsDirectProxyPassTargetsByDefault) {
    std::promise<std::string> first_upstream_request;
    std::promise<std::string> second_upstream_request;
    auto first_future = first_upstream_request.get_future();
    auto second_future = second_upstream_request.get_future();
    KeepAliveUpstream upstream(
            {
                    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain\r\nConnection: "
                    "keep-alive\r\n\r\nfirst",
                    "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nContent-Type: text/plain\r\nConnection: "
                    "keep-alive\r\n\r\nsecond",
            },
            {&first_upstream_request, &second_upstream_request});
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;

    server {
        server_name localhost;
        location /* {
            proxy_pass http://127.0.0.1:UPSTREAM_PORT;
        }
    }
}
)";
    std::size_t marker = config_text.find("LISTEN_PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("LISTEN_PORT") - 1, std::to_string(port));

    marker = config_text.find("UPSTREAM_PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("UPSTREAM_PORT") - 1, std::to_string(upstream.port()));

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "runtime_direct_short.conf");
    ASSERT_TRUE(config.has_value());

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char first_request[] = "GET /first HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, first_request, sizeof(first_request) - 1, 0),
              static_cast<ssize_t>(sizeof(first_request) - 1));
    std::string first_response = recv_http_response(client);
    ::close(client);

    client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char second_request[] = "GET /second HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, second_request, sizeof(second_request) - 1, 0),
              static_cast<ssize_t>(sizeof(second_request) - 1));
    std::string second_response = recv_http_response(client);
    ::close(client);

    ASSERT_EQ(first_future.wait_for(3s), std::future_status::ready);
    ASSERT_EQ(second_future.wait_for(3s), std::future_status::ready);

    EXPECT_NE(first_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(second_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_EQ(upstream.accept_count(), 1);
}

TEST(LiteNginxRuntimeTest, ReuseConnectionOffBypassesPoolForDirectTargets) {
    std::promise<std::string> first_upstream_request;
    std::promise<std::string> second_upstream_request;
    auto first_future = first_upstream_request.get_future();
    auto second_future = second_upstream_request.get_future();
    KeepAliveUpstream upstream(
            {
                    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain\r\nConnection: "
                    "keep-alive\r\n\r\nfirst",
                    "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nContent-Type: text/plain\r\nConnection: "
                    "keep-alive\r\n\r\nsecond",
            },
            {&first_upstream_request, &second_upstream_request});
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;

    server {
        server_name localhost;
        location /* {
            proxy_pass http://127.0.0.1:UPSTREAM_PORT;
            reuse_connection off;
        }
    }
}
)";
    std::size_t marker = config_text.find("LISTEN_PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("LISTEN_PORT") - 1, std::to_string(port));

    marker = config_text.find("UPSTREAM_PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("UPSTREAM_PORT") - 1, std::to_string(upstream.port()));

    auto config =
            fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "runtime_direct_no_reuse.conf");
    ASSERT_TRUE(config.has_value());

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;
    EXPECT_FALSE(runtime->servers[0].locations[0].reuse_connection);

    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char first_request[] = "GET /first HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, first_request, sizeof(first_request) - 1, 0),
              static_cast<ssize_t>(sizeof(first_request) - 1));
    std::string first_response = recv_http_response(client);
    ::close(client);

    client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char second_request[] = "GET /second HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, second_request, sizeof(second_request) - 1, 0),
              static_cast<ssize_t>(sizeof(second_request) - 1));
    std::string second_response = recv_http_response(client);
    ::close(client);

    ASSERT_EQ(first_future.wait_for(3s), std::future_status::ready);
    ASSERT_EQ(second_future.wait_for(3s), std::future_status::ready);

    const std::string first_proxy_request = first_future.get();
    const std::string second_proxy_request = second_future.get();
    EXPECT_NE(first_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(second_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(first_proxy_request.find("Connection: close\r\n"), std::string::npos);
    EXPECT_NE(second_proxy_request.find("Connection: close\r\n"), std::string::npos);
    EXPECT_EQ(upstream.accept_count(), 2);
}

TEST(LiteNginxRuntimeTest, StealsNamedUpstreamConnectionsAcrossWorkersWhenEnabled) {
    std::promise<std::string> first_upstream_request;
    std::promise<std::string> second_upstream_request;
    auto first_future = first_upstream_request.get_future();
    auto second_future = second_upstream_request.get_future();
    KeepAliveUpstream upstream(
            {
                    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain\r\nConnection: "
                    "keep-alive\r\n\r\nfirst",
                    "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nContent-Type: text/plain\r\nConnection: "
                    "keep-alive\r\n\r\nsecond",
            },
            {&first_upstream_request, &second_upstream_request});
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 2;
http {
    listen 127.0.0.1:LISTEN_PORT;

    upstream backend {
        server 127.0.0.1:UPSTREAM_PORT;
    }

    connection_pool {
        keepalive_size 2;
        keepalive_timeout 30s;
    }

    server {
        server_name localhost;
        location /* {
            proxy_pass upstream://backend;
        }
    }
}
)";
    std::size_t marker = config_text.find("LISTEN_PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("LISTEN_PORT") - 1, std::to_string(port));

    marker = config_text.find("UPSTREAM_PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("UPSTREAM_PORT") - 1, std::to_string(upstream.port()));

    auto config =
            fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "runtime_stealable_keepalive.conf");
    ASSERT_TRUE(config.has_value());

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char first_request[] = "GET /first HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, first_request, sizeof(first_request) - 1, 0),
              static_cast<ssize_t>(sizeof(first_request) - 1));
    std::string first_response = recv_http_response(client);
    ::close(client);

    client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char second_request[] = "GET /second HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, second_request, sizeof(second_request) - 1, 0),
              static_cast<ssize_t>(sizeof(second_request) - 1));
    std::string second_response = recv_http_response(client);
    ::close(client);

    ASSERT_EQ(first_future.wait_for(3s), std::future_status::ready);
    ASSERT_EQ(second_future.wait_for(3s), std::future_status::ready);

    EXPECT_NE(first_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(second_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_EQ(upstream.accept_count(), 1);
}

TEST(LiteNginxRuntimeTest, AppliesInheritedGzipToScriptResponse) {
    TestScriptFile script("gzip_script",
                          "resp.sendJson(200, {message: \"lite-nginx gzip runtime response response response\"});");
    ASSERT_TRUE(script.ok());

    const std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);
    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    gzip on;
    gzip_types application/json;
    gzip_min_length 1;
    gzip_comp_level 2;

    server {
        server_name localhost;
        location /* {
            script_file SCRIPT_PATH;
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("SCRIPT_PATH"), sizeof("SCRIPT_PATH") - 1, script.path());

    auto config = ConfigLoader::load_from_string(config_text, "gzip_runtime.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;
    auto runtime = RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;
    const auto &gzip = runtime->servers[0].locations[0].gzip;
    EXPECT_TRUE(gzip.enabled);
    EXPECT_EQ(gzip.compression_level, 2);
    EXPECT_EQ(gzip.min_length, 1u);

    RuntimeHarness harness(*runtime);
    const int client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    static constexpr std::string_view kRequest = "GET /script HTTP/1.1\r\n"
                                                 "Host: localhost\r\n"
                                                 "Accept-Encoding: gzip\r\n"
                                                 "Connection: close\r\n\r\n";
    ASSERT_TRUE(send_all(client, kRequest));
    const std::string response = recv_until_close(client);
    ::close(client);

    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("Content-Encoding: gzip\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("Transfer-Encoding: chunked\r\n"), std::string::npos) << response;
    EXPECT_EQ(response.find("Content-Length:"), std::string::npos) << response;
    EXPECT_NE(response.find("Vary: Accept-Encoding\r\n"), std::string::npos) << response;
    const std::string compressed = decode_chunked_body(response);
    ASSERT_FALSE(compressed.empty());
    const std::string decoded = gunzip_body(compressed);
    EXPECT_NE(decoded.find("\"message\":\"lite-nginx gzip runtime response response response\""), std::string::npos)
            << decoded;
}

TEST(LiteNginxRuntimeTest, CompressesScriptResponseOverHttp2) {
    TestPemFile cert("gzip-h2-cert", kSelfSignedCertPem);
    TestPemFile key("gzip-h2-key", kSelfSignedKeyPem);
    TestScriptFile script("gzip_h2_script",
                          "resp.sendJson(200, {message: \"lite-nginx HTTP/2 gzip response response\"});");
    ASSERT_TRUE(cert.ok());
    ASSERT_TRUE(key.ok());
    ASSERT_TRUE(script.ok());

    const std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);
    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT ssl;
    gzip on;
    gzip_types application/json;
    gzip_min_length 1;

    server {
        server_name localhost;
        certificate CERT_FILE;
        certificate_key KEY_FILE;
        location /* { script_file SCRIPT_PATH; }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("CERT_FILE"), sizeof("CERT_FILE") - 1, cert.path());
    config_text.replace(config_text.find("KEY_FILE"), sizeof("KEY_FILE") - 1, key.path());
    config_text.replace(config_text.find("SCRIPT_PATH"), sizeof("SCRIPT_PATH") - 1, script.path());

    auto config = ConfigLoader::load_from_string(config_text, "gzip_h2_runtime.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;
    auto runtime = RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;
    RuntimeHarness harness(*runtime);

    fiber::event::EventLoopGroup client_group(1);
    client_group.start();
    std::promise<Http2GzipOutcome> client_promise;
    auto client_future = client_promise.get_future();
    fiber::async::spawn(client_group.at(0),
                        [&]() { return run_http2_gzip_client(&client_group.at(0), harness.port(), &client_promise); });

    const std::future_status client_status = client_future.wait_for(5s);
    Http2GzipOutcome outcome;
    if (client_status == std::future_status::ready) {
        outcome = client_future.get();
    }
    client_group.stop();
    client_group.join();

    ASSERT_EQ(client_status, std::future_status::ready);
    EXPECT_EQ(outcome.error, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.status_code, 200);
    EXPECT_EQ(outcome.content_encoding, "gzip");
    EXPECT_TRUE(outcome.content_length.empty());
    EXPECT_TRUE(outcome.body_complete);
    const std::string decoded = gunzip_body(outcome.body);
    EXPECT_NE(decoded.find("\"message\":\"lite-nginx HTTP/2 gzip response response\""), std::string::npos) << decoded;
}

TEST(LiteNginxRuntimeTest, GzipWriterUsesNativeHttp3StreamCompletion) {
    TestPemFile cert("gzip-h3-cert", kSelfSignedCertPem);
    TestPemFile key("gzip-h3-key", kSelfSignedKeyPem);
    ASSERT_TRUE(cert.ok());
    ASSERT_TRUE(key.ok());

    static constexpr std::string_view kBody = "lite-nginx HTTP/3 gzip response lite-nginx HTTP/3 gzip response";
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::net::TlsOptions tls_material{};
    tls_material.certificate_chain = fiber::net::TlsPemSource::from_file(cert.path());
    tls_material.private_key = fiber::net::TlsPemSource::from_file(key.path());
    auto tls_context = fiber::net::TlsContext::create(tls_material);
    ASSERT_TRUE(tls_context);
    fiber::http::HttpServerOptions server_options;
    server_options.tls.default_context = tls_context->get();
    server_options.http3.enabled = true;
    fiber::http::HttpHandler handler = [](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        static const std::vector<std::string> kTypes{"text/plain"};
        auto base = fiber::http::make_http_response_writer(exchange);
        fiber::lite_nginx::runtime::GzipResponseWriter gzip(
                exchange, base, {.enabled = true, .types = kTypes, .min_length = 1, .compression_level = 1});
        auto response = gzip.writer();
        fiber::http::HttpHeaders headers(exchange.pool());
        headers.set("Content-Type", "text/plain");
        headers.set("Content-Length", std::to_string(kBody.size()));
        auto header_result = co_await response.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 200,
                .headers = &headers,
                .body = fiber::http::HttpBodySpec::ContentLength(kBody.size()),
                .end_stream = false,
        });
        if (header_result) {
            (void) co_await response.write_all(reinterpret_cast<const std::uint8_t *>(kBody.data()), kBody.size(), true,
                                               2s);
        }
    };
    fiber::http::Http3Server server(group.at(0), std::move(handler), std::move(server_options));
    ASSERT_TRUE(server.bind({fiber::net::IpAddress::loopback_v4(), 0}));
    server.serve();

    fiber::quic::QuicUdpEndpoint client_endpoint;
    fiber::quic::QuicUdpEndpoint::EndpointOptions endpoint_options;
    endpoint_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    ASSERT_TRUE(client_endpoint.init(group.at(0), endpoint_options));

    std::promise<Http3GzipOutcome> client_promise;
    auto client_future = client_promise.get_future();
    const fiber::net::SocketAddress server_addr = server.local_addr();
    const std::string cert_path = cert.path();
    fiber::async::spawn(group.at(0), [&]() {
        return run_http3_gzip_client(&client_endpoint, &server_addr, &cert_path, &client_promise);
    });

    const std::future_status client_status = client_future.wait_for(5s);
    Http3GzipOutcome outcome;
    if (client_status == std::future_status::ready) {
        outcome = client_future.get();
    }

    std::promise<void> close_promise;
    auto close_future = close_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        co_await server.shutdown_and_wait();
        close_promise.set_value();
        co_return;
    });
    close_future.wait();
    group.stop();
    group.join();

    ASSERT_EQ(client_status, std::future_status::ready);
    EXPECT_EQ(outcome.error, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.status_code, 200);
    EXPECT_EQ(outcome.content_encoding, "gzip");
    EXPECT_TRUE(outcome.content_length.empty());
    EXPECT_TRUE(outcome.body_complete);
    EXPECT_EQ(outcome.request_outcome, fiber::http::Http3RequestOutcome::Complete);
    EXPECT_EQ(gunzip_body(outcome.body), kBody);
}

TEST(LiteNginxRuntimeTest, CompressesUnknownLengthProxyResponse) {
    const std::string body = "unknown-length proxy body unknown-length proxy body unknown-length proxy body";
    SingleRequestUpstream upstream("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n" + body,
                                   nullptr);
    ASSERT_NE(upstream.port(), 0);

    const std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);
    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    gzip on;
    gzip_types text/plain;
    gzip_min_length 1;

    server {
        server_name localhost;
        location /* {
            proxy_pass http://127.0.0.1:UPSTREAM_PORT;
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("UPSTREAM_PORT"), sizeof("UPSTREAM_PORT") - 1,
                        std::to_string(upstream.port()));

    auto config = ConfigLoader::load_from_string(config_text, "gzip_unknown_length_proxy.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;
    auto runtime = RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;
    RuntimeHarness harness(*runtime);

    const int client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    static constexpr std::string_view kRequest = "GET /proxy HTTP/1.1\r\n"
                                                 "Host: localhost\r\n"
                                                 "Accept-Encoding: gzip\r\n"
                                                 "Connection: close\r\n\r\n";
    ASSERT_TRUE(send_all(client, kRequest));
    const std::string response = recv_until_close(client);
    ::close(client);

    EXPECT_NE(response.find("Content-Encoding: gzip\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("Transfer-Encoding: chunked\r\n"), std::string::npos) << response;
    EXPECT_EQ(response.find("Content-Length:"), std::string::npos) << response;
    const std::string compressed = decode_chunked_body(response);
    ASSERT_FALSE(compressed.empty());
    EXPECT_EQ(gunzip_body(compressed), body);
}

TEST(LiteNginxRuntimeTest, ScriptFileLocationServesScriptResponse) {
    const std::string script_path = "/tmp/lite_nginx_script_location_test.js";
    {
        std::ofstream file(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.good());
        file << "resp.sendJson(200, {msg: \"hello-script\", path: req.getPath()});";
    }

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;

    server {
        server_name localhost;
        location /* {
            script_file SCRIPT_PATH;
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("SCRIPT_PATH"), sizeof("SCRIPT_PATH") - 1, script_path);

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "script_location.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;
    ASSERT_TRUE(runtime->script_library != nullptr);
    ASSERT_EQ(runtime->servers[0].locations.size(), 1u);
    ASSERT_TRUE(runtime->servers[0].locations[0].script != nullptr);

    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);

    const char request[] = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));

    std::string response = recv_http_response(client);
    ::close(client);

    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("application/json"), std::string::npos) << response;
    EXPECT_NE(response.find("\"msg\":\"hello-script\""), std::string::npos) << response;
    EXPECT_NE(response.find("\"path\":\"/hello\""), std::string::npos) << response;

    ::unlink(script_path.c_str());
}

TEST(LiteNginxRuntimeTest, ScriptBodyConsumersMapIncrementalLimitFailureTo413) {
    TestScriptFile script("body_limit", "req.discardBody(); return req.readBinary();");
    ASSERT_TRUE(script.ok());

    const std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);
    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    server {
        server_name localhost;
        location /* {
            client_max_body_size 5;
            script_file SCRIPT_PATH;
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("SCRIPT_PATH"), sizeof("SCRIPT_PATH") - 1, script.path());

    auto config = ConfigLoader::load_from_string(config_text, "script_body_limit.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;
    auto runtime = RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);
    const int client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char request[] = "POST /upload HTTP/1.1\r\n"
                           "Host: localhost\r\n"
                           "Transfer-Encoding: chunked\r\n"
                           "Connection: close\r\n"
                           "\r\n"
                           "6\r\nabcdef\r\n"
                           "0\r\n\r\n";
    ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));

    const std::string response = recv_http_response(client);
    ::close(client);

    EXPECT_NE(response.find("HTTP/1.1 413 Payload Too Large\r\n"), std::string::npos) << response;
    EXPECT_EQ(response.find("HTTP/1.1 500"), std::string::npos) << response;
}

// Drives a single GET /x through a script_file location whose script is `script_body`, and
// returns the raw HTTP/1.1 response. Used to check how run_script synthesizes a response from
// the script outcome (Value -> 200+json, Void -> 204, Exception -> 500+json, Abort -> 500+json)
// when the script never called resp.* / http.proxyPass itself.
std::string run_script_result_response(std::string_view script_body) {
    const std::string script_path = "/tmp/lite_nginx_script_result_test.js";
    {
        std::ofstream file(script_path, std::ios::binary | std::ios::trunc);
        if (!file.good()) {
            return {};
        }
        file << script_body;
    }

    std::uint16_t port = reserve_loopback_port();
    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    server {
        server_name localhost;
        location /* {
            script_file SCRIPT_PATH;
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("SCRIPT_PATH"), sizeof("SCRIPT_PATH") - 1, script_path);

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "script_result.conf");
    if (!config.has_value()) {
        ::unlink(script_path.c_str());
        return {};
    }
    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    if (!runtime.has_value()) {
        ::unlink(script_path.c_str());
        return {};
    }
    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    const char request[] = "GET /x HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    (void) ::send(client, request, sizeof(request) - 1, 0);
    std::string response = recv_http_response(client);
    ::close(client);
    ::unlink(script_path.c_str());
    return response;
}

// A script that returns a Value is served as 200 + a JSON body encoding that value.
TEST(LiteNginxRuntimeTest, ScriptReturnValueServes200Json) {
    std::string response = run_script_result_response("return {a: 1, b: [2, 3]};");
    ASSERT_FALSE(response.empty()) << "no response";
    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("application/json"), std::string::npos) << response;
    EXPECT_NE(response.find("\"a\":1"), std::string::npos) << response;
    EXPECT_NE(response.find("\"b\":[2,3]"), std::string::npos) << response;
}

// A script that ends without producing a value (bare `return;` / fall-through) is Void and
// served as 204 No Content.
TEST(LiteNginxRuntimeTest, ScriptReturnVoidServes204) {
    std::string response = run_script_result_response("var x = 42;");
    ASSERT_FALSE(response.empty()) << "no response";
    EXPECT_NE(response.find("HTTP/1.1 204 No Content\r\n"), std::string::npos) << response;
}

// An uncaught tagged TypeError (no heap payload) is served as 500 + {"error":"TypeError"}.
TEST(LiteNginxRuntimeTest, ScriptTaggedExceptionServes500JsonErrorName) {
    std::string response = run_script_result_response("hash.md5(123);");
    ASSERT_FALSE(response.empty()) << "no response";
    EXPECT_NE(response.find("HTTP/1.1 500"), std::string::npos) << response;
    EXPECT_NE(response.find("application/json"), std::string::npos) << response;
    EXPECT_NE(response.find("\"error\":\"TypeError\""), std::string::npos) << response;
}

// An uncaught heap exception (JSON.parse failure -> SyntaxError) is served as 500 + the
// exception's serialized form, which carries its name.
TEST(LiteNginxRuntimeTest, ScriptHeapExceptionServes500JsonWithName) {
    std::string response = run_script_result_response("JSON.parse(\"{bad\");");
    ASSERT_FALSE(response.empty()) << "no response";
    EXPECT_NE(response.find("HTTP/1.1 500"), std::string::npos) << response;
    EXPECT_NE(response.find("application/json"), std::string::npos) << response;
    EXPECT_NE(response.find("\"name\":\"SyntaxError\""), std::string::npos) << response;
}

TEST(LiteNginxRuntimeTest, ScriptFileLocationRejectsMissingFile) {
    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(R"(
worker_processes 1;
http {
    listen 127.0.0.1:8080;
    server {
        server_name localhost;
        location /* { script_file /tmp/lite_nginx_does_not_exist_999999.js; }
    }
}
)",
                                                                            "missing_script.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_FALSE(runtime.has_value());
    EXPECT_NE(runtime.error().message.find("script_file not found"), std::string::npos);
}

// A relative script_file (resolved against the config file's directory at parse time) is
// opened and compiled by RuntimeBuilder regardless of the process pwd. This exercises the
// full path-resolution + open + compile pipeline end to end via load_from_file.
TEST(LiteNginxRuntimeTest, RelativeScriptFileCompilesAfterResolution) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "lite_nginx_rel_script_e2e";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "scripts", ec);
    {
        std::ofstream f(dir / "scripts" / "x.js", std::ios::binary | std::ios::trunc);
        f << "resp.sendJson(200, {msg: \"hello-relative\", path: req.getPath()});";
    }
    {
        std::ofstream f(dir / "main.conf", std::ios::binary | std::ios::trunc);
        f << R"(
worker_processes 1;
http {
    listen 127.0.0.1:8080;
    server { server_name localhost; location /* { script_file scripts/x.js; } }
}
)";
    }

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_file((dir / "main.conf").string());
    ASSERT_TRUE(config.has_value()) << config.error().message;
    // Resolved to an absolute path under the config directory (not the bare "scripts/x.js").
    const auto &script_file = config->http.servers[0].locations[0].script_file;
    EXPECT_EQ(script_file.front(), '/') << script_file;
    EXPECT_NE(script_file.find("scripts/x.js"), std::string::npos) << script_file;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;
    ASSERT_EQ(runtime->servers[0].locations.size(), 1u);
    EXPECT_TRUE(runtime->servers[0].locations[0].script != nullptr);

    fs::remove_all(dir, ec);
}

TEST(LiteNginxRuntimeTest, ScriptFileAndProxyPassAreMutuallyExclusive) {
    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(R"(
worker_processes 1;
http {
    listen 127.0.0.1:8080;
    server {
        server_name localhost;
        location /* {
            proxy_pass http://127.0.0.1:9001;
            script_file /tmp/x.js;
        }
    }
}
)",
                                                                            "mutual_exclusive.conf");
    ASSERT_FALSE(config.has_value());
    EXPECT_NE(config.error().message.find("only one of proxy_pass or script_file"), std::string::npos);
}

TEST(LiteNginxRuntimeTest, ScriptFileLocationResolvesPathVar) {
    const std::string script_path = "/tmp/lite_nginx_path_var_resolve_test.js";
    {
        std::ofstream file(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.good());
        // $path.id is validated at compile time against the route pattern /users/:id, and
        // resolved at request time from the matched path capture.
        file << "resp.sendJson(200, {id: $path.id, uri: $req.uri, method: $req.method});";
    }

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    server {
        server_name localhost;
        location /users/:id {
            script_file SCRIPT_PATH;
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("SCRIPT_PATH"), sizeof("SCRIPT_PATH") - 1, script_path);

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "path_var.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;
    ASSERT_TRUE(runtime->servers[0].locations[0].script != nullptr);
    ASSERT_TRUE(runtime->route_script_extension != nullptr);

    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);

    const char request[] = "GET /users/42 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));

    std::string response = recv_http_response(client);
    ::close(client);

    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("\"id\":\"42\""), std::string::npos) << response;
    EXPECT_NE(response.find("\"uri\":\"/users/42\""), std::string::npos) << response;
    EXPECT_NE(response.find("\"method\":\"GET\""), std::string::npos) << response;

    ::unlink(script_path.c_str());
}

TEST(LiteNginxRuntimeTest, ScriptFileLocationRejectsUnknownPathVar) {
    // $path.missing is not a capture of /users/:id, so the script must fail to compile at
    // runtime-build time with "constant not found".
    const std::string script_path = "/tmp/lite_nginx_path_var_unknown_test.js";
    {
        std::ofstream file(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.good());
        file << "resp.sendJson(200, $path.missing);";
    }

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    server {
        server_name localhost;
        location /users/:id {
            script_file SCRIPT_PATH;
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("SCRIPT_PATH"), sizeof("SCRIPT_PATH") - 1, script_path);

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "path_var_unknown.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_FALSE(runtime.has_value());
    EXPECT_NE(runtime.error().message.find("constant not found"), std::string::npos) << runtime.error().message;

    ::unlink(script_path.c_str());
}

TEST(LiteNginxRuntimeTest, AccessLogConstantsDoNotLeakIntoRouteScripts) {
    const std::string script_path = "/tmp/lite_nginx_access_scope_test.js";
    {
        std::ofstream file(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.good());
        file << "resp.sendJson(200, $access.status);";
    }

    std::string config_text = R"(
http {
    listen 127.0.0.1:8080;
    access_log test.access "status=${$access.status}";
    server {
        server_name localhost;
        location /* { script_file SCRIPT_PATH; }
    }
}
)";
    config_text.replace(config_text.find("SCRIPT_PATH"), sizeof("SCRIPT_PATH") - 1, script_path);

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "access_scope.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;
    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_FALSE(runtime.has_value());
    EXPECT_NE(runtime.error().message.find("constant not found"), std::string::npos);

    ::unlink(script_path.c_str());
}

// A bare `location /foo` (no `:param`/`*`) matches exactly that path -- it is NOT a prefix.
// `/foo` hits the script; `/foo/bar` matches no location and returns 404.
TEST(LiteNginxRuntimeTest, ScriptFileLocationBarePatternMatchesExactly) {
    const std::string script_path = "/tmp/lite_nginx_bare_pattern_exact_test.js";
    {
        std::ofstream file(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.good());
        file << "resp.sendJson(200, {hit: \"foo\"});";
    }

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    server {
        server_name localhost;
        location /foo {
            script_file SCRIPT_PATH;
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("SCRIPT_PATH"), sizeof("SCRIPT_PATH") - 1, script_path);

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "bare_pattern.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);

    // /foo matches the bare static location exactly.
    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char foo_request[] = "GET /foo HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, foo_request, sizeof(foo_request) - 1, 0), static_cast<ssize_t>(sizeof(foo_request) - 1));
    std::string foo_response = recv_http_response(client);
    ::close(client);
    EXPECT_NE(foo_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << foo_response;
    EXPECT_NE(foo_response.find("\"hit\":\"foo\""), std::string::npos) << foo_response;

    // /foo/bar does not match /foo (exact, not prefix) and there is no catch-all -> 404.
    client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char sub_request[] = "GET /foo/bar HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, sub_request, sizeof(sub_request) - 1, 0), static_cast<ssize_t>(sizeof(sub_request) - 1));
    std::string sub_response = recv_http_response(client);
    ::close(client);
    EXPECT_NE(sub_response.find("HTTP/1.1 404"), std::string::npos) << sub_response;

    ::unlink(script_path.c_str());
}

// `location /` matches only the root path `/` -- it is NOT a catch-all (use `/*` for
// that). Locks the absence of the old `/` -> `/*` rewrite.
TEST(LiteNginxRuntimeTest, LocationRootPatternMatchesOnlyRoot) {
    const std::string script_path = "/tmp/lite_nginx_root_pattern_exact_test.js";
    {
        std::ofstream file(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.good());
        file << "resp.sendJson(200, {hit: \"root\"});";
    }

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    server {
        server_name localhost;
        location / {
            script_file SCRIPT_PATH;
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("SCRIPT_PATH"), sizeof("SCRIPT_PATH") - 1, script_path);

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "root_pattern.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);

    // / matches the root location exactly.
    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char root_request[] = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, root_request, sizeof(root_request) - 1, 0),
              static_cast<ssize_t>(sizeof(root_request) - 1));
    std::string root_response = recv_http_response(client);
    ::close(client);
    EXPECT_NE(root_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << root_response;
    EXPECT_NE(root_response.find("\"hit\":\"root\""), std::string::npos) << root_response;

    // /anything does not match / (exact, not catch-all) -> 404.
    client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char sub_request[] = "GET /anything HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, sub_request, sizeof(sub_request) - 1, 0), static_cast<ssize_t>(sizeof(sub_request) - 1));
    std::string sub_response = recv_http_response(client);
    ::close(client);
    EXPECT_NE(sub_response.find("HTTP/1.1 404"), std::string::npos) << sub_response;

    ::unlink(script_path.c_str());
}

// vars.js (shipped under conf/scripts/) demonstrates all five route-variable constants
// ($path/$query/$header/$cookie/$req) on a /api/:id route. This loads the actual shipped
// file -- not a /tmp copy -- so it cannot bit-rot, and asserts every namespace end to end
// including the absent -> null contract.
TEST(LiteNginxRuntimeTest, ScriptFileVarsJsRouteVariables) {
    const std::string vars_js = std::string(FIBER_LITE_NGINX_SOURCE_DIR) + "/conf/scripts/vars.js";

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    server {
        server_name localhost;
        location /api/:id {
            script_file VARS_JS;
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("VARS_JS"), sizeof("VARS_JS") - 1, vars_js);

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "vars_js.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);

    // Present values across all five namespaces.
    {
        int client = connect_client(harness.port());
        ASSERT_GE(client, 0);
        const char request[] = "GET /api/42?src=web HTTP/1.1\r\n"
                               "Host: localhost\r\n"
                               "X-Forwarded-For: 1.2.3.4\r\n"
                               "Cookie: session=abc\r\n"
                               "Connection: close\r\n"
                               "\r\n";
        ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));
        std::string response = recv_http_response(client);
        ::close(client);

        EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << response;
        EXPECT_NE(response.find("\"id\":\"42\""), std::string::npos) << response; // $path
        EXPECT_NE(response.find("\"src\":\"web\""), std::string::npos) << response; // $query
        EXPECT_NE(response.find("\"clientIp\":\"1.2.3.4\""), std::string::npos) << response; // $header (-/_ fold)
        EXPECT_NE(response.find("\"session\":\"abc\""), std::string::npos) << response; // $cookie
        EXPECT_NE(response.find("\"uri\":\"/api/42?src=web\""), std::string::npos) << response; // $req.uri
        EXPECT_NE(response.find("\"method\":\"GET\""), std::string::npos) << response; // $req.method
        EXPECT_NE(response.find("\"path\":\"/api/42\""), std::string::npos) << response; // $req.path
        EXPECT_NE(response.find("\"queryStr\":\"src=web\""), std::string::npos) << response; // $req.query
    }

    // Absent -> null (not error, not undefined): $query/$header/$cookie missing; $req.query
    // is the empty raw query string.
    {
        int client = connect_client(harness.port());
        ASSERT_GE(client, 0);
        const char request[] = "GET /api/7 HTTP/1.1\r\n"
                               "Host: localhost\r\n"
                               "Connection: close\r\n"
                               "\r\n";
        ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));
        std::string response = recv_http_response(client);
        ::close(client);

        EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << response;
        EXPECT_NE(response.find("\"id\":\"7\""), std::string::npos) << response;
        EXPECT_NE(response.find("\"src\":null"), std::string::npos) << response;
        EXPECT_NE(response.find("\"clientIp\":null"), std::string::npos) << response;
        EXPECT_NE(response.find("\"session\":null"), std::string::npos) << response;
        EXPECT_NE(response.find("\"uri\":\"/api/7\""), std::string::npos) << response;
        EXPECT_NE(response.find("\"method\":\"GET\""), std::string::npos) << response;
        EXPECT_NE(response.find("\"path\":\"/api/7\""), std::string::npos) << response;
        EXPECT_NE(response.find("\"queryStr\":\"\""), std::string::npos) << response;
    }
}

// `directive svc = http "@backend"; svc.request({...})` issues an upstream request and returns
// {status, headers?, body}.
TEST(LiteNginxRuntimeTest, HttpRequestFetchesUpstreamResponse) {
    const std::string script_path = "/tmp/lite_nginx_http_request_test.js";
    {
        std::ofstream file(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.good());
        file << "directive svc = http \"@backend\";\n"
                "let r = svc.request({path: \"/x\", includeHeaders: true});\n"
                "resp.sendJson(200, {status: r.status, headers: r.headers});";
    }

    std::promise<std::string> upstream_request;
    auto upstream_future = upstream_request.get_future();
    SingleRequestUpstream upstream("HTTP/1.1 200 OK\r\nContent-Length: 7\r\nContent-Type: text/plain\r\n\r\nproxied",
                                   &upstream_request);
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    connection_pool { keepalive_size 8; keepalive_timeout 30s; }
    upstream backend { server 127.0.0.1:UPSTREAM_PORT; }
    server {
        server_name localhost;
        location /* { script_file SCRIPT_PATH; }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("UPSTREAM_PORT"), sizeof("UPSTREAM_PORT") - 1,
                        std::to_string(upstream.port()));
    config_text.replace(config_text.find("SCRIPT_PATH"), sizeof("SCRIPT_PATH") - 1, script_path);

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "http_request.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);

    const char request[] = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));

    std::string response = recv_http_response(client);
    ::close(client);

    ASSERT_EQ(upstream_future.wait_for(3s), std::future_status::ready);
    std::string proxied_request = upstream_future.get();

    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("\"status\":200"), std::string::npos) << response;
    EXPECT_NE(response.find("\"Content-Type\":\"text/plain\""), std::string::npos) << response;
    EXPECT_NE(proxied_request.find("GET /x HTTP/1.1"), std::string::npos) << proxied_request;

    ::unlink(script_path.c_str());
}

// `directive svc = http "@backend"; svc.proxyPass({})` forwards the inbound request to the
// upstream and copies the upstream response back to the client.
TEST(LiteNginxRuntimeTest, HttpProxyPassForwardsRequestAndResponse) {
    const std::string script_path = "/tmp/lite_nginx_http_proxy_pass_test.js";
    {
        std::ofstream file(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.good());
        file << "directive svc = http \"@backend\";\n"
                "svc.proxyPass({});";
    }

    std::promise<std::string> upstream_request;
    auto upstream_future = upstream_request.get_future();
    SingleRequestUpstream upstream("HTTP/1.1 200 OK\r\nContent-Length: 7\r\nContent-Type: text/plain\r\n\r\nproxied",
                                   &upstream_request);
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    connection_pool { keepalive_size 8; keepalive_timeout 30s; }
    upstream backend { server 127.0.0.1:UPSTREAM_PORT; }
    server {
        server_name localhost;
        location /* {
            client_max_body_size 5;
            gzip on;
            gzip_types text/plain;
            gzip_min_length 1;
            script_file SCRIPT_PATH;
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("UPSTREAM_PORT"), sizeof("UPSTREAM_PORT") - 1,
                        std::to_string(upstream.port()));
    config_text.replace(config_text.find("SCRIPT_PATH"), sizeof("SCRIPT_PATH") - 1, script_path);

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "http_proxy_pass.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);

    const char headers[] = "POST /api/42 HTTP/1.1\r\n"
                           "Host: localhost\r\n"
                           "Accept-Encoding: gzip\r\n"
                           "Expect: 100-continue\r\n"
                           "Content-Length: 5\r\n"
                           "Connection: close\r\n"
                           "\r\n";
    ASSERT_EQ(::send(client, headers, sizeof(headers) - 1, 0), static_cast<ssize_t>(sizeof(headers) - 1));
    const std::string informational = recv_until_contains(client, "\r\n\r\n");
    ASSERT_EQ(informational, "HTTP/1.1 100 Continue\r\n\r\n");
    ASSERT_TRUE(send_all(client, "hello"));

    std::string response = recv_until_close(client);
    ::close(client);

    ASSERT_EQ(upstream_future.wait_for(3s), std::future_status::ready);
    std::string proxied_request = upstream_future.get();

    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("Content-Encoding: gzip\r\n"), std::string::npos) << response;
    const std::string compressed = decode_chunked_body(response);
    ASSERT_FALSE(compressed.empty());
    EXPECT_EQ(gunzip_body(compressed), "proxied");
    EXPECT_NE(proxied_request.find("POST /api/42 HTTP/1.1"), std::string::npos) << proxied_request;
    EXPECT_NE(proxied_request.find("\r\n\r\nhello"), std::string::npos) << proxied_request;
    EXPECT_EQ(proxied_request.find("Expect:"), std::string::npos) << proxied_request;

    ::unlink(script_path.c_str());
}

TEST(LiteNginxRuntimeTest, HttpProxyPassFlushControlsResponseBodyBuffering) {
    struct TestCase {
        std::string_view options;
        bool expect_first_body_before_tail;
    };
    constexpr std::array<TestCase, 3> cases{{
            {.options = "{}", .expect_first_body_before_tail = false},
            {.options = "{flush: false}", .expect_first_body_before_tail = false},
            {.options = "{flush: true}", .expect_first_body_before_tail = true},
    }};

    for (const TestCase &test: cases) {
        SCOPED_TRACE(test.options);
        StagedProxyPassOutcome outcome = run_staged_proxy_pass(test.options);

        ASSERT_TRUE(outcome.error.empty()) << outcome.error;
        EXPECT_EQ(outcome.first_body_arrived_before_tail, test.expect_first_body_before_tail);
        EXPECT_NE(outcome.response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << outcome.response;
        EXPECT_NE(outcome.response.find("first"), std::string::npos) << outcome.response;
        EXPECT_NE(outcome.response.find("second"), std::string::npos) << outcome.response;
    }
}

TEST(LiteNginxRuntimeTest, HttpProxyPassFlushControlsGzipSyncFlush) {
    struct TestCase {
        std::string_view options;
        bool expect_first_body_before_tail;
    };
    constexpr std::array<TestCase, 2> cases{{
            {.options = "{flush: false}", .expect_first_body_before_tail = false},
            {.options = "{flush: true}", .expect_first_body_before_tail = true},
    }};

    for (const TestCase &test: cases) {
        SCOPED_TRACE(test.options);
        StagedProxyPassOutcome outcome = run_staged_proxy_pass(test.options, true);

        ASSERT_TRUE(outcome.error.empty()) << outcome.error;
        EXPECT_EQ(outcome.first_body_arrived_before_tail, test.expect_first_body_before_tail);
        EXPECT_NE(outcome.response.find("Content-Encoding: gzip\r\n"), std::string::npos) << outcome.response;
        EXPECT_NE(outcome.response.find("Transfer-Encoding: chunked\r\n"), std::string::npos) << outcome.response;
        const std::string compressed = decode_chunked_body(outcome.response);
        ASSERT_FALSE(compressed.empty());
        EXPECT_EQ(gunzip_body(compressed), "firstsecond");
    }
}

TEST(LiteNginxRuntimeTest, ProxyBufferingOffSyncFlushesChunkedUpstreamGzipResponse) {
    for (const bool buffering: {true, false}) {
        SCOPED_TRACE(buffering ? "proxy_buffering on" : "proxy_buffering off");
        StagedProxyPassOutcome outcome = run_staged_direct_chunked_proxy(buffering);

        ASSERT_TRUE(outcome.error.empty()) << outcome.error;
        EXPECT_EQ(outcome.first_body_arrived_before_tail, !buffering);
        EXPECT_NE(outcome.response.find("Content-Encoding: gzip\r\n"), std::string::npos) << outcome.response;
        EXPECT_NE(outcome.response.find("Transfer-Encoding: chunked\r\n"), std::string::npos) << outcome.response;
        EXPECT_EQ(outcome.response.find("Content-Length:"), std::string::npos) << outcome.response;
        const std::string compressed = decode_chunked_body(outcome.response);
        ASSERT_FALSE(compressed.empty());
        EXPECT_EQ(gunzip_body(compressed), "firstsecond");
    }
}

TEST(LiteNginxRuntimeTest, HttpProxyPassCompletesZeroLengthResponseWithoutBodyWrite) {
    TestScriptFile script("http_proxy_pass_empty", "directive svc = http \"@backend\";\nsvc.proxyPass({});");
    ASSERT_TRUE(script.ok());

    std::promise<std::string> upstream_request;
    auto upstream_future = upstream_request.get_future();
    SingleRequestUpstream upstream("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", &upstream_request);
    ASSERT_NE(upstream.port(), 0);

    const std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);
    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    connection_pool { keepalive_size 8; keepalive_timeout 30s; }
    upstream backend { server 127.0.0.1:UPSTREAM_PORT; }
    server {
        server_name localhost;
        location /* { script_file SCRIPT_PATH; }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("UPSTREAM_PORT"), sizeof("UPSTREAM_PORT") - 1,
                        std::to_string(upstream.port()));
    config_text.replace(config_text.find("SCRIPT_PATH"), sizeof("SCRIPT_PATH") - 1, script.path());

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "empty_proxy_pass.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;
    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);
    const int client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const std::string_view request = "GET /empty HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_TRUE(send_all(client, request));

    const std::string response = recv_http_response(client);
    ::close(client);

    ASSERT_EQ(upstream_future.wait_for(3s), std::future_status::ready);
    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("Content-Length: 0\r\n"), std::string::npos) << response;
    EXPECT_EQ(response.find("500"), std::string::npos) << response;
}

// `directive svc = http "http://127.0.0.1:PORT";` binds a script handle to an ad-hoc IP-literal URL
// target; svc.request then resolves to the bound target.
TEST(LiteNginxRuntimeTest, HttpDirectiveBindsUrlTarget) {
    const std::string script_path = "/tmp/lite_nginx_http_directive_test.js";
    {
        std::ofstream file(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.good());
        file << "directive svc = http \"http://127.0.0.1:UPSTREAM_PORT\";\n"
                "let r = svc.request({path: \"/x\"});\n"
                "resp.sendJson(200, {status: r.status});";
    }

    std::promise<std::string> upstream_request;
    auto upstream_future = upstream_request.get_future();
    SingleRequestUpstream upstream("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Type: text/plain\r\n\r\nok",
                                   &upstream_request);
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    connection_pool { keepalive_size 8; keepalive_timeout 30s; }
    server {
        server_name localhost;
        location /* { script_file SCRIPT_PATH; }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("SCRIPT_PATH"), sizeof("SCRIPT_PATH") - 1, script_path);

    // Substitute the upstream port into the script (the directive binds to the URL literal).
    {
        std::ofstream file(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.good());
        file << "directive svc = http \"http://127.0.0.1:" << upstream.port()
             << "\";\n"
                "let r = svc.request({path: \"/x\"});\n"
                "resp.sendJson(200, {status: r.status});";
    }

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "http_directive.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);

    const char request[] = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));

    std::string response = recv_http_response(client);
    ::close(client);

    ASSERT_EQ(upstream_future.wait_for(3s), std::future_status::ready);
    std::string proxied_request = upstream_future.get();

    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("\"status\":200"), std::string::npos) << response;
    EXPECT_NE(proxied_request.find("GET /x HTTP/1.1"), std::string::npos) << proxied_request;

    ::unlink(script_path.c_str());
}

// ${...} proxy_set_header values compile with the runtime's route extension.
TEST(LiteNginxRuntimeTest, ProxySetHeaderTemplateCompiles) {
    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    server {
        server_name localhost;
        location / {
            proxy_pass http://127.0.0.1:9001;
            proxy_set_header X-Original-Host "${$header.host}";
            proxy_set_header X-Static "literal";
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "tmpl_compile.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    const auto &loc = runtime->servers[0].locations[0];
    ASSERT_EQ(loc.set_headers.size(), 2u);
    EXPECT_EQ(loc.set_headers[0].name, "X-Original-Host");
    EXPECT_EQ(loc.set_headers[1].name, "X-Static");
    // Template header has a compiled script; static header does not.
    EXPECT_TRUE(loc.set_headers[0].template_script != nullptr);
    EXPECT_TRUE(loc.set_headers[1].template_script == nullptr);
    EXPECT_FALSE(loc.set_headers[0].template_script->contains_async());
}

// $path.<unknown> in a template is a compile-time error (the name is not a route capture).
TEST(LiteNginxRuntimeTest, ProxySetHeaderTemplateRejectsUnknownPathVar) {
    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    server {
        server_name localhost;
        location /users/:id {
            proxy_pass http://127.0.0.1:9001;
            proxy_set_header X "${$path.missing}";
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "tmpl_bad_path.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_FALSE(runtime.has_value());
    EXPECT_NE(runtime.error().message.find("constant not found"), std::string::npos) << runtime.error().message;
}

// End-to-end: ${$header.host} evaluates per request from the inbound Host header and is sent
// to the upstream as the templated header value.
TEST(LiteNginxRuntimeTest, ProxySetHeaderTemplateEvaluatesPerRequest) {
    std::promise<std::string> upstream_request;
    auto upstream_future = upstream_request.get_future();
    SingleRequestUpstream upstream("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", &upstream_request);
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    server {
        server_name localhost;
        location / {
            proxy_pass http://127.0.0.1:UPSTREAM_PORT;
            proxy_set_header X-Original-Host "${$header.host}";
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("UPSTREAM_PORT"), sizeof("UPSTREAM_PORT") - 1,
                        std::to_string(upstream.port()));

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "tmpl_eval.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);

    const char request[] = "GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));

    std::string response = recv_http_response(client);
    ::close(client);

    ASSERT_EQ(upstream_future.wait_for(3s), std::future_status::ready);
    std::string proxied_request = upstream_future.get();

    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << response;
    EXPECT_NE(proxied_request.find("X-Original-Host: example.com\r\n"), std::string::npos) << proxied_request;
}

} // namespace
