#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <utility>
#include <vector>

#include "../../../tests/HttpTransportStub.h"
#include "async/Sleep.h"
#include "async/Spawn.h"
#include "event/EventLoopGroup.h"
#include "execution/AccessRequestHandler.h"
#include "execution/ProxyExecutor.h"
#include "execution/ProxyRequestSender.h"
#include "fiber/cat/CatClient.h"
#include "fiber/cat/CatClientConfig.h"
#include "http/Http1Connection.h"
#include "http/HttpServer.h"
#include "net/SocketAddress.h"
#include "net/TcpListener.h"
#include "net/TcpStream.h"
#include "observability/AccessRequestTelemetry.h"

namespace {

using namespace std::chrono_literals;

struct ObservedUpstreamRequest {
    fiber::http::HttpMethod method = fiber::http::HttpMethod::Unknown;
    std::string target;
    std::string host;
    std::string content_length;
    std::string transfer_encoding;
    std::string source;
    std::string client_header;
    std::string connection;
    std::string upgrade;
    std::string websocket_key;
    std::string trace_id;
    std::string parent_span_id;
    std::string span_id;
    std::string body;
    std::uint16_t remote_port = 0;
};

struct UpstreamState {
    struct Response {
        int status = 201;
        std::optional<int> informational_status;
        std::vector<std::pair<std::string, std::string>> headers;
        std::optional<std::string> body;
        std::optional<std::size_t> advertised_content_length;
        std::chrono::milliseconds delay{};
        bool chunked = false;
        bool websocket = false;
        std::promise<void> *completion = nullptr;
    };

    std::vector<ObservedUpstreamRequest> requests;
    std::string websocket_client_data;
    Response response;
};

struct ProxyAdapterState {
    fiber::access_server::ProxyRequestSender *sender = nullptr;
    std::vector<int> statuses;
    std::vector<std::string> bodies;
    std::optional<fiber::access_server::ProxyRequestError> error;
};

struct ServiceSelectorState {
    std::uint16_t port = 0;
    std::string good_host_header;
    std::string bad_host_header;
    std::size_t select_count = 0;
    std::vector<std::pair<std::uint64_t, bool>> reports;
};

struct CatFrameCapture {
    mutable std::mutex mutex;
    std::vector<std::vector<std::uint8_t>> frames;

    [[nodiscard]] bool contains(std::string_view first, std::string_view second = {}) const {
        std::lock_guard lock(mutex);
        for (const auto &frame: frames) {
            const bool contains_first =
                    std::search(frame.begin(), frame.end(), first.begin(), first.end()) != frame.end();
            const bool contains_second = second.empty() || std::search(frame.begin(), frame.end(), second.begin(),
                                                                       second.end()) != frame.end();
            if (contains_first && contains_second) {
                return true;
            }
        }
        return false;
    }
};

class RecordingTransport final : public fiber::test::HttpTransportStub {
public:
    RecordingTransport(fiber::event::EventLoop &loop, std::string input, std::string &output,
                       bool hold_open_after_input = false) :
        loop_(loop), input_(std::move(input)), output_(output), hold_open_after_input_(hold_open_after_input) {}

    fiber::async::Task<fiber::common::IoResult<void>> handshake(std::chrono::milliseconds) override {
        co_return fiber::common::IoResult<void>{};
    }

    fiber::async::Task<fiber::common::IoResult<void>> shutdown(std::chrono::milliseconds) override {
        co_return fiber::common::IoResult<void>{};
    }

    fiber::async::Task<fiber::common::IoResult<void>> wait_readable(std::chrono::milliseconds) override {
        co_return fiber::common::IoResult<void>{};
    }

    fiber::async::Task<fiber::common::IoResult<std::size_t>> read(void *, std::size_t,
                                                                  std::chrono::milliseconds) override {
        if (hold_open_after_input_) {
            co_await fiber::async::sleep(10s);
        }
        co_return static_cast<std::size_t>(0);
    }

    fiber::async::Task<fiber::common::IoResult<std::size_t>> read_into(fiber::mem::IoBuf &buffer,
                                                                       std::chrono::milliseconds) override {
        if (input_consumed_) {
            if (hold_open_after_input_) {
                co_await fiber::async::sleep(10s);
            }
            co_return static_cast<std::size_t>(0);
        }
        if (buffer.writable() < input_.size()) {
            co_return std::unexpected(fiber::common::IoErr::MessageTooLarge);
        }
        std::memcpy(buffer.writable_data(), input_.data(), input_.size());
        buffer.commit(input_.size());
        input_consumed_ = true;
        co_return input_.size();
    }

    fiber::async::Task<fiber::common::IoResult<std::size_t>> readv_into(fiber::mem::IoBufChain &,
                                                                        std::chrono::milliseconds) override {
        co_return std::unexpected(fiber::common::IoErr::NotSupported);
    }

    fiber::async::Task<fiber::common::IoResult<std::size_t>> write(const void *buffer, std::size_t size,
                                                                   std::chrono::milliseconds) override {
        output_.append(static_cast<const char *>(buffer), size);
        co_return size;
    }

    fiber::async::Task<fiber::common::IoResult<std::size_t>> write(fiber::mem::IoBuf &buffer,
                                                                   std::chrono::milliseconds) override {
        const std::size_t size = buffer.readable();
        output_.append(reinterpret_cast<const char *>(buffer.readable_data()), size);
        buffer.consume(size);
        co_return size;
    }

    fiber::async::Task<fiber::common::IoResult<std::size_t>> writev(fiber::mem::IoBufChain &buffers,
                                                                    std::chrono::milliseconds) override {
        std::array<iovec, 16> iov{};
        const int count = buffers.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
        std::size_t size = 0;
        for (int i = 0; i < count; ++i) {
            const iovec &entry = iov[static_cast<std::size_t>(i)];
            output_.append(static_cast<const char *>(entry.iov_base), entry.iov_len);
            size += entry.iov_len;
        }
        buffers.consume_and_compact(size);
        co_return size;
    }

    void disconnect() noexcept {
        closed_ = true;
        notify_terminal(fiber::common::IoErr::ConnReset);
    }

    void close() override { closed_ = true; }
    [[nodiscard]] bool valid() const noexcept override { return !closed_; }
    [[nodiscard]] int fd() const noexcept override { return -1; }
    [[nodiscard]] std::string_view negotiated_alpn() const noexcept override { return {}; }
    [[nodiscard]] const fiber::net::SocketAddress &remote_addr() const noexcept override { return remote_addr_; }
    [[nodiscard]] fiber::event::EventLoop &loop() const noexcept override { return loop_; }

private:
    fiber::event::EventLoop &loop_;
    std::string input_;
    std::string &output_;
    fiber::net::SocketAddress remote_addr_{};
    bool input_consumed_ = false;
    bool closed_ = false;
    bool hold_open_after_input_ = false;
};

fiber::common::IoResult<std::uint16_t> bound_port(int fd) {
    sockaddr_storage bound{};
    socklen_t length = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &length) != 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    fiber::net::SocketAddress address;
    if (!fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<sockaddr *>(&bound), length, address)) {
        return std::unexpected(fiber::common::IoErr::NotSupported);
    }
    return address.port();
}

fiber::async::Task<fiber::common::IoResult<void>> read_exact(fiber::net::TcpStream &stream, std::uint8_t *data,
                                                             std::size_t size) noexcept {
    std::size_t offset = 0;
    while (offset < size) {
        auto read = co_await stream.read(data + offset, size - offset, 5s);
        if (!read) {
            co_return std::unexpected(read.error());
        }
        if (*read == 0) {
            co_return std::unexpected(fiber::common::IoErr::NotConnected);
        }
        offset += *read;
    }
    co_return fiber::common::IoResult<void>{};
}

fiber::async::DetachedTask collect_cat_frames(fiber::net::TcpListener *listener, CatFrameCapture *capture,
                                              std::promise<void> *done) noexcept {
    auto accepted = co_await listener->accept();
    if (!accepted) {
        done->set_value();
        co_return;
    }
    listener->close();
    fiber::net::TcpStream stream(fiber::event::EventLoop::current(), accepted->release_fd(), accepted->take_peer());
    for (;;) {
        std::array<std::uint8_t, 4> prefix{};
        auto prefix_result = co_await read_exact(stream, prefix.data(), prefix.size());
        if (!prefix_result) {
            break;
        }
        const std::size_t payload_size = static_cast<std::size_t>(prefix[0]) << 24U |
                                         static_cast<std::size_t>(prefix[1]) << 16U |
                                         static_cast<std::size_t>(prefix[2]) << 8U | prefix[3];
        if (payload_size == 0 || payload_size > 2 * 1024 * 1024) {
            break;
        }
        std::vector<std::uint8_t> frame(prefix.begin(), prefix.end());
        frame.resize(prefix.size() + payload_size);
        auto payload_result = co_await read_exact(stream, frame.data() + prefix.size(), payload_size);
        if (!payload_result) {
            break;
        }
        std::lock_guard lock(capture->mutex);
        capture->frames.push_back(std::move(frame));
    }
    stream.close();
    done->set_value();
}

bool wait_for_cat_frame(const CatFrameCapture &capture, std::string_view first, std::string_view second = {}) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (capture.contains(first, second)) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return capture.contains(first, second);
}

std::string consume_chain(fiber::mem::IoBufChain chain) {
    std::string result;
    while (fiber::mem::IoBuf *part = chain.first_readable()) {
        result.append(reinterpret_cast<const char *>(part->readable_data()), part->readable());
        chain.consume_and_compact(part->readable());
    }
    return result;
}

template<typename Exchange>
fiber::async::Task<fiber::common::IoResult<std::string>> read_body(Exchange &exchange) {
    std::string result;
    for (;;) {
        auto body = co_await exchange.read_body(64 * 1024);
        if (!body) {
            co_return std::unexpected(body.error());
        }
        const bool complete = body->complete();
        result.append(consume_chain(std::move(*body)));
        if (complete) {
            co_return result;
        }
    }
}

fiber::async::Task<void> serve_upstream(fiber::http::HttpExchange &exchange, UpstreamState *state) {
    ObservedUpstreamRequest observed;
    observed.method = exchange.method();
    observed.target.assign(exchange.uri().unparsed_uri);
    observed.host.assign(exchange.header("Host"));
    observed.content_length.assign(exchange.header("Content-Length"));
    observed.transfer_encoding.assign(exchange.header("Transfer-Encoding"));
    observed.source.assign(exchange.header("X-Ploto-Source-App"));
    observed.client_header.assign(exchange.header("X-Client"));
    observed.connection.assign(exchange.header("Connection"));
    observed.upgrade.assign(exchange.header("Upgrade"));
    observed.websocket_key.assign(exchange.header("Sec-WebSocket-Key"));
    observed.trace_id.assign(exchange.header("HI-TRACE-ID"));
    observed.parent_span_id.assign(exchange.header("HI-SPAN-ID-PARENT"));
    observed.span_id.assign(exchange.header("HI-SPAN-ID"));
    observed.remote_port = exchange.remote_addr().port();

    auto body = co_await read_body(exchange);
    if (!body) {
        co_return;
    }
    observed.body = std::move(*body);
    state->requests.push_back(std::move(observed));

    if (state->response.delay > 0ms) {
        co_await fiber::async::sleep(state->response.delay);
    }

    if (state->response.websocket) {
        fiber::http::HttpHeaders headers(exchange.pool());
        headers.set("Connection", "Upgrade");
        headers.set("Upgrade", "websocket");
        headers.set("Sec-WebSocket-Accept", "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
        auto sent = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 101,
                .headers = &headers,
                .body = fiber::http::HttpBodySpec::Stream(),
                .end_stream = false,
        });
        if (sent) {
            constexpr std::string_view server_frame = "server-frame";
            (void) co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(server_frame.data()),
                                               server_frame.size(), false);
            auto client_frame = co_await exchange.read_body(64 * 1024);
            if (client_frame) {
                state->websocket_client_data = consume_chain(std::move(*client_frame));
            }
            (void) co_await exchange.write_all(nullptr, 0, true);
        }
        if (state->response.completion) {
            state->response.completion->set_value();
        }
        co_return;
    }

    const std::string response_body =
            state->response.body.value_or("upstream-" + std::to_string(state->requests.size()));
    fiber::http::HttpHeaders headers(exchange.pool());
    headers.set("Content-Type", "text/plain");
    for (const auto &[name, value]: state->response.headers) {
        if (!headers.add(name, value)) {
            co_return;
        }
    }
    if (state->response.informational_status) {
        auto informational = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Informational,
                .status_code = *state->response.informational_status,
                .headers = &headers,
                .end_stream = false,
        });
        if (!informational) {
            co_return;
        }
    }
    const fiber::http::HttpBodySpec body_spec =
            state->response.chunked ? fiber::http::HttpBodySpec::Chunked()
                                    : fiber::http::HttpBodySpec::ContentLength(
                                              state->response.advertised_content_length.value_or(response_body.size()));
    auto sent = co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Final,
            .status_code = state->response.status,
            .headers = &headers,
            .body = body_spec,
            .end_stream = false,
    });
    if (sent) {
        const bool complete = !state->response.advertised_content_length ||
                              *state->response.advertised_content_length == response_body.size();
        (void) co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(response_body.data()),
                                           response_body.size(), complete);
        if (!complete) {
            (void) exchange.abort(fiber::common::IoErr::ConnReset);
        }
    }
    if (state->response.completion) {
        state->response.completion->set_value();
    }
}

fiber::async::DetachedTask start_server(fiber::event::EventLoop *loop, UpstreamState *state,
                                        std::promise<std::uint16_t> *port_promise,
                                        std::promise<fiber::http::HttpServer *> *server_promise) {
    fiber::http::HttpHandler handler = [state](fiber::http::HttpExchange &exchange) {
        return serve_upstream(exchange, state);
    };
    auto *server = new (std::nothrow) fiber::http::HttpServer(*loop, std::move(handler));
    if (!server) {
        port_promise->set_value(0);
        server_promise->set_value(nullptr);
        co_return;
    }
    auto bound = server->bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), {});
    if (!bound) {
        delete server;
        port_promise->set_value(0);
        server_promise->set_value(nullptr);
        co_return;
    }
    const auto port = bound_port(server->fd());
    port_promise->set_value(port ? *port : 0);
    server_promise->set_value(server);
    fiber::async::spawn(*loop, [server]() { return server->serve(); });
}

fiber::async::Task<fiber::common::IoResult<void>>
execute_proxy(void *context, fiber::http::HttpExchange &exchange,
              const fiber::access_server::PreparedProxyRequest &request,
              std::span<const fiber::access_server::EvaluatedHeader> base_headers,
              fiber::access_server::AccessRequestTelemetry *telemetry) noexcept {
    auto &state = *static_cast<ProxyAdapterState *>(context);
    auto started = co_await state.sender->start(exchange, request, telemetry);
    if (!started) {
        state.error = started.error();
        co_return std::unexpected(started.error().io_error == fiber::common::IoErr::None ? fiber::common::IoErr::Invalid
                                                                                         : started.error().io_error);
    }

    state.statuses.push_back(started->status_code());
    auto body = co_await read_body(*started);
    if (!body) {
        (void) started->abort(body.error());
        co_return std::unexpected(body.error());
    }
    state.bodies.push_back(std::move(*body));

    fiber::http::HttpHeaders headers(exchange.pool());
    for (const auto &header: base_headers) {
        if (!headers.set(header.name, header.value)) {
            co_return std::unexpected(fiber::common::IoErr::NoMem);
        }
    }
    co_return co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Final,
            .status_code = 204,
            .headers = &headers,
            .body = fiber::http::HttpBodySpec::None(),
            .end_stream = true,
    });
}

fiber::async::DetachedTask
run_downstream(fiber::event::EventLoop *loop, const fiber::access_server::RouteConfigStore *store,
               fiber::access_server::AccessProxyAdapter proxy_adapter, std::string request, std::string *output,
               std::promise<void> *done, std::promise<RecordingTransport *> *transport_ready = nullptr,
               bool hold_open_after_input = false, fiber::cat::CatClient *cat_client = nullptr) {
    auto transport = std::make_unique<RecordingTransport>(*loop, std::move(request), *output, hold_open_after_input);
    if (transport_ready) {
        transport_ready->set_value(transport.get());
    }
    fiber::access_server::AccessRequestHandler handler(*store, {}, {}, proxy_adapter);
    fiber::http::HttpHandler http_handler =
            [&handler, cat_client](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        if (!cat_client) {
            co_await handler.handle(exchange);
            co_return;
        }
        fiber::access_server::AccessRequestTelemetry telemetry(exchange, nullptr, cat_client);
        co_await handler.handle(exchange, &telemetry);
    };
    fiber::http::Http1Connection connection(nullptr, std::move(transport), std::move(http_handler), {});
    co_await connection.run();
    done->set_value();
}

fiber::async::DetachedTask disconnect_after(std::chrono::milliseconds delay, RecordingTransport *transport) {
    co_await fiber::async::sleep(delay);
    transport->disconnect();
}

fiber::async::DetachedTask shutdown(fiber::http::LocalHttp1ConnectionPoolSet *pool, fiber::http::HttpServer *server,
                                    std::promise<void> *done) {
    server->close();
    co_await pool->shutdown_async();
    done->set_value();
}

fiber::access_server::ProjectConfig project_config(std::uint16_t port) {
    fiber::access_server::RouteConfig route;
    route.path = "/proxy";
    route.addresses = {
            std::optional<std::string>("127.0.0.1:" + std::to_string(port)),
    };
    route.max_client_body_size = 64;
    route.timeout_millis = 2000;

    fiber::access_server::ProjectConfig config;
    config.version = 1;
    config.hosts = std::vector<fiber::access_server::HostConfigEntry>{
            fiber::access_server::HostConfigEntry{
                    .pattern = "api.example.com",
                    .strategy = fiber::access_server::HostStrategyConfig{},
            },
    };
    config.routes = std::vector<std::optional<fiber::access_server::RouteConfig>>{
            std::move(route),
    };
    return config;
}

fiber::access_server::ProjectConfig
proxy_response_project_config(std::uint16_t port, std::optional<std::int64_t> max_body = 64, bool flush = true) {
    auto config = project_config(port);
    auto &route = **config.routes->begin();
    route.max_proxy_body_size = max_body;
    route.flush = flush;
    route.response_headers = {
            {.name = "X-Override", .value = "configured"},
            {.name = "X-Empty", .value = ""},
            {.name = "X-Custom", .value = "response-template"},
    };
    return config;
}

fiber::access_server::ProjectConfig service_project_config() {
    fiber::access_server::RouteConfig route;
    route.path = "/proxy";
    route.service = "inventory/gray";
    route.cluster = "stable";
    route.timeout_millis = 2000;

    fiber::access_server::ProjectConfig config;
    config.version = 1;
    config.hosts = std::vector<fiber::access_server::HostConfigEntry>{
            fiber::access_server::HostConfigEntry{
                    .pattern = "api.example.com",
                    .strategy = fiber::access_server::HostStrategyConfig{},
            },
    };
    config.routes = std::vector<std::optional<fiber::access_server::RouteConfig>>{
            std::move(route),
    };
    return config;
}

std::expected<fiber::access_server::ProxyUpstreamEndpoint, fiber::access_server::ProxyRequestError>
select_service(void *context, fiber::http::HttpExchange &, std::string_view service,
               std::optional<std::string_view> cluster,
               std::span<const std::uint64_t> excluded_selection_tokens) noexcept {
    auto &state = *static_cast<ServiceSelectorState *>(context);
    if (service != "inventory" || cluster != std::optional<std::string_view>("stable")) {
        return std::unexpected(fiber::access_server::ProxyRequestError{
                .code = fiber::access_server::ProxyRequestErrorCode::SelectUpstream,
                .io_error = fiber::common::IoErr::Invalid,
                .message = "unexpected service selector input",
        });
    }
    const bool first = state.select_count++ == 0;
    if (first) {
        if (!excluded_selection_tokens.empty()) {
            return std::unexpected(fiber::access_server::ProxyRequestError{
                    .code = fiber::access_server::ProxyRequestErrorCode::SelectUpstream,
                    .io_error = fiber::common::IoErr::Invalid,
                    .message = "first selection unexpectedly excluded an instance",
            });
        }
    } else if (excluded_selection_tokens.size() != 1 || excluded_selection_tokens.front() != 1U) {
        return std::unexpected(fiber::access_server::ProxyRequestError{
                .code = fiber::access_server::ProxyRequestErrorCode::SelectUpstream,
                .io_error = fiber::common::IoErr::Invalid,
                .message = "retry did not exclude the failed instance",
        });
    }
    return fiber::access_server::ProxyUpstreamEndpoint{
            .host = first ? std::string_view("127.0.0.2") : std::string_view("127.0.0.1"),
            .port = state.port,
            .host_header = first ? std::string_view(state.bad_host_header) : std::string_view(state.good_host_header),
            .ip_address = first ? fiber::net::IpAddress::v4({127, 0, 0, 2}) : fiber::net::IpAddress::loopback_v4(),
            .selection_token = first ? 1U : 2U,
    };
}

void report_service(void *context, fiber::access_server::ProxyUpstreamEndpoint &endpoint, bool success) noexcept {
    auto &state = *static_cast<ServiceSelectorState *>(context);
    state.reports.emplace_back(endpoint.selection_token, success);
}

std::size_t count_status(std::string_view response, std::string_view status) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = response.find(status, offset)) != std::string_view::npos) {
        ++count;
        offset += status.size();
    }
    return count;
}

std::size_t count_header(std::string_view response, std::string_view header) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = response.find(header, offset)) != std::string_view::npos) {
        ++count;
        offset += header.size();
    }
    return count;
}

TEST(ProxyRequestSenderTest, StreamsJavaCompatibleRequestsAndReusesTheUpstreamConnection) {
    fiber::event::EventLoopGroup group(1);
    fiber::http::LocalHttp1ConnectionPoolSet pool(group);
    ASSERT_TRUE(pool.init());

    fiber::net::TcpListener cat_collector(group.at(0));
    ASSERT_TRUE(cat_collector.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), {}));
    auto cat_port = bound_port(cat_collector.fd());
    ASSERT_TRUE(cat_port);
    fiber::cat::CatClientConfigParams cat_params{
            .app_key = "access-server-test",
            .hostname = "test-host",
            .ip = "127.0.0.1",
            .thread_group_name = "test",
            .thread_id = "0",
            .thread_name = "worker",
    };
    cat_params.bootstrap_collectors.emplace_back(fiber::net::IpAddress::loopback_v4(), *cat_port);
    auto cat_config = fiber::cat::CatClientConfig::create(std::move(cat_params));
    ASSERT_TRUE(cat_config);
    fiber::cat::CatClientOptions cat_options;
    cat_options.enable_heartbeat = false;
    cat_options.enable_system_stats = false;
    cat_options.collector_connect_timeout = 10ms;
    cat_options.collector_write_timeout = 10ms;
    cat_options.reconnect_initial_delay = 10ms;
    cat_options.reconnect_max_delay = 10ms;
    cat_options.shutdown_drain_timeout = 20ms;
    cat_options.aggregation_flush_interval = 10ms;
    auto created_cat_client =
            fiber::cat::CatClient::create(group.at(0), std::move(*cat_config), std::move(cat_options));
    ASSERT_TRUE(created_cat_client);
    std::unique_ptr<fiber::cat::CatClient> cat_client = std::move(*created_cat_client);
    CatFrameCapture cat_capture;
    std::promise<void> cat_capture_done_promise;
    auto cat_capture_done = cat_capture_done_promise.get_future();
    std::promise<bool> cat_started_promise;
    auto cat_started = cat_started_promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0),
                        [&]() { return collect_cat_frames(&cat_collector, &cat_capture, &cat_capture_done_promise); });
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        cat_started_promise.set_value(cat_client->start().has_value());
        co_return;
    });
    ASSERT_EQ(cat_started.wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(cat_started.get());

    UpstreamState upstream_state;
    std::promise<std::uint16_t> port_promise;
    std::promise<fiber::http::HttpServer *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return start_server(&group.at(0), &upstream_state, &port_promise, &server_promise); });

    fiber::http::HttpServer *server = server_future.get();
    ASSERT_NE(server, nullptr);
    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    fiber::access_server::RouteConfigStore store;
    auto published = store.apply("orders", project_config(port));
    ASSERT_TRUE(published) << published.error().message;

    fiber::access_server::ProxyRequestSender sender(pool);
    ProxyAdapterState adapter_state{
            .sender = &sender,
    };
    std::string output;
    std::promise<void> request_promise;
    auto request_future = request_promise.get_future();
    std::string requests = "POST /proxy?item=1 HTTP/1.1\r\n"
                           "Host: api.example.com\r\n"
                           "X-Client: first\r\n"
                           "HI-TRACE-ID: access-root-1\r\n"
                           "HI-SPAN-ID-PARENT: access-parent-1\r\n"
                           "HI-SPAN-ID: access-span-1\r\n"
                           "Transfer-Encoding: chunked\r\n\r\n"
                           "3\r\nhel\r\n2\r\nlo\r\n0\r\n\r\n"
                           "POST /proxy?item=2 HTTP/1.1\r\n"
                           "Host: api.example.com\r\n"
                           "X-Client: second\r\n"
                           "HI-TRACE-ID: access-root-2\r\n"
                           "HI-SPAN-ID-PARENT: access-parent-2\r\n"
                           "HI-SPAN-ID: access-span-2\r\n"
                           "Content-Length: 5\r\n"
                           "Connection: close\r\n\r\n"
                           "world";
    fiber::async::spawn(group.at(0), [&]() {
        return run_downstream(&group.at(0), &store,
                              {
                                      .context = &adapter_state,
                                      .execute = execute_proxy,
                              },
                              std::move(requests), &output, &request_promise, nullptr, false, cat_client.get());
    });

    ASSERT_EQ(request_future.wait_for(5s), std::future_status::ready);
    ASSERT_FALSE(adapter_state.error);
    ASSERT_EQ(adapter_state.statuses, (std::vector<int>{201, 201}));
    ASSERT_EQ(adapter_state.bodies, (std::vector<std::string>{"upstream-1", "upstream-2"}));
    ASSERT_EQ(upstream_state.requests.size(), 2U);

    const ObservedUpstreamRequest &first = upstream_state.requests[0];
    EXPECT_EQ(first.method, fiber::http::HttpMethod::Post);
    EXPECT_EQ(first.target, "/proxy?item=1");
    EXPECT_EQ(first.host, "127.0.0.1:" + std::to_string(port));
    EXPECT_TRUE(first.content_length.empty());
    EXPECT_EQ(first.transfer_encoding, "chunked");
    EXPECT_EQ(first.source, "orders.unifiedAccess");
    EXPECT_EQ(first.client_header, "first");
    EXPECT_EQ(first.trace_id, "access-root-1");
    EXPECT_EQ(first.parent_span_id, "access-span-1");
    EXPECT_FALSE(first.span_id.empty());
    EXPECT_NE(first.span_id, first.parent_span_id);
    EXPECT_EQ(first.body, "hello");

    const ObservedUpstreamRequest &second = upstream_state.requests[1];
    EXPECT_EQ(second.target, "/proxy?item=2");
    EXPECT_EQ(second.host, "127.0.0.1:" + std::to_string(port));
    EXPECT_EQ(second.content_length, "5");
    EXPECT_TRUE(second.transfer_encoding.empty());
    EXPECT_EQ(second.source, "orders.unifiedAccess");
    EXPECT_EQ(second.client_header, "second");
    EXPECT_EQ(second.trace_id, "access-root-2");
    EXPECT_EQ(second.parent_span_id, "access-span-2");
    EXPECT_FALSE(second.span_id.empty());
    EXPECT_NE(second.span_id, second.parent_span_id);
    EXPECT_NE(second.span_id, first.span_id);
    EXPECT_EQ(second.body, "world");
    EXPECT_NE(first.remote_port, 0);
    EXPECT_EQ(second.remote_port, first.remote_port);
    EXPECT_EQ(count_status(output, "HTTP/1.1 204 No Content\r\n"), 2U);
    EXPECT_TRUE(wait_for_cat_frame(cat_capture, "Access.Provider", "&reuse_count=0"));
    EXPECT_TRUE(wait_for_cat_frame(cat_capture, "Access.Provider", "&reuse_count=1"));
    EXPECT_TRUE(wait_for_cat_frame(cat_capture, "Access.Provider", "RemoteCall"));
    EXPECT_FALSE(cat_capture.contains("connection_request_count="));
    EXPECT_FALSE(cat_capture.contains("connection_reuse_count="));

    std::promise<void> shutdown_promise;
    auto shutdown_future = shutdown_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return shutdown(&pool, server, &shutdown_promise); });
    ASSERT_EQ(shutdown_future.wait_for(2s), std::future_status::ready);
    std::promise<void> cat_shutdown_promise;
    auto cat_shutdown = cat_shutdown_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        co_await cat_client->shutdown();
        cat_shutdown_promise.set_value();
    });
    ASSERT_EQ(cat_shutdown.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(cat_capture_done.wait_for(2s), std::future_status::ready);
    group.stop();
    group.join();
    delete server;
}

TEST(ProxyRequestSenderTest, RetriesAServiceSelectionBeforeSendingRequestHeaders) {
    fiber::event::EventLoopGroup group(1);
    fiber::http::LocalHttp1ConnectionPoolSet pool(group);
    ASSERT_TRUE(pool.init());
    group.start();

    UpstreamState upstream_state;
    std::promise<std::uint16_t> port_promise;
    std::promise<fiber::http::HttpServer *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return start_server(&group.at(0), &upstream_state, &port_promise, &server_promise); });

    fiber::http::HttpServer *server = server_future.get();
    ASSERT_NE(server, nullptr);
    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    fiber::access_server::RouteConfigStore store;
    auto published = store.apply("orders", service_project_config());
    ASSERT_TRUE(published) << published.error().message;

    ServiceSelectorState selector_state{
            .port = port,
            .good_host_header = "127.0.0.1:" + std::to_string(port),
            .bad_host_header = "127.0.0.2:" + std::to_string(port),
    };
    fiber::access_server::ProxyRequestSender sender(pool, {
                                                                  .context = &selector_state,
                                                                  .select = select_service,
                                                                  .report = report_service,
                                                          });
    ProxyAdapterState adapter_state{
            .sender = &sender,
    };
    std::string output;
    std::promise<void> request_promise;
    auto request_future = request_promise.get_future();
    std::string request = "GET /proxy HTTP/1.1\r\n"
                          "Host: api.example.com\r\n"
                          "Connection: close\r\n\r\n";
    fiber::async::spawn(group.at(0), [&]() {
        return run_downstream(&group.at(0), &store,
                              {
                                      .context = &adapter_state,
                                      .execute = execute_proxy,
                              },
                              std::move(request), &output, &request_promise);
    });

    ASSERT_EQ(request_future.wait_for(5s), std::future_status::ready);
    ASSERT_FALSE(adapter_state.error);
    EXPECT_EQ(selector_state.select_count, 2U);
    EXPECT_EQ(selector_state.reports, (std::vector<std::pair<std::uint64_t, bool>>{
                                              {1, false},
                                              {2, true},
                                      }));
    ASSERT_EQ(upstream_state.requests.size(), 1U);
    EXPECT_EQ(upstream_state.requests[0].host, selector_state.good_host_header);
    EXPECT_EQ(upstream_state.requests[0].transfer_encoding, "chunked");
    EXPECT_TRUE(upstream_state.requests[0].body.empty());
    EXPECT_EQ(count_status(output, "HTTP/1.1 204 No Content\r\n"), 1U);

    std::promise<void> shutdown_promise;
    auto shutdown_future = shutdown_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return shutdown(&pool, server, &shutdown_promise); });
    ASSERT_EQ(shutdown_future.wait_for(2s), std::future_status::ready);
    group.stop();
    group.join();
    delete server;
}

TEST(ProxyExecutorTest, BridgesJavaCompatibleResponseHeadersAndBody) {
    fiber::event::EventLoopGroup group(1);
    fiber::http::LocalHttp1ConnectionPoolSet pool(group);
    ASSERT_TRUE(pool.init());
    group.start();

    UpstreamState upstream_state{
            .response =
                    {
                            .status = 202,
                            .headers =
                                    {
                                            {"Location", "http://127.0.0.1/next?q=1"},
                                            {"Refresh", "5;url=http://127.0.0.1/refresh"},
                                            {"Proxy-Authenticate", "upstream-secret"},
                                            {"X-Upstream", "kept"},
                                            {"X-Override", "upstream"},
                                            {"X-Empty", "upstream"},
                                    },
                            .body = "bridge-body",
                    },
    };
    std::promise<std::uint16_t> port_promise;
    std::promise<fiber::http::HttpServer *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return start_server(&group.at(0), &upstream_state, &port_promise, &server_promise); });

    fiber::http::HttpServer *server = server_future.get();
    ASSERT_NE(server, nullptr);
    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    fiber::access_server::RouteConfigStore store;
    auto published = store.apply("orders", proxy_response_project_config(port));
    ASSERT_TRUE(published) << published.error().message;

    fiber::access_server::ProxyRequestSender sender(pool);
    fiber::access_server::ProxyExecutor executor(sender);
    std::string output;
    std::promise<void> request_promise;
    auto request_future = request_promise.get_future();
    std::string request = "GET /proxy HTTP/1.1\r\n"
                          "Host: api.example.com\r\n"
                          "X-Forwarded-Proto: https\r\n"
                          "Connection: close\r\n\r\n";
    fiber::async::spawn(group.at(0), [&]() {
        return run_downstream(&group.at(0), &store, executor.adapter(), std::move(request), &output, &request_promise);
    });

    ASSERT_EQ(request_future.wait_for(5s), std::future_status::ready);
    EXPECT_NE(output.find("HTTP/1.1 202 Accepted\r\n"), std::string::npos);
    EXPECT_NE(output.find("Location: https://api.example.com/next?q=1\r\n"), std::string::npos);
    EXPECT_NE(output.find("Refresh: 5;url=https://api.example.com/refresh\r\n"), std::string::npos);
    EXPECT_NE(output.find("X-Upstream: kept\r\n"), std::string::npos);
    EXPECT_NE(output.find("X-Override: configured\r\n"), std::string::npos);
    EXPECT_NE(output.find("X-Custom: response-template\r\n"), std::string::npos);
    EXPECT_NE(output.find("X-Accel-Buffering: no\r\n"), std::string::npos);
    EXPECT_NE(output.find("Content-Length: 11\r\n"), std::string::npos);
    EXPECT_EQ(output.find("Proxy-Authenticate:"), std::string::npos);
    EXPECT_EQ(output.find("X-Empty:"), std::string::npos);
    EXPECT_EQ(output.find("X-Override: upstream\r\n"), std::string::npos);
    EXPECT_EQ(count_header(output, "X-Override:"), 1U);
    EXPECT_NE(output.find("bridge-body"), std::string::npos) << output;

    std::promise<void> shutdown_promise;
    auto shutdown_future = shutdown_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return shutdown(&pool, server, &shutdown_promise); });
    ASSERT_EQ(shutdown_future.wait_for(2s), std::future_status::ready);
    group.stop();
    group.join();
    delete server;
}

TEST(ProxyExecutorTest, ForwardsFinalStatusAndHonorsNoBodyResponses) {
    fiber::event::EventLoopGroup group(1);
    fiber::http::LocalHttp1ConnectionPoolSet pool(group);
    ASSERT_TRUE(pool.init());
    group.start();

    UpstreamState upstream_state;
    std::promise<std::uint16_t> port_promise;
    std::promise<fiber::http::HttpServer *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return start_server(&group.at(0), &upstream_state, &port_promise, &server_promise); });

    fiber::http::HttpServer *server = server_future.get();
    ASSERT_NE(server, nullptr);
    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    fiber::access_server::RouteConfigStore store;
    auto published = store.apply("orders", proxy_response_project_config(port));
    ASSERT_TRUE(published) << published.error().message;
    fiber::access_server::ProxyRequestSender sender(pool);
    fiber::access_server::ProxyExecutor executor(sender);

    struct StatusCase {
        std::string_view method;
        int status;
        std::optional<int> informational_status;
        std::string_view status_line;
        std::string_view body;
    };
    const std::array cases{
            StatusCase{"GET", 200, 103, "HTTP/1.1 200 OK\r\n", "after-informational"},
            StatusCase{"GET", 302, std::nullopt, "HTTP/1.1 302 Found\r\n", "redirect-body"},
            StatusCase{"GET", 404, std::nullopt, "HTTP/1.1 404 Not Found\r\n", "client-error"},
            StatusCase{"GET", 503, std::nullopt, "HTTP/1.1 503 Service Unavailable\r\n", "server-error"},
            StatusCase{"GET", 204, std::nullopt, "HTTP/1.1 204 No Content\r\n", ""},
            StatusCase{"GET", 304, std::nullopt, "HTTP/1.1 304 Not Modified\r\n", ""},
            StatusCase{"HEAD", 200, std::nullopt, "HTTP/1.1 200 OK\r\n", ""},
    };

    for (const StatusCase &item: cases) {
        upstream_state.response = {
                .status = item.status,
                .informational_status = item.informational_status,
                .body = std::string(item.body),
        };
        std::string output;
        std::promise<void> request_promise;
        auto request_future = request_promise.get_future();
        std::string request(item.method);
        request.append(" /proxy HTTP/1.1\r\n"
                       "Host: api.example.com\r\n"
                       "Connection: close\r\n\r\n");
        fiber::async::spawn(group.at(0), [&]() {
            return run_downstream(&group.at(0), &store, executor.adapter(), std::move(request), &output,
                                  &request_promise);
        });

        ASSERT_EQ(request_future.wait_for(5s), std::future_status::ready);
        EXPECT_NE(output.find(item.status_line), std::string::npos) << output;
        EXPECT_EQ(output.find("HTTP/1.1 103"), std::string::npos) << output;
        EXPECT_EQ(count_status(output, "HTTP/1.1 "), 1U) << output;
        if (item.body.empty()) {
            EXPECT_EQ(output.find("after-informational"), std::string::npos);
            EXPECT_EQ(output.find("redirect-body"), std::string::npos);
            EXPECT_EQ(output.find("client-error"), std::string::npos);
            EXPECT_EQ(output.find("server-error"), std::string::npos);
        } else {
            EXPECT_NE(output.find(item.body), std::string::npos) << output;
        }
    }
    EXPECT_EQ(upstream_state.requests.size(), cases.size());

    std::promise<void> shutdown_promise;
    auto shutdown_future = shutdown_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return shutdown(&pool, server, &shutdown_promise); });
    ASSERT_EQ(shutdown_future.wait_for(2s), std::future_status::ready);
    group.stop();
    group.join();
    delete server;
}

TEST(ProxyExecutorTest, RejectsKnownOversizedResponseBeforeCommittingUpstreamStatus) {
    fiber::event::EventLoopGroup group(1);
    fiber::http::LocalHttp1ConnectionPoolSet pool(group);
    ASSERT_TRUE(pool.init());
    group.start();

    UpstreamState upstream_state{
            .response =
                    {
                            .body = "hello",
                    },
    };
    std::promise<std::uint16_t> port_promise;
    std::promise<fiber::http::HttpServer *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return start_server(&group.at(0), &upstream_state, &port_promise, &server_promise); });

    fiber::http::HttpServer *server = server_future.get();
    ASSERT_NE(server, nullptr);
    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    fiber::access_server::RouteConfigStore store;
    auto published = store.apply("orders", proxy_response_project_config(port, 4, false));
    ASSERT_TRUE(published) << published.error().message;

    fiber::access_server::ProxyRequestSender sender(pool);
    fiber::access_server::ProxyExecutor executor(sender);
    std::string output;
    std::promise<void> request_promise;
    auto request_future = request_promise.get_future();
    std::string request = "GET /proxy HTTP/1.1\r\n"
                          "Host: api.example.com\r\n"
                          "Connection: close\r\n\r\n";
    fiber::async::spawn(group.at(0), [&]() {
        return run_downstream(&group.at(0), &store, executor.adapter(), std::move(request), &output, &request_promise);
    });

    ASSERT_EQ(request_future.wait_for(5s), std::future_status::ready);
    EXPECT_NE(output.find("HTTP/1.1 500 Internal Server Error\r\n"), std::string::npos);
    EXPECT_NE(output.find(R"("name":"READ_RESP_BODY")"), std::string::npos);
    EXPECT_NE(output.find("body size is too big：5"), std::string::npos);
    EXPECT_EQ(output.find("HTTP/1.1 201 Created\r\n"), std::string::npos);
    EXPECT_EQ(count_status(output, "HTTP/1.1 "), 1U);

    std::promise<void> shutdown_promise;
    auto shutdown_future = shutdown_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return shutdown(&pool, server, &shutdown_promise); });
    ASSERT_EQ(shutdown_future.wait_for(2s), std::future_status::ready);
    group.stop();
    group.join();
    delete server;
}

TEST(ProxyExecutorTest, AbortsCommittedChunkedResponseWhenDynamicLimitIsExceeded) {
    fiber::event::EventLoopGroup group(1);
    fiber::http::LocalHttp1ConnectionPoolSet pool(group);
    ASSERT_TRUE(pool.init());
    group.start();

    UpstreamState upstream_state{
            .response =
                    {
                            .body = "hello",
                            .chunked = true,
                    },
    };
    std::promise<std::uint16_t> port_promise;
    std::promise<fiber::http::HttpServer *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return start_server(&group.at(0), &upstream_state, &port_promise, &server_promise); });

    fiber::http::HttpServer *server = server_future.get();
    ASSERT_NE(server, nullptr);
    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    fiber::access_server::RouteConfigStore store;
    auto published = store.apply("orders", proxy_response_project_config(port, 4, false));
    ASSERT_TRUE(published) << published.error().message;

    fiber::access_server::ProxyRequestSender sender(pool);
    fiber::access_server::ProxyExecutor executor(sender);
    std::string output;
    std::promise<void> request_promise;
    auto request_future = request_promise.get_future();
    std::string request = "GET /proxy HTTP/1.1\r\n"
                          "Host: api.example.com\r\n"
                          "Connection: close\r\n\r\n";
    fiber::async::spawn(group.at(0), [&]() {
        return run_downstream(&group.at(0), &store, executor.adapter(), std::move(request), &output, &request_promise);
    });

    ASSERT_EQ(request_future.wait_for(5s), std::future_status::ready);
    EXPECT_NE(output.find("HTTP/1.1 201 Created\r\n"), std::string::npos);
    EXPECT_EQ(output.find("hello"), std::string::npos);
    EXPECT_EQ(output.find("READ_RESP_BODY"), std::string::npos);
    EXPECT_EQ(count_status(output, "HTTP/1.1 "), 1U);

    std::promise<void> shutdown_promise;
    auto shutdown_future = shutdown_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return shutdown(&pool, server, &shutdown_promise); });
    ASSERT_EQ(shutdown_future.wait_for(2s), std::future_status::ready);
    group.stop();
    group.join();
    delete server;
}

TEST(ProxyExecutorTest, AbortsDownstreamAfterAnUpstreamBodyEndsEarly) {
    fiber::event::EventLoopGroup group(1);
    fiber::http::LocalHttp1ConnectionPoolSet pool(group);
    ASSERT_TRUE(pool.init());
    group.start();

    UpstreamState upstream_state{
            .response =
                    {
                            .body = "hello",
                            .advertised_content_length = 10,
                    },
    };
    std::promise<std::uint16_t> port_promise;
    std::promise<fiber::http::HttpServer *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return start_server(&group.at(0), &upstream_state, &port_promise, &server_promise); });

    fiber::http::HttpServer *server = server_future.get();
    ASSERT_NE(server, nullptr);
    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    fiber::access_server::RouteConfigStore store;
    auto published = store.apply("orders", proxy_response_project_config(port));
    ASSERT_TRUE(published) << published.error().message;

    fiber::access_server::ProxyRequestSender sender(pool);
    fiber::access_server::ProxyExecutor executor(sender);
    std::string output;
    std::promise<void> request_promise;
    auto request_future = request_promise.get_future();
    std::string request = "GET /proxy HTTP/1.1\r\n"
                          "Host: api.example.com\r\n"
                          "Connection: close\r\n\r\n";
    fiber::async::spawn(group.at(0), [&]() {
        return run_downstream(&group.at(0), &store, executor.adapter(), std::move(request), &output, &request_promise);
    });

    ASSERT_EQ(request_future.wait_for(5s), std::future_status::ready);
    EXPECT_NE(output.find("HTTP/1.1 201 Created\r\n"), std::string::npos);
    EXPECT_NE(output.find("Content-Length: 10\r\n"), std::string::npos);
    EXPECT_NE(output.find("hello"), std::string::npos);
    EXPECT_EQ(output.find("READ_RESP_BODY"), std::string::npos);
    EXPECT_EQ(count_status(output, "HTTP/1.1 "), 1U);

    std::promise<void> shutdown_promise;
    auto shutdown_future = shutdown_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return shutdown(&pool, server, &shutdown_promise); });
    ASSERT_EQ(shutdown_future.wait_for(2s), std::future_status::ready);
    group.stop();
    group.join();
    delete server;
}

TEST(ProxyExecutorTest, CancelsUpstreamWhenDownstreamClosesBeforeResponseHeaders) {
    fiber::event::EventLoopGroup group(1);
    fiber::http::LocalHttp1ConnectionPoolSet pool(group);
    ASSERT_TRUE(pool.init());
    group.start();

    std::promise<void> response_promise;
    auto response_future = response_promise.get_future();
    UpstreamState upstream_state{
            .response =
                    {
                            .body = "late-response",
                            .delay = 250ms,
                            .completion = &response_promise,
                    },
    };
    std::promise<std::uint16_t> port_promise;
    std::promise<fiber::http::HttpServer *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return start_server(&group.at(0), &upstream_state, &port_promise, &server_promise); });

    fiber::http::HttpServer *server = server_future.get();
    ASSERT_NE(server, nullptr);
    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    fiber::access_server::RouteConfigStore store;
    auto published = store.apply("orders", proxy_response_project_config(port));
    ASSERT_TRUE(published) << published.error().message;

    fiber::access_server::ProxyRequestSender sender(pool);
    fiber::access_server::ProxyExecutor executor(sender);
    std::string output;
    std::promise<void> request_promise;
    std::promise<RecordingTransport *> transport_promise;
    auto request_future = request_promise.get_future();
    auto transport_future = transport_promise.get_future();
    std::string request = "GET /proxy HTTP/1.1\r\n"
                          "Host: api.example.com\r\n\r\n";
    fiber::async::spawn(group.at(0), [&]() {
        return run_downstream(&group.at(0), &store, executor.adapter(), std::move(request), &output, &request_promise,
                              &transport_promise);
    });

    RecordingTransport *transport = transport_future.get();
    ASSERT_NE(transport, nullptr);
    fiber::async::spawn(group.at(0), [transport]() { return disconnect_after(10ms, transport); });

    ASSERT_EQ(request_future.wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(output.empty());
    ASSERT_EQ(response_future.wait_for(1s), std::future_status::ready);

    std::promise<void> shutdown_promise;
    auto shutdown_future = shutdown_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return shutdown(&pool, server, &shutdown_promise); });
    ASSERT_EQ(shutdown_future.wait_for(2s), std::future_status::ready);
    group.stop();
    group.join();
    delete server;
}

TEST(ProxyExecutorTest, RelaysWebSocketUpgradeAndRawBytesInBothDirections) {
    fiber::event::EventLoopGroup group(1);
    fiber::http::LocalHttp1ConnectionPoolSet pool(group);
    ASSERT_TRUE(pool.init());
    group.start();

    std::promise<void> response_promise;
    auto response_future = response_promise.get_future();
    UpstreamState upstream_state{
            .response =
                    {
                            .websocket = true,
                            .completion = &response_promise,
                    },
    };
    std::promise<std::uint16_t> port_promise;
    std::promise<fiber::http::HttpServer *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return start_server(&group.at(0), &upstream_state, &port_promise, &server_promise); });

    fiber::http::HttpServer *server = server_future.get();
    ASSERT_NE(server, nullptr);
    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    auto config = proxy_response_project_config(port);
    (**config.routes->begin()).websocket_timeout_millis = 1000;
    fiber::access_server::RouteConfigStore store;
    auto published = store.apply("orders", std::move(config));
    ASSERT_TRUE(published) << published.error().message;

    fiber::access_server::ProxyRequestSender sender(pool);
    fiber::access_server::ProxyExecutor executor(sender);
    std::string output;
    std::promise<void> request_promise;
    auto request_future = request_promise.get_future();
    std::string request = "GET /proxy HTTP/1.1\r\n"
                          "Host: api.example.com\r\n"
                          "Connection: upgrade\r\n"
                          "Upgrade: websocket\r\n"
                          "Sec-WebSocket-Version: 13\r\n"
                          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n"
                          "client-frame";
    fiber::async::spawn(group.at(0), [&]() {
        return run_downstream(&group.at(0), &store, executor.adapter(), std::move(request), &output, &request_promise);
    });

    ASSERT_EQ(response_future.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(request_future.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(upstream_state.requests.size(), 1U);
    EXPECT_EQ(upstream_state.requests[0].connection, "upgrade");
    EXPECT_EQ(upstream_state.requests[0].upgrade, "websocket");
    EXPECT_EQ(upstream_state.requests[0].websocket_key, "dGhlIHNhbXBsZSBub25jZQ==");
    EXPECT_EQ(upstream_state.websocket_client_data, "client-frame");
    EXPECT_NE(output.find("HTTP/1.1 101 Switching Protocols\r\n"), std::string::npos);
    EXPECT_NE(output.find("Connection: Upgrade\r\n"), std::string::npos);
    EXPECT_NE(output.find("Upgrade: websocket\r\n"), std::string::npos);
    EXPECT_NE(output.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"), std::string::npos);
    EXPECT_NE(output.find("X-Custom: response-template\r\n"), std::string::npos);
    EXPECT_EQ(output.find("Content-Length:"), std::string::npos);
    EXPECT_EQ(output.find("X-Accel-Buffering:"), std::string::npos);
    EXPECT_NE(output.find("server-frame"), std::string::npos);

    std::promise<void> shutdown_promise;
    auto shutdown_future = shutdown_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return shutdown(&pool, server, &shutdown_promise); });
    ASSERT_EQ(shutdown_future.wait_for(2s), std::future_status::ready);
    group.stop();
    group.join();
    delete server;
}

TEST(ProxyExecutorTest, AbortsUpgradeBeforeRenderingAResponseTemplateFailure) {
    fiber::event::EventLoopGroup group(1);
    fiber::http::LocalHttp1ConnectionPoolSet pool(group);
    ASSERT_TRUE(pool.init());
    group.start();

    std::promise<void> response_promise;
    auto response_future = response_promise.get_future();
    UpstreamState upstream_state{
            .response =
                    {
                            .websocket = true,
                            .completion = &response_promise,
                    },
    };
    std::promise<std::uint16_t> port_promise;
    std::promise<fiber::http::HttpServer *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return start_server(&group.at(0), &upstream_state, &port_promise, &server_promise); });

    fiber::http::HttpServer *server = server_future.get();
    ASSERT_NE(server, nullptr);
    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    auto config = proxy_response_project_config(port);
    auto &route = **config.routes->begin();
    route.websocket_timeout_millis = 1000;
    route.response_headers = {
            {.name = "X-Fail", .value = "${missing}"},
    };
    fiber::access_server::RouteConfigStore store;
    auto published = store.apply("orders", std::move(config));
    ASSERT_TRUE(published) << published.error().message;

    fiber::access_server::ProxyRequestSender sender(pool);
    fiber::access_server::ProxyExecutor executor(sender);
    std::string output;
    std::promise<void> request_promise;
    auto request_future = request_promise.get_future();
    std::string request = "GET /proxy HTTP/1.1\r\n"
                          "Host: api.example.com\r\n"
                          "Connection: upgrade\r\n"
                          "Upgrade: websocket\r\n"
                          "Sec-WebSocket-Version: 13\r\n"
                          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    fiber::async::spawn(group.at(0), [&]() {
        return run_downstream(&group.at(0), &store, executor.adapter(), std::move(request), &output, &request_promise);
    });

    ASSERT_EQ(request_future.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(response_future.wait_for(2s), std::future_status::ready);
    EXPECT_NE(output.find("HTTP/1.1 500 Internal Server Error\r\n"), std::string::npos);
    EXPECT_NE(output.find(R"("name":"TEMPLATE_SCRIPT")"), std::string::npos);
    EXPECT_EQ(output.find("HTTP/1.1 101 Switching Protocols\r\n"), std::string::npos);
    EXPECT_EQ(count_status(output, "HTTP/1.1 "), 1U);

    std::promise<void> shutdown_promise;
    auto shutdown_future = shutdown_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return shutdown(&pool, server, &shutdown_promise); });
    ASSERT_EQ(shutdown_future.wait_for(2s), std::future_status::ready);
    group.stop();
    group.join();
    delete server;
}

TEST(ProxyExecutorTest, AppliesConfiguredWebSocketTunnelTimeout) {
    fiber::event::EventLoopGroup group(1);
    fiber::http::LocalHttp1ConnectionPoolSet pool(group);
    ASSERT_TRUE(pool.init());
    group.start();

    std::promise<void> response_promise;
    auto response_future = response_promise.get_future();
    UpstreamState upstream_state{
            .response =
                    {
                            .websocket = true,
                            .completion = &response_promise,
                    },
    };
    std::promise<std::uint16_t> port_promise;
    std::promise<fiber::http::HttpServer *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return start_server(&group.at(0), &upstream_state, &port_promise, &server_promise); });

    fiber::http::HttpServer *server = server_future.get();
    ASSERT_NE(server, nullptr);
    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    auto config = proxy_response_project_config(port);
    (**config.routes->begin()).websocket_timeout_millis = 20;
    fiber::access_server::RouteConfigStore store;
    auto published = store.apply("orders", std::move(config));
    ASSERT_TRUE(published) << published.error().message;

    fiber::access_server::ProxyRequestSender sender(pool);
    fiber::access_server::ProxyExecutor executor(sender);
    std::string output;
    std::promise<void> request_promise;
    auto request_future = request_promise.get_future();
    std::string request = "GET /proxy HTTP/1.1\r\n"
                          "Host: api.example.com\r\n"
                          "Connection: upgrade\r\n"
                          "Upgrade: websocket\r\n"
                          "Sec-WebSocket-Version: 13\r\n"
                          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    const auto started_at = std::chrono::steady_clock::now();
    fiber::async::spawn(group.at(0), [&]() {
        return run_downstream(&group.at(0), &store, executor.adapter(), std::move(request), &output, &request_promise,
                              nullptr, true);
    });

    ASSERT_EQ(request_future.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(response_future.wait_for(2s), std::future_status::ready);
    EXPECT_GE(std::chrono::steady_clock::now() - started_at, 15ms);
    EXPECT_NE(output.find("HTTP/1.1 101 Switching Protocols\r\n"), std::string::npos);
    EXPECT_NE(output.find("server-frame"), std::string::npos);
    EXPECT_TRUE(upstream_state.websocket_client_data.empty());

    std::promise<void> shutdown_promise;
    auto shutdown_future = shutdown_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return shutdown(&pool, server, &shutdown_promise); });
    ASSERT_EQ(shutdown_future.wait_for(2s), std::future_status::ready);
    group.stop();
    group.join();
    delete server;
}

} // namespace
