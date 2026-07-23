#include "AiServer.h"

#include <cstdint>
#include <string_view>
#include <utility>

#include "async/WhenAny.h"
#include "common/Assert.h"
#include "http/HttpExchange.h"
#include "http/HttpExchangeIo.h"
#include "http/HttpHeaders.h"

namespace fiber::ai_server {

namespace {

constexpr std::string_view kHealthPath = "/health";
constexpr std::string_view kHealthBody = "{\"status\":\"ok\"}\n";
constexpr std::string_view kReadyPath = "/ready";
constexpr std::string_view kReadyBody = "{\"status\":\"ready\"}\n";
constexpr std::string_view kNotReadyBody = "{\"status\":\"not_ready\"}\n";
constexpr std::string_view kMethodNotAllowedBody = "{\"error\":\"method_not_allowed\"}\n";
constexpr std::string_view kNotFoundBody = "{\"error\":\"not_found\"}\n";

http::HttpServerOptions make_server_options() noexcept {
    http::HttpServerOptions options;
    options.drain_unread_body = true;
    return options;
}

async::Task<void> send_json(http::HttpExchange &exchange, int status_code, std::string_view body,
                            bool allow_get = false) {
    http::HttpHeaders headers(exchange.pool());
    headers.set_view("Content-Type", "application/json");
    if (allow_get) {
        headers.set_view("Allow", "GET");
    }

    auto header_result = co_await exchange.send_header({
            .kind = http::OutgoingHeaderKind::Final,
            .status_code = status_code,
            .headers = &headers,
            .body = http::HttpBodySpec::ContentLength(body.size()),
            .connection_mode = http::ResponseConnectionMode::Auto,
            .end_stream = body.empty(),
    });
    if (!header_result || body.empty()) {
        co_return;
    }

    (void) co_await exchange.write_body(reinterpret_cast<const std::uint8_t *>(body.data()), body.size(), true);
}

} // namespace

AiServer::AiServer(event::EventLoop &accept_loop, event::EventLoopGroup &worker_group) :
    accept_loop_(&accept_loop), worker_group_(&worker_group), workers_(worker_group.size()),
    server_(
            accept_loop, [this](http::HttpExchange &exchange) { return handle(exchange); }, make_server_options(),
            &worker_group) {
    FIBER_ASSERT(worker_group.size() > 0);
    config_stop_publisher_ = config_stop_.acquire_publisher();
    FIBER_ASSERT(config_stop_publisher_.has_value());
}

AiServer::~AiServer() {
    FIBER_ASSERT(config_tasks_.empty());
    FIBER_ASSERT(initial_installs_.empty());
}

async::Task<bool> AiServer::start_config_workers(LlmConfigManager &config_manager) noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    FIBER_ASSERT(!config_workers_started_);
    config_workers_started_ = true;

    const std::size_t worker_count = worker_group_->size();
    initial_installs_.add(worker_count);
    config_tasks_.add(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i) {
        async::spawn(worker_group_->at(i), [this, i, subscription = config_manager.subscribe_snapshot()]() mutable {
            return watch_config(i, std::move(subscription));
        });
    }
    co_await initial_installs_.join();
    co_return !initial_install_failed_.load(std::memory_order_acquire);
}

async::DetachedTask AiServer::watch_config(std::size_t worker_index,
                                           LlmConfigManager::SnapshotSubscriber subscription) noexcept {
    FIBER_ASSERT(worker_index < workers_.size());
    FIBER_ASSERT(&event::EventLoop::current() == &worker_group_->at(worker_index));

    WorkerState &worker = workers_[worker_index];
    auto stop = config_stop_.subscribe();
    auto stop_snapshot = stop.current();
    auto snapshot = subscription.current();
    for (;;) {
        if (stop_snapshot.value && *stop_snapshot.value) {
            break;
        }
        if (snapshot.value) {
            worker.config = snapshot.value;
            if (!worker.initial_installed) {
                worker.initial_installed = true;
                initial_installs_.done();
            }
        }
        auto result = co_await async::when_any(
                [&subscription, version = snapshot.version]() { return subscription.next(version); },
                [&stop, version = stop_snapshot.version]() { return stop.next(version); });
        if (result.is<1>()) {
            std::move(result).get<1>();
            break;
        }
        snapshot = std::move(result).get<0>();
        stop_snapshot = stop.current();
    }
    if (!worker.initial_installed) {
        initial_install_failed_.store(true, std::memory_order_release);
        initial_installs_.done();
    }
    config_tasks_.done();
}

common::IoResult<void> AiServer::bind(const net::SocketAddress &address, const net::ListenOptions &options) {
    FIBER_ASSERT(accept_loop_->in_loop());
    return server_.bind(address, options);
}

async::DetachedTask AiServer::serve() { return server_.serve(); }

void AiServer::close() { server_.close(); }

async::Task<void> AiServer::shutdown_and_wait() {
    FIBER_ASSERT(accept_loop_->in_loop());
    co_await server_.shutdown_and_wait();
    if (config_workers_started_ && !config_workers_stopping_) {
        config_workers_stopping_ = true;
        config_stop_publisher_->publish(true);
    }
    co_await config_tasks_.join();
}

int AiServer::fd() const noexcept { return server_.fd(); }

std::shared_ptr<const LlmConfigSnapshot> AiServer::current_config() const noexcept {
    const event::EventLoop &loop = event::EventLoop::current();
    FIBER_ASSERT(loop.group() == worker_group_);
    const std::size_t index = loop.group_index();
    FIBER_ASSERT(index < workers_.size());
    return workers_[index].config;
}

async::Task<void> AiServer::handle(http::HttpExchange &exchange) {
    const std::string_view path = exchange.uri().path;
    if (path != kHealthPath && path != kReadyPath) {
        co_await send_json(exchange, 404, kNotFoundBody);
        co_return;
    }
    if (exchange.method() != http::HttpMethod::Get) {
        co_await send_json(exchange, 405, kMethodNotAllowedBody, true);
        co_return;
    }

    if (path == kHealthPath) {
        co_await send_json(exchange, 200, kHealthBody);
        co_return;
    }
    const auto config = current_config();
    if (config) {
        co_await send_json(exchange, 200, kReadyBody);
        co_return;
    }
    co_await send_json(exchange, 503, kNotReadyBody);
}

} // namespace fiber::ai_server
