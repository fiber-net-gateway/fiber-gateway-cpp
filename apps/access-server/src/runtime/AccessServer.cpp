#include "AccessServer.h"
#include "../observability/AccessRequestTelemetry.h"

#include <fiber/async/Spawn.h>
#include <fiber/cat/CatClient.h>
#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>
#include <fiber/http/HttpBodySpec.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/http/HttpHeaders.h>

namespace fiber::access_server {
namespace {

http::HttpServerOptions make_http_options() noexcept {
    http::HttpServerOptions options;
    options.drain_unread_body = true;
    return options;
}

} // namespace

AccessServer::AccessServer(event::EventLoop &accept_loop, event::EventLoopGroup &workers,
                           const RouteConfigStore &config_store, ProxyClusterMatcher cluster_matcher,
                           AccessServerOptions options) :
    accept_loop_(&accept_loop), workers_(&workers), pool_(workers),
    executor_(pool_, cluster_matcher, dns_.adapter(), options.executor),
    handler_(config_store, options.script_adapter,
             AccessRequestHandlerOptions{
                     .default_max_request_body_size = options.default_max_request_body_size,
                     .test_mode = options.test_mode,
             },
             executor_.adapter()),
    metrics_(workers), cat_client_(options.cat_client),
    server_(
            accept_loop, [this](http::HttpExchange &exchange) { return handle(exchange); }, make_http_options(),
            &workers),
    metrics_server_(
            accept_loop, [this](http::HttpExchange &exchange) { return handle_metrics(exchange); }, make_http_options(),
            &workers) {
    FIBER_ASSERT(workers.size() > 0);
}

AccessServer::~AccessServer() {
    metrics_.stop_collecting();
    FIBER_ASSERT(!initialized_);
    FIBER_ASSERT(cat_detach_tasks_.empty());
}

common::IoResult<void> AccessServer::initialize() noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    if (initialized_) {
        return std::unexpected(common::IoErr::Already);
    }
    if (!metrics_.valid()) {
        return std::unexpected(common::IoErr::NoMem);
    }
    if (!dns_.init(*workers_)) {
        return std::unexpected(common::IoErr::NoMem);
    }
    if (!pool_.init()) {
        dns_.shutdown();
        return std::unexpected(common::IoErr::NoMem);
    }
    initialized_ = true;
    return {};
}

common::IoResult<void> AccessServer::bind(const net::SocketAddress &address, const net::ListenOptions &options) {
    FIBER_ASSERT(accept_loop_->in_loop());
    FIBER_ASSERT(initialized_);
    return server_.bind(address, options);
}

common::IoResult<void> AccessServer::bind_metrics(const net::SocketAddress &address,
                                                  const net::ListenOptions &options) {
    FIBER_ASSERT(accept_loop_->in_loop());
    FIBER_ASSERT(initialized_);
    auto bound = metrics_server_.bind(address, options);
    if (bound) {
        metrics_bound_ = true;
    }
    return bound;
}

async::DetachedTask AccessServer::serve() { return server_.serve(); }

async::DetachedTask AccessServer::serve_metrics() { return metrics_server_.serve(); }

async::Task<void> AccessServer::shutdown_and_wait() noexcept {
    FIBER_ASSERT(accept_loop_->in_loop());
    if (metrics_bound_) {
        co_await metrics_server_.shutdown_and_wait();
        metrics_bound_ = false;
    }
    co_await server_.shutdown_and_wait();
    metrics_.stop_collecting();
    co_await metrics_.wait_for_idle();
    co_await detach_cat_workers();
    co_await pool_.shutdown_async();
    if (initialized_) {
        dns_.shutdown();
    }
    initialized_ = false;
}

async::Task<void> AccessServer::handle(http::HttpExchange &exchange) noexcept {
    AccessServerMetrics::Worker &worker = metrics_.worker(event::EventLoop::current().group_index());
    AccessRequestTelemetry telemetry(exchange, &worker, cat_client_);
    co_await handler_.handle(exchange, telemetry);
}

async::Task<void> AccessServer::handle_metrics(http::HttpExchange &exchange) noexcept {
    auto collected = co_await metrics_.collect(event::EventLoop::current().io_buf_node_pool());
    if (!collected) {
        constexpr std::string_view kBusy = "metrics unavailable\n";
        http::HttpHeaders headers(exchange.pool());
        headers.set_view("Content-Type", "text/plain; charset=utf-8");
        auto sent = co_await exchange.send_header({
                .kind = http::OutgoingHeaderKind::Final,
                .status_code = collected.error() == common::IoErr::Busy ? 503 : 500,
                .headers = &headers,
                .body = http::HttpBodySpec::ContentLength(kBusy.size()),
                .connection_mode = http::ResponseConnectionMode::Auto,
                .end_stream = false,
        });
        if (sent) {
            (void) co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(kBusy.data()), kBusy.size(),
                                               true);
        }
        co_return;
    }

    http::HttpHeaders headers(exchange.pool());
    headers.set_view("Content-Type", "text/plain; version=0.0.4; charset=utf-8");
    const std::size_t size = collected->readable_bytes();
    auto sent = co_await exchange.send_header({
            .kind = http::OutgoingHeaderKind::Final,
            .status_code = 200,
            .headers = &headers,
            .body = http::HttpBodySpec::ContentLength(size),
            .connection_mode = http::ResponseConnectionMode::Auto,
            .end_stream = size == 0,
    });
    if (sent && size != 0) {
        collected->mark_complete();
        (void) co_await exchange.write_all(std::move(*collected));
    }
}

async::DetachedTask AccessServer::detach_cat_worker() noexcept {
    if (cat_client_) {
        (void) co_await cat_client_->detach_current_event_loop();
    }
    cat_detach_tasks_.done();
}

async::Task<void> AccessServer::detach_cat_workers() noexcept {
    if (!cat_client_ || cat_client_->state() != cat::CatClientState::Running) {
        co_return;
    }
    cat_detach_tasks_.add(workers_->size());
    for (std::size_t i = 0; i < workers_->size(); ++i) {
        async::spawn(workers_->at(i), [this]() { return detach_cat_worker(); });
    }
    co_await cat_detach_tasks_.join();
}

} // namespace fiber::access_server
