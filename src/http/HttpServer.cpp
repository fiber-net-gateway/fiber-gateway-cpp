#include <fiber/http/HttpServer.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>
#include <sys/socket.h>
#include <utility>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/async/WaitGroup.h>
#include <fiber/common/Assert.h>
#include <fiber/common/IoError.h>
#include <fiber/http/Http1Connection.h>
#include <fiber/http/HttpTransport.h>
#include <fiber/net/TcpStream.h>
#include "http/TlsAlpn.h"

namespace fiber::http {

namespace {

enum class SelectedProtocol {
    Http1,
    Http2,
    Unsupported,
};

SelectedProtocol select_protocol(std::string_view alpn) noexcept {
    if (alpn.empty() || alpn == "http/1.1") {
        return SelectedProtocol::Http1;
    }
    if (alpn == "h2") {
        return SelectedProtocol::Http2;
    }
    return SelectedProtocol::Unsupported;
}

common::IoResult<net::SocketAddress> resolve_local_addr(int fd) noexcept {
    sockaddr_storage storage{};
    socklen_t len = sizeof(storage);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&storage), &len) != 0) {
        return std::unexpected(common::io_err_from_errno(errno));
    }

    net::SocketAddress local;
    if (!net::SocketAddress::from_sockaddr(reinterpret_cast<const sockaddr *>(&storage), len, local)) {
        return std::unexpected(common::IoErr::NotSupported);
    }
    return local;
}

} // namespace

struct HttpServer::Runtime {
    struct Http1Entry {
        std::mutex mutex{};
        std::shared_ptr<Http1Connection> connection{};
        event::EventLoop *loop = nullptr;
        bool shutdown_posted = false;
    };

    struct Http2Entry {
        std::mutex mutex{};
        std::shared_ptr<Http2Connection> connection{};
        event::EventLoop *loop = nullptr;
        bool shutdown_posted = false;
    };

    event::EventLoop &owner_loop;
    event::EventLoopGroup *worker_group = nullptr;
    net::TcpListener listener;
    HttpHandler handler;
    HttpServerOptions options;
    ServerRequestFactory http2_request_factory;
    std::unique_ptr<Http3Server> http3_server{};
    std::atomic<HttpServer::State> state{HttpServer::State::Created};
    std::atomic<bool> shutting_down_flag{false};
    std::atomic<bool> close_posted{false};
    std::atomic<std::size_t> next_loop_index{0};
    mutable std::mutex lifecycle_mutex{};
    async::WaitGroup shutdown_wg{};
    bool shutdown_completion_started = false;
    async::WaitGroup tasks{};
    std::mutex connections_mutex{};
    std::vector<std::shared_ptr<Http1Entry>> http1_connections{};
    std::vector<std::shared_ptr<Http2Entry>> http2_connections{};

    Runtime(event::EventLoop &loop, HttpHandler handler, HttpServerOptions options,
            event::EventLoopGroup *worker_group) :
        owner_loop(loop), worker_group(worker_group), listener(loop), handler(std::move(handler)),
        options(std::move(options)), http2_request_factory(this->options, this->handler) {
        http1_connections.reserve(16);
        http2_connections.reserve(16);
    }

    bool begin_shutdown() {
        std::lock_guard guard(lifecycle_mutex);
        const HttpServer::State current = state.load(std::memory_order_relaxed);
        if (current == HttpServer::State::Closed || current == HttpServer::State::Closing) {
            return false;
        }
        state.store(HttpServer::State::Closing, std::memory_order_release);
        shutting_down_flag.store(true, std::memory_order_release);
        // Add the waiter before posting the owner-loop callback. This closes
        // the race where shutdown_and_wait() observes an empty group before
        // the asynchronous close request is dispatched.
        shutdown_wg.add();
        return true;
    }

    bool start_shutdown_completion() {
        std::lock_guard guard(lifecycle_mutex);
        if (shutdown_completion_started) {
            return false;
        }
        shutdown_completion_started = true;
        return true;
    }

    bool mark_bound() noexcept {
        HttpServer::State expected = HttpServer::State::Created;
        return state.compare_exchange_strong(expected, HttpServer::State::Bound, std::memory_order_acq_rel);
    }

    bool start_serving() {
        std::lock_guard guard(lifecycle_mutex);
        if (state.load(std::memory_order_relaxed) != HttpServer::State::Bound) {
            return false;
        }
        state.store(HttpServer::State::Running, std::memory_order_release);
        tasks.add();
        return true;
    }

    [[nodiscard]] bool running() const noexcept {
        return state.load(std::memory_order_acquire) == HttpServer::State::Running;
    }

    [[nodiscard]] bool closing() const noexcept {
        const HttpServer::State current = state.load(std::memory_order_acquire);
        return current == HttpServer::State::Closing || current == HttpServer::State::Closed;
    }

    [[nodiscard]] int fd() const noexcept {
        std::lock_guard guard(lifecycle_mutex);
        return listener.fd();
    }

    static fiber::async::DetachedTask shutdown_http1_entry(std::shared_ptr<Http1Entry> entry) {
        std::shared_ptr<Http1Connection> connection;
        {
            std::lock_guard guard(entry->mutex);
            connection = entry->connection;
            entry->shutdown_posted = false;
        }
        if (connection) {
            connection->shutdown();
        }
        co_return;
    }

    static fiber::async::DetachedTask shutdown_http2_entry(std::shared_ptr<Http2Entry> entry) {
        std::shared_ptr<Http2Connection> connection;
        {
            std::lock_guard guard(entry->mutex);
            connection = entry->connection;
            entry->shutdown_posted = false;
        }
        if (connection) {
            connection->shutdown(common::IoErr::Canceled);
        }
        co_return;
    }

    void add_http1(const std::shared_ptr<Http1Entry> &entry) {
        std::lock_guard guard(connections_mutex);
        http1_connections.push_back(entry);
    }

    void remove_http1(const std::shared_ptr<Http1Entry> &entry) {
        std::lock_guard guard(connections_mutex);
        std::erase(http1_connections, entry);
    }

    void add_http2(const std::shared_ptr<Http2Entry> &entry) {
        std::lock_guard guard(connections_mutex);
        http2_connections.push_back(entry);
    }

    void remove_http2(const std::shared_ptr<Http2Entry> &entry) {
        std::lock_guard guard(connections_mutex);
        std::erase(http2_connections, entry);
    }

    void request_shutdown() {
        std::lock_guard guard(connections_mutex);
        for (const auto &entry: http1_connections) {
            std::lock_guard entry_guard(entry->mutex);
            if (!entry->connection || entry->loop == nullptr || entry->shutdown_posted) {
                continue;
            }
            entry->shutdown_posted = true;
            async::spawn(*entry->loop,
                         [entry]() -> fiber::async::DetachedTask { return Runtime::shutdown_http1_entry(entry); });
        }
        for (const auto &entry: http2_connections) {
            std::lock_guard entry_guard(entry->mutex);
            if (!entry->connection || entry->loop == nullptr || entry->shutdown_posted) {
                continue;
            }
            entry->shutdown_posted = true;
            async::spawn(*entry->loop,
                         [entry]() -> fiber::async::DetachedTask { return Runtime::shutdown_http2_entry(entry); });
        }
    }
};

HttpServer::HttpServer(event::EventLoop &loop, HttpHandler handler, HttpServerOptions options,
                       event::EventLoopGroup *worker_group) :
    runtime_(std::make_shared<Runtime>(loop, std::move(handler), std::move(options), worker_group)) {}

HttpServer::~HttpServer() { request_close(); }

fiber::common::IoResult<void> HttpServer::bind(const net::SocketAddress &addr, const net::ListenOptions &options) {
    auto runtime = runtime_;
    std::unique_lock lifecycle_guard(runtime->lifecycle_mutex);
    if (runtime->state.load(std::memory_order_acquire) != State::Created) {
        return std::unexpected(common::IoErr::Already);
    }
    if (runtime->options.http3.enabled && !runtime->options.tls.enabled()) {
        return std::unexpected(common::IoErr::Invalid);
    }

    auto result = runtime->listener.bind(addr, options);
    if (!result) {
        return std::unexpected(result.error());
    }

    net::SocketAddress bound_addr = addr;
    auto local_addr = resolve_local_addr(runtime->listener.fd());
    if (!local_addr) {
        runtime->listener.close();
        return std::unexpected(local_addr.error());
    }
    bound_addr = *local_addr;

    if (runtime->options.tls.enabled()) {
        normalize_http_server_alpn(runtime->options.tls);
    }
    if (runtime->options.http3.enabled) {
        runtime->http3_server = std::make_unique<Http3Server>(runtime->listener.loop(), runtime->handler,
                                                              runtime->options, runtime->worker_group);
        if (!runtime->http3_server) {
            runtime->listener.close();
            return std::unexpected(common::IoErr::NoMem);
        }
        auto http3_bound = runtime->http3_server->bind(bound_addr);
        if (!http3_bound) {
            runtime->http3_server.reset();
            runtime->listener.close();
            return std::unexpected(http3_bound.error());
        }
    }
    if (!runtime->mark_bound()) {
        runtime->http3_server.reset();
        runtime->listener.close();
        return std::unexpected(common::IoErr::Canceled);
    }
    return {};
}

fiber::async::DetachedTask HttpServer::noop_task() { co_return; }

fiber::async::DetachedTask HttpServer::serve() {
    auto runtime = runtime_;
    if (event::EventLoop::current_or_null() != &runtime->owner_loop) {
        return noop_task();
    }
    if (!runtime->start_serving()) {
        return noop_task();
    }
    return serve_loop(std::move(runtime));
}

fiber::async::DetachedTask HttpServer::serve_loop(std::shared_ptr<Runtime> runtime) {
    struct TaskGuard {
        std::shared_ptr<Runtime> runtime;
        ~TaskGuard() {
            if (runtime) {
                runtime->tasks.done();
            }
        }
    } task_guard{runtime};

    auto *accept_loop = event::EventLoop::current_or_null();
    FIBER_ASSERT(accept_loop != nullptr);
    FIBER_ASSERT(accept_loop == &runtime->owner_loop);

    if (runtime->http3_server && runtime->running()) {
        runtime->http3_server->serve();
    }

    while (runtime->listener.valid() && runtime->running()) {
        auto accept_result = co_await runtime->listener.accept();
        if (!accept_result) {
            if (accept_result.error() == common::IoErr::Canceled || accept_result.error() == common::IoErr::BadFd) {
                break;
            }
            continue;
        }

        if (!runtime->running()) {
            continue;
        }

        auto accept = std::move(*accept_result);
        runtime->tasks.add();
        event::EventLoop &connection_loop = select_connection_loop(runtime);
        fiber::async::spawn(connection_loop,
                            [runtime, accept = std::move(accept)]() mutable -> fiber::async::DetachedTask {
                                return handle_connection(std::move(runtime), std::move(accept));
                            });
    }
    co_return;
}

event::EventLoop &HttpServer::select_connection_loop(const std::shared_ptr<Runtime> &runtime) noexcept {
    if (!runtime->worker_group || runtime->worker_group->size() == 0) {
        return event::EventLoop::current();
    }
    const std::size_t index = runtime->next_loop_index.fetch_add(1, std::memory_order_relaxed);
    return runtime->worker_group->at(index % runtime->worker_group->size());
}

fiber::async::DetachedTask HttpServer::handle_connection(std::shared_ptr<Runtime> runtime, net::AcceptResult accept) {
    struct TaskGuard {
        std::shared_ptr<Runtime> runtime;
        ~TaskGuard() {
            if (runtime) {
                runtime->tasks.done();
            }
        }
    } task_guard{runtime};

    if (!runtime || runtime->closing()) {
        co_return;
    }

    std::unique_ptr<HttpTransport> transport;
    if (runtime->options.tls.enabled()) {
        auto tls_result = TlsTransport::create(event::EventLoop::current(), std::move(accept), runtime->options.tls,
                                               runtime->options.tcp);
        if (!tls_result) {
            co_return;
        }
        transport = std::move(*tls_result);
        auto hs_result = co_await transport->handshake(runtime->options.tls.handshake_timeout);
        if (!hs_result) {
            transport->close();
            co_return;
        }
        if (runtime->closing()) {
            transport->close();
            co_return;
        }
    } else {
        auto tcp_result = TcpTransport::create(event::EventLoop::current(), std::move(accept), runtime->options.tcp);
        if (!tcp_result) {
            co_return;
        }
        transport = std::move(*tcp_result);
    }

    if (!transport) {
        co_return;
    }

    if (!runtime->options.tls.enabled()) {
        co_await serve_http1(runtime, std::move(transport));
        co_return;
    }

    switch (select_protocol(transport->negotiated_alpn())) {
        case SelectedProtocol::Http1:
            co_await serve_http1(runtime, std::move(transport));
            co_return;
        case SelectedProtocol::Http2:
            co_await serve_http2(runtime, std::move(transport));
            co_return;
        case SelectedProtocol::Unsupported:
            transport->close();
            co_return;
    }
}

fiber::async::Task<void> HttpServer::serve_http1(std::shared_ptr<Runtime> runtime,
                                                 std::unique_ptr<HttpTransport> transport) {
    if (!transport) {
        co_return;
    }

    auto entry = std::make_shared<Runtime::Http1Entry>();
    auto connection = std::make_shared<Http1Connection>(nullptr, std::move(transport), runtime->handler,
                                                        runtime->options, &runtime->shutting_down_flag);
    {
        std::lock_guard guard(entry->mutex);
        entry->connection = connection;
        entry->loop = &connection->loop();
    }
    runtime->add_http1(entry);
    if (runtime->closing()) {
        runtime->request_shutdown();
    }

    co_await connection->run();

    {
        std::lock_guard guard(entry->mutex);
        entry->connection.reset();
    }
    runtime->remove_http1(entry);
    co_return;
}

fiber::async::Task<void> HttpServer::serve_http2(std::shared_ptr<Runtime> runtime,
                                                 std::unique_ptr<HttpTransport> transport) {
    if (!transport) {
        co_return;
    }

    auto connection = std::make_shared<Http2Connection>(make_http2_options(runtime->options),
                                                        &runtime->http2_request_factory, ServerRequestFactory::ops());
    if (!connection) {
        co_return;
    }
    if (connection->start(std::move(transport)) != common::IoErr::None) {
        co_return;
    }

    auto entry = std::make_shared<Runtime::Http2Entry>();
    {
        std::lock_guard guard(entry->mutex);
        entry->connection = connection;
        entry->loop = &connection->loop();
    }
    runtime->add_http2(entry);
    if (runtime->closing()) {
        runtime->request_shutdown();
    }

    auto close_result = co_await connection->wait_closed();
    (void) close_result;

    {
        std::lock_guard guard(entry->mutex);
        entry->connection.reset();
    }
    runtime->remove_http2(entry);
    co_return;
}

Http2Connection::Options HttpServer::make_http2_options(const HttpServerOptions &http_options) noexcept {
    Http2Connection::Options options;
    options.role = Http2Connection::ConnectionRole::Server;
    options.read_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(http_options.keep_alive_timeout);
    options.write_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(http_options.write_timeout);
    options.keepalive_ping_interval =
            std::chrono::duration_cast<std::chrono::milliseconds>(http_options.keep_alive_timeout);
    options.enable_connect_protocol = http_options.enable_extended_connect;
    return options;
}

void HttpServer::close_on_owner_loop(const std::shared_ptr<Runtime> &runtime) noexcept {
    FIBER_ASSERT(runtime != nullptr);
    FIBER_ASSERT(runtime->owner_loop.in_loop());

    runtime->close_posted.store(true, std::memory_order_release);
    (void) runtime->begin_shutdown();
    {
        std::lock_guard guard(runtime->lifecycle_mutex);
        runtime->listener.close();
        if (runtime->http3_server) {
            runtime->http3_server->close();
        }
    }
    runtime->request_shutdown();

    if (!runtime->start_shutdown_completion()) {
        return;
    }
    async::spawn(runtime->owner_loop,
                 [runtime]() -> fiber::async::DetachedTask { return HttpServer::finish_shutdown(std::move(runtime)); });
}

fiber::async::DetachedTask HttpServer::finish_shutdown(std::shared_ptr<Runtime> runtime) {
    if (runtime->http3_server) {
        co_await runtime->http3_server->shutdown_and_wait();
    }
    co_await runtime->tasks.join();
    runtime->state.store(State::Closed, std::memory_order_release);
    runtime->shutdown_wg.done();
    co_return;
}

void HttpServer::request_close() noexcept {
    auto runtime = runtime_;
    (void) runtime->begin_shutdown();
    if (runtime->owner_loop.in_loop()) {
        close_on_owner_loop(runtime);
        return;
    }

    if (runtime->close_posted.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    async::spawn(runtime->owner_loop, [runtime]() -> fiber::async::DetachedTask {
        close_on_owner_loop(runtime);
        co_return;
    });
}

void HttpServer::close() noexcept { request_close(); }

fiber::async::Task<void> HttpServer::shutdown_and_wait() {
    request_close();
    co_await runtime_->shutdown_wg.join();
    co_return;
}

HttpServer::State HttpServer::state() const noexcept { return runtime_->state.load(std::memory_order_acquire); }

bool HttpServer::shutting_down() const noexcept {
    const State current = state();
    return current == State::Closing || current == State::Closed;
}

int HttpServer::fd() const noexcept { return runtime_->fd(); }

} // namespace fiber::http
