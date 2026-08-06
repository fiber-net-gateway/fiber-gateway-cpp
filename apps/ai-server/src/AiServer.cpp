#include "AiServer.h"
#include "observability/AiServerCatRequest.h"
#include "observability/AiServerLogCategories.h"
#include "server/LlmRequestHandler.h"
#include "server/McpHttpHandler.h"
#include "server/TokenRateLimitHttpHandler.h"

#include <chrono>
#include <cstdint>
#include <string_view>
#include <utility>

#include <fiber/cat/CatClient.h>
#include "async/Sleep.h"
#include "async/WhenAny.h"
#include "common/Assert.h"
#include "http/HttpExchange.h"
#include "http/HttpExchangeIo.h"
#include "http/HttpHeaders.h"
#include "log/Log.h"

namespace fiber::ai_server {

namespace {

DEFINE_LOGGER(LOG_HTTP, kAiServerHttpLogger);

constexpr std::string_view kHealthPath = "/health";
constexpr std::string_view kHealthBody = "{\"status\":\"ok\"}\n";
constexpr std::string_view kReadyPath = "/ready";
constexpr std::string_view kOpenAiChatPath = "/v1/chat/completions";
constexpr std::string_view kAnthropicMessagesPath = "/v1/messages";
constexpr std::string_view kAnthropicMessageAliasPath = "/v1/message";
constexpr std::string_view kMetricsPath = "/metrics";
constexpr std::string_view kMetricsAliasPath = "/_metric_prometheus";
constexpr std::string_view kRateLimitCheckPath = "/internal/llm/rate-limit/check";
constexpr std::string_view kRateLimitSettlePath = "/internal/llm/rate-limit/settle";
constexpr std::string_view kReadyBody = "{\"status\":\"ready\"}\n";
constexpr std::string_view kNotReadyBody = "{\"status\":\"not_ready\"}\n";
constexpr std::string_view kMethodNotAllowedBody = "{\"error\":\"method_not_allowed\"}\n";
constexpr std::string_view kNotFoundBody = "{\"error\":\"not_found\"}\n";
constexpr std::chrono::minutes kRateLimitSweepInterval{1};
constexpr std::chrono::seconds kMcpSessionSweepInterval{65};

std::int64_t wall_now_millis() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
}

http::HttpServerOptions make_server_options() noexcept {
    http::HttpServerOptions options;
    options.drain_unread_body = true;
    return options;
}

async::Task<void> send_json(http::HttpExchange &exchange, const AiServerCatRequest &cat_request, int status_code,
                            std::string_view body, bool allow_get = false) {
    http::HttpHeaders headers(exchange.pool());
    headers.set_view("Content-Type", "application/json");
    if (allow_get) {
        headers.set_view("Allow", "GET");
    }
    cat_request.inject_response_header(headers);

    auto header_result = co_await exchange.send_header({
            .kind = http::OutgoingHeaderKind::Final,
            .status_code = status_code,
            .headers = &headers,
            .body = http::HttpBodySpec::ContentLength(body.size()),
            .connection_mode = http::ResponseConnectionMode::Auto,
            .end_stream = body.empty(),
    });
    if (!header_result) {
        LOG(LOG_HTTP, DEBUG) << "response header write failed path=" << fiber::log::quoted(exchange.uri().path)
                             << " status=" << status_code << " io_error=" << common::io_err_name(header_result.error());
        co_return;
    }
    if (body.empty()) {
        co_return;
    }

    auto body_result =
            co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(body.data()), body.size(), true);
    if (!body_result) {
        LOG(LOG_HTTP, DEBUG) << "response body write failed path=" << fiber::log::quoted(exchange.uri().path)
                             << " status=" << status_code << " io_error=" << common::io_err_name(body_result.error());
    }
}

} // namespace

AiServer::AiServer(event::EventLoop &accept_loop, event::EventLoopGroup &worker_group, cat::CatClient *cat_client,
                   std::size_t audit_max_record_bytes, log::AppenderId audit_appender_id, McpConfigStore *mcp_config,
                   McpScriptServices *mcp_script_services) :
    accept_loop_(&accept_loop), worker_group_(&worker_group), cat_client_(cat_client),
    audit_max_record_bytes_(audit_max_record_bytes), audit_appender_id_(audit_appender_id),
    workers_(worker_group.size()), rate_limiters_(worker_group.size()), mcp_forwarder_(worker_group, rate_limit_ring_),
    rate_limit_remote_client_(worker_group),
    rate_limit_coordinator_(rate_limiters_, rate_limit_ring_, rate_limit_remote_client_),
    provider_connections_(worker_group), provider_client_(provider_connections_), metrics_(worker_group),
    mcp_config_(mcp_config), mcp_script_services_(mcp_script_services),
    server_(
            accept_loop, [this](http::HttpExchange &exchange) { return handle(exchange); }, make_server_options(),
            &worker_group) {
    FIBER_ASSERT(worker_group.size() > 0);
    config_stop_publisher_ = config_stop_.acquire_publisher();
    FIBER_ASSERT(config_stop_publisher_.has_value());
}

AiServer::~AiServer() {
    metrics_.stop_collecting();
    FIBER_ASSERT(config_tasks_.empty());
    FIBER_ASSERT(initial_installs_.empty());
    FIBER_ASSERT(sweep_tasks_.empty());
    FIBER_ASSERT(mcp_tasks_.empty());
    FIBER_ASSERT(cat_detach_tasks_.empty());
}

bool AiServer::start_mcp(std::string node_prefix) {
    FIBER_ASSERT(accept_loop_->in_loop());
    if (!mcp_config_ || node_prefix.size() != 12 || mcp_handler_) {
        return false;
    }
    mcp_sessions_ = std::make_unique<McpSessionManager>(std::move(node_prefix));
    mcp_handler_ = std::make_unique<McpHttpHandler>(*mcp_config_, *mcp_sessions_, &mcp_forwarder_);
    mcp_tasks_.add();
    async::spawn([this]() { return sweep_mcp_sessions(); });
    return true;
}

async::Task<bool> AiServer::start_config_workers(LlmConfigManager &config_manager) noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    FIBER_ASSERT(!config_workers_started_);
    if (!metrics_.valid() || !rate_limit_coordinator_.init()) {
        co_return false;
    }
    if (!mcp_forwarder_.init()) {
        co_await rate_limit_coordinator_.shutdown();
        co_return false;
    }
    if (!co_await provider_connections_.init()) {
        co_await mcp_forwarder_.shutdown();
        co_await rate_limit_coordinator_.shutdown();
        co_return false;
    }
    if (mcp_script_services_ && !co_await mcp_script_services_->init_workers()) {
        co_await provider_connections_.shutdown();
        co_await mcp_forwarder_.shutdown();
        co_await rate_limit_coordinator_.shutdown();
        co_return false;
    }
    config_workers_started_ = true;
    sweep_tasks_.add();
    async::spawn([this]() { return sweep_rate_limits(); });

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

async::DetachedTask AiServer::sweep_rate_limits() noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    auto stop = config_stop_.subscribe();
    auto stop_snapshot = stop.current();
    while (!stop_snapshot.value || !*stop_snapshot.value) {
        auto result =
                co_await async::when_any([]() { return async::sleep(kRateLimitSweepInterval); },
                                         [&stop, version = stop_snapshot.version]() { return stop.next(version); });
        if (result.is<1>()) {
            std::move(result).get<1>();
            break;
        }
        std::move(result).get<0>();
        (void) rate_limiters_.sweep_expired(wall_now_millis());
        stop_snapshot = stop.current();
    }
    sweep_tasks_.done();
}

async::DetachedTask AiServer::sweep_mcp_sessions() noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    auto stop = config_stop_.subscribe();
    auto stop_snapshot = stop.current();
    while (!stop_snapshot.value || !*stop_snapshot.value) {
        auto result =
                co_await async::when_any([]() { return async::sleep(kMcpSessionSweepInterval); },
                                         [&stop, version = stop_snapshot.version]() { return stop.next(version); });
        if (result.is<1>()) {
            std::move(result).get<1>();
            break;
        }
        std::move(result).get<0>();
        if (mcp_sessions_) {
            (void) mcp_sessions_->sweep(event::EventLoop::current().now());
        }
        stop_snapshot = stop.current();
    }
    mcp_tasks_.done();
}

async::DetachedTask AiServer::detach_cat_worker() noexcept {
    if (cat_client_) {
        (void) co_await cat_client_->detach_current_event_loop();
    }
    cat_detach_tasks_.done();
}

async::Task<void> AiServer::detach_cat_workers() noexcept {
    if (!cat_client_ || cat_client_->state() != cat::CatClientState::Running) {
        co_return;
    }
    cat_detach_tasks_.add(worker_group_->size());
    for (std::size_t i = 0; i < worker_group_->size(); ++i) {
        async::spawn(worker_group_->at(i), [this]() { return detach_cat_worker(); });
    }
    co_await cat_detach_tasks_.join();
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
            metrics_.set_config_generation(snapshot.value->generation);
            if (snapshot.value->project) {
                worker.provider_runtime.reconcile(*snapshot.value->project);
            }
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
    worker.config.reset();
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
    if (mcp_sessions_) {
        mcp_sessions_->close_all();
    }
    if ((config_workers_started_ || mcp_handler_) && !config_workers_stopping_) {
        config_workers_stopping_ = true;
        config_stop_publisher_->publish(true);
    }
    co_await server_.shutdown_and_wait();
    co_await detach_cat_workers();
    co_await config_tasks_.join();
    co_await sweep_tasks_.join();
    co_await mcp_tasks_.join();
    co_await rate_limit_coordinator_.shutdown();
    co_await mcp_forwarder_.shutdown();
    co_await provider_connections_.shutdown();
    if (mcp_script_services_) {
        co_await mcp_script_services_->shutdown_workers();
    }
    metrics_.stop_collecting();
    co_await metrics_.wait_for_idle();
    mcp_handler_.reset();
}

int AiServer::fd() const noexcept { return server_.fd(); }

std::shared_ptr<const LlmConfigSnapshot> AiServer::current_config() const noexcept {
    const event::EventLoop &loop = event::EventLoop::current();
    FIBER_ASSERT(loop.group() == worker_group_);
    const std::size_t index = loop.group_index();
    FIBER_ASSERT(index < workers_.size());
    return workers_[index].config;
}

AiServer::WorkerState &AiServer::current_worker() noexcept {
    event::EventLoop &loop = event::EventLoop::current();
    FIBER_ASSERT(loop.group() == worker_group_);
    const std::size_t index = loop.group_index();
    FIBER_ASSERT(index < workers_.size());
    return workers_[index];
}

async::Task<void> AiServer::handle(http::HttpExchange &exchange) {
    AiServerCatRequest cat_request(exchange, cat_client_);
    const std::string_view path = exchange.uri().path;
    if (path == kRateLimitCheckPath || path == kRateLimitSettlePath) {
        TokenRateLimitHttpHandler handler(rate_limiters_, &cat_request);
        if (path == kRateLimitCheckPath) {
            co_await handler.handle_check(exchange);
        } else {
            co_await handler.handle_settle(exchange);
        }
        co_return;
    }
    if (path == kMetricsPath || path == kMetricsAliasPath) {
        if (exchange.method() != http::HttpMethod::Get) {
            co_await send_json(exchange, cat_request, 405, kMethodNotAllowedBody, true);
            co_return;
        }
        const auto ring = rate_limit_ring_.snapshot();
        const log::AppenderStats audit_stats = log::LoggerManager::global().appender_stats(audit_appender_id_);
        auto collected = co_await metrics_.collect(event::EventLoop::current().io_buf_node_pool(),
                                                   rate_limiters_.stats(), ring ? ring->nodes.size() : 0, &audit_stats);
        if (!collected) {
            co_await send_json(exchange, cat_request, collected.error() == common::IoErr::Busy ? 503 : 500,
                               collected.error() == common::IoErr::Busy
                                       ? std::string_view("{\"error\":\"metrics_busy\"}\n")
                                       : std::string_view("{\"error\":\"metrics_unavailable\"}\n"));
            co_return;
        }
        http::HttpHeaders headers(exchange.pool());
        headers.set_view("Content-Type", "text/plain; version=0.0.4; charset=utf-8");
        cat_request.inject_response_header(headers);
        const std::size_t size = collected->readable_bytes();
        auto header = co_await exchange.send_header({
                .kind = http::OutgoingHeaderKind::Final,
                .status_code = 200,
                .headers = &headers,
                .body = http::HttpBodySpec::ContentLength(size),
                .connection_mode = http::ResponseConnectionMode::Auto,
                .end_stream = size == 0,
        });
        if (header && size != 0) {
            collected->mark_complete();
            (void) co_await exchange.write_all(std::move(*collected));
        }
        co_return;
    }
    if (path == kOpenAiChatPath || path == kAnthropicMessagesPath || path == kAnthropicMessageAliasPath) {
        WorkerState &worker = current_worker();
        const LlmWireProtocol protocol =
                path == kOpenAiChatPath ? LlmWireProtocol::OpenAiChatCompletions : LlmWireProtocol::AnthropicMessages;
        AiServerMetrics::Worker &metrics = metrics_.worker(event::EventLoop::current().group_index());
        metrics.request_started(protocol);
        const auto started = event::EventLoop::current().now();
        LlmRequestHandler handler(provider_client_, worker.provider_runtime, rate_limit_coordinator_, metrics,
                                  audit_max_record_bytes_);
        co_await handler.handle(exchange, protocol, worker.config, &cat_request);
        metrics.request_finished(
                protocol, exchange.response_stats(),
                std::chrono::duration_cast<std::chrono::microseconds>(event::EventLoop::current().now() - started));
        co_return;
    }
    if (mcp_handler_ && mcp_handler_->matches(path)) {
        co_await mcp_handler_->handle(exchange);
        co_return;
    }
    if (path != kHealthPath && path != kReadyPath) {
        co_await send_json(exchange, cat_request, 404, kNotFoundBody);
        co_return;
    }
    if (exchange.method() != http::HttpMethod::Get) {
        co_await send_json(exchange, cat_request, 405, kMethodNotAllowedBody, true);
        co_return;
    }

    if (path == kHealthPath) {
        co_await send_json(exchange, cat_request, 200, kHealthBody);
        co_return;
    }
    const auto config = current_config();
    const auto ring = rate_limit_ring_.snapshot();
    if (config && ring && !ring->entries.empty()) {
        co_await send_json(exchange, cat_request, 200, kReadyBody);
        co_return;
    }
    co_await send_json(exchange, cat_request, 503, kNotReadyBody);
}

} // namespace fiber::ai_server
