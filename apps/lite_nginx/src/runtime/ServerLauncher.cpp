#include "ServerLauncher.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <future>
#include <memory>
#include <optional>
#include <string_view>
#include <sys/socket.h>
#include <utility>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/http/HttpBodyPipe.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/http/HttpExchangeIo.h>
#include <fiber/http/HttpHeaders.h>
#include <fiber/http/HttpResponseWriter.h>
#include <fiber/http_script/ConstPackage.h>
#include <fiber/http_script/ScriptExchangeCtx.h>
#include <fiber/log/Log.h>
#include <fiber/net/IpAddress.h>
#include <fiber/net/TcpListener.h>
#include <fiber/net/TlsCredential.h>
#include <fiber/net/TlsServerHandshakeConfig.h>
#include <fiber/script/JsGc.h>
#include <fiber/script/JsValue.h>
#include <fiber/script/ScriptResult.h>

#include "../logging/AccessLogger.h"
#include "../proxy/ProxyHandler.h"
#include "../upstream/ConnectionPool.h"
#include "../upstream/UpstreamRegistry.h"
#include "DnsService.h"
#include "GzipResponseWriter.h"
#include "HttpScriptServices.h"
#include "RequestBodyLimiter.h"

namespace fiber::lite_nginx::runtime {

struct ListenerTlsCredentials {
    struct Entry {
        std::string name;
        std::unique_ptr<fiber::net::TlsCredential> credential;
    };

    std::unique_ptr<fiber::net::TlsCredential> default_credential;
    std::vector<Entry> identities;
};

namespace {

constexpr std::string_view kNotFoundBody = "404 Not Found\n";
constexpr std::string_view kPayloadTooLargeBody = "413 Payload Too Large\n";

DEFINE_LOGGER(LOG_SCRIPT, "lite_nginx.script");

class RequestResponseScope : public fiber::common::NonCopyable, public fiber::common::NonMovable {
public:
    RequestResponseScope(fiber::http::HttpExchange &exchange, const GzipRuntime &runtime,
                         logging::RequestLogContext &log_context) noexcept :
        log_context_(&log_context), writer_(fiber::http::make_http_response_writer(exchange)) {
        if (!runtime.enabled) {
            return;
        }
        gzip_.emplace(exchange, writer_,
                      GzipResponseWriterOptions{
                              .enabled = true,
                              .any_type = runtime.any_type,
                              .types = runtime.types,
                              .min_length = runtime.min_length,
                              .compression_level = runtime.compression_level,
                      });
        writer_ = gzip_->writer();
    }

    ~RequestResponseScope() noexcept {
        if (!gzip_) {
            return;
        }
        const auto &stats = gzip_->stats();
        log_context_->gzip_input_bytes = stats.input_bytes;
        log_context_->gzip_output_bytes = stats.output_bytes;
        log_context_->gzip_used = stats.decision == GzipResponseDecision::Active ||
                                  stats.decision == GzipResponseDecision::Completed || stats.input_bytes != 0 ||
                                  stats.output_bytes != 0;
        switch (stats.decision) {
            case GzipResponseDecision::Undecided:
                log_context_->gzip_status = "not_started";
                break;
            case GzipResponseDecision::Bypassed:
                log_context_->gzip_status = "bypassed";
                break;
            case GzipResponseDecision::Active:
                log_context_->gzip_status = "active";
                break;
            case GzipResponseDecision::Completed:
                log_context_->gzip_status = "compressed";
                break;
            case GzipResponseDecision::Failed:
                log_context_->gzip_status = "failed";
                break;
        }
    }

    [[nodiscard]] fiber::http::HttpResponseWriter &writer() noexcept { return writer_; }

private:
    logging::RequestLogContext *log_context_;
    fiber::http::HttpResponseWriter writer_;
    std::optional<GzipResponseWriter> gzip_;
};

RuntimeError make_error(const config::SourceLocation &location, std::string message) {
    return RuntimeError{
            .message = std::move(message),
            .location = location,
    };
}

fiber::async::Task<void>
send_plain_response(fiber::http::HttpExchange &exchange, fiber::http::HttpResponseWriter response, int status_code,
                    std::string_view body, const ListenerRuntime *listener = nullptr,
                    fiber::http::ResponseConnectionMode connection_mode = fiber::http::ResponseConnectionMode::Auto) {
    fiber::http::HttpHeaders headers(exchange.pool());
    headers.set("Content-Type", "text/plain");
    if (listener != nullptr && listener->http3 && !listener->http3_alt_svc.empty()) {
        headers.set("Alt-Svc", listener->http3_alt_svc);
    }

    auto header_result = co_await response.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Final,
            .status_code = status_code,
            .headers = &headers,
            .body = fiber::http::HttpBodySpec::ContentLength(body.size()),
            .connection_mode = connection_mode,
            .end_stream = body.empty(),
    });
    if (!header_result) {
        co_return;
    }
    if (body.empty()) {
        co_return;
    }

    (void) co_await response.write_all(reinterpret_cast<const std::uint8_t *>(body.data()), body.size(), true);
}

// Runs a compiled script against the request, wiring the HttpExchange via a
// ScriptExchangeCtx attach payload. If the script (or a directive such as http.proxyPass)
// already wrote a response and marked it sent, that response is left as-is. Otherwise a
// response is synthesized from the script's outcome:
//   Value      -> 200 + JSON body (undefined renders as null)
//   Void       -> 204 No Content
//   Exception  -> 500 + JSON {"error": ...}; a heap GcException serializes its name/message,
//                 a tagged exception (no heap payload) renders its kind name.
//   Abort      -> 500 + JSON {"error": "<AbortReason>"}
// path_vars are the route captures for this request (name/value pairs borrowing the matcher
// text and request path buffer).
fiber::async::Task<void>
run_script(fiber::http::HttpExchange &exchange, fiber::http::HttpResponseWriter response, fiber::script::Script &script,
           const fiber::http_script::ConstPackage &const_package,
           const std::vector<std::pair<std::string_view, std::string_view>> &path_vars,
           fiber::http_script::HttpScriptServices *services, const logging::RequestLogContext &log_context,
           fiber::http_script::ScriptRequestBody request_body, const RequestBodyLimiter &body_limiter) {
    fiber::script::GcHeap heap;
    fiber::http_script::ScriptExchangeCtx ctx{exchange, heap, log_context.connection, request_body, response};
    auto constants_ready = ctx.prepare_constants(const_package);
    if (!constants_ready || !ctx.bind_path_constants(const_package, path_vars)) {
        co_await ctx.write_error_json(500, "SCRIPT_CONSTANTS");
        co_return;
    }
    ctx.set_services(services);
    fiber::script::JsValue root = fiber::script::JsValue::make_undefined();
    auto result = co_await script.exec_async(root, &ctx, heap);

    // The dispatcher finalizes request-body limit failures as 413. Do not turn
    // the limiter's read error into a script-level 500.
    if (body_limiter.exceeded()) {
        co_return;
    }

    if (ctx.response_header_sent()) {
        co_return; // script/directive already wrote the full response
    }

    using fiber::script::js_value_exception_kind;
    using fiber::script::js_value_is_heap_ref;
    using fiber::script::js_value_type;
    using fiber::script::JsNodeType;
    using fiber::script::ScriptResultKind;

    switch (result.kind) {
        case ScriptResultKind::Void: {
            co_await ctx.write_empty(204);
            break;
        }
        case ScriptResultKind::Value: {
            co_await ctx.write_json(200, result.value());
            break;
        }
        case ScriptResultKind::Exception: {
            const fiber::script::JsValue &exc = result.exception();
            if (js_value_is_heap_ref(exc) && js_value_type(exc) == JsNodeType::Exception) {
                LOG(LOG_SCRIPT, ERROR) << "request_id=" << log_context.request_id << " script exception kind=heap";
                co_await ctx.write_json(500, exc);
            } else {
                LOG(LOG_SCRIPT, ERROR) << "request_id=" << log_context.request_id << " script exception kind="
                                       << fiber::script::exception_kind_name(js_value_exception_kind(exc));
                co_await ctx.write_error_json(500, fiber::script::exception_kind_name(js_value_exception_kind(exc)));
            }
            break;
        }
        case ScriptResultKind::Abort: {
            LOG(LOG_SCRIPT, ERROR) << "request_id=" << log_context.request_id << " script abort reason="
                                   << fiber::script::abort_reason_name(result.abort().reason);
            co_await ctx.write_error_json(500, fiber::script::abort_reason_name(result.abort().reason));
            break;
        }
    }
    co_return;
}

fiber::common::IoErr configure_identity_by_server_name(void *ctx, fiber::net::TlsServerHandshakeConfig &config,
                                                       const fiber::net::TlsClientHelloView &client_hello) noexcept {
    auto *credentials = static_cast<const ListenerTlsCredentials *>(ctx);
    if (!credentials || !credentials->default_credential) {
        return fiber::common::IoErr::Invalid;
    }
    const fiber::net::TlsCredential *selected = credentials->default_credential.get();
    if (!client_hello.server_name.empty()) {
        auto it = std::lower_bound(
                credentials->identities.begin(), credentials->identities.end(), client_hello.server_name,
                [](const ListenerTlsCredentials::Entry &entry, std::string_view name) { return entry.name < name; });
        if (it != credentials->identities.end() && it->name == client_hello.server_name) {
            selected = it->credential.get();
        }
    }
    return config.add_credential(*selected);
}

std::expected<std::unique_ptr<ListenerTlsCredentials>, RuntimeError>
make_tls_credentials(const ListenerRuntime &listener) {
    auto credentials = std::make_unique<ListenerTlsCredentials>();
    fiber::net::TlsCredentialOptions default_options{};
    default_options.certificate_chain = fiber::net::TlsPemSource::from_file(listener.default_certificate);
    default_options.private_key = fiber::net::TlsPemSource::from_file(listener.default_certificate_key);
    auto default_credential = fiber::net::TlsCredential::create(default_options);
    if (!default_credential) {
        return std::unexpected(make_error(listener.location, "failed to load default TLS identity"));
    }
    credentials->default_credential = std::move(*default_credential);
    credentials->identities.reserve(listener.tls_identities.size());
    for (const auto &identity: listener.tls_identities) {
        fiber::net::TlsCredentialOptions options{};
        options.certificate_chain = fiber::net::TlsPemSource::from_file(identity.certificate);
        options.private_key = fiber::net::TlsPemSource::from_file(identity.certificate_key);
        auto credential = fiber::net::TlsCredential::create(options);
        if (!credential) {
            return std::unexpected(make_error(listener.location, "failed to load named TLS identity"));
        }
        credentials->identities.push_back({.name = identity.server_name, .credential = std::move(*credential)});
    }
    std::sort(credentials->identities.begin(), credentials->identities.end(),
              [](const ListenerTlsCredentials::Entry &left, const ListenerTlsCredentials::Entry &right) {
                  return left.name < right.name;
              });
    return credentials;
}

std::expected<fiber::net::SocketAddress, RuntimeError> make_socket_address(const ListenerRuntime &listener) {
    if (!listener.has_host) {
        return fiber::net::SocketAddress::any_v4(listener.port);
    }

    fiber::net::IpAddress ip;
    if (!fiber::net::IpAddress::parse(listener.host, ip)) {
        return std::unexpected(make_error(listener.location,
                                          "listen host must be an IP literal in lite-nginx runtime: " + listener.host));
    }
    return fiber::net::SocketAddress(ip, listener.port);
}

fiber::common::IoResult<std::uint16_t> resolve_port(int fd) {
    sockaddr_storage bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    fiber::net::SocketAddress local;
    if (!fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<sockaddr *>(&bound), len, local)) {
        return std::unexpected(fiber::common::IoErr::NotSupported);
    }
    return local.port();
}

fiber::http::HttpServerOptions make_server_options(const ListenerRuntime &listener,
                                                   const ListenerTlsCredentials *tls_credentials) {
    fiber::http::HttpServerOptions options;
    options.drain_unread_body = true;
    options.enable_extended_connect = true;
    options.http3.enabled = listener.http3;
    if (!listener.tls) {
        return options;
    }

    options.tls.alpn = {"h2", "http/1.1"};
    options.tls.configure_callback = &configure_identity_by_server_name;
    options.tls.configure_ctx = const_cast<ListenerTlsCredentials *>(tls_credentials);
    return options;
}

std::string_view strip_host_port(std::string_view host) noexcept {
    if (host.empty()) {
        return {};
    }
    if (host.front() == '[') {
        const std::size_t end = host.find(']');
        if (end == std::string_view::npos) {
            return host;
        }
        return host.substr(1, end - 1);
    }

    const std::size_t colon = host.find(':');
    if (colon == std::string_view::npos) {
        return host;
    }
    return host.substr(0, colon);
}

std::uint32_t find_server_index(const ListenerRuntime &listener, std::string_view host_name) noexcept {
    if (host_name.empty() || listener.server_names.empty()) {
        return listener.default_server_index;
    }

    auto it = std::lower_bound(listener.server_names.begin(), listener.server_names.end(), host_name,
                               [](const ServerNameRuntime &entry, std::string_view key) { return entry.name < key; });
    if (it == listener.server_names.end() || it->name != host_name) {
        return listener.default_server_index;
    }
    return it->server_index;
}

struct LocationMatchContext {
    static constexpr std::uint32_t kInvalidIndex = std::numeric_limits<std::uint32_t>::max();

    bool matched(std::uint32_t, const std::uint32_t &handler) {
        location_index = handler;
        return true;
    }

    // Collect path variables captured by the matcher (name/value pairs). The matcher uses
    // push/pop with backtracking on mismatch; on a successful match the captures along the
    // matched path remain (the success branches return without popping), so path_vars holds
    // exactly the matched route's variables.
    void add_path_var(std::string_view name, std::string_view value) { path_vars.emplace_back(name, value); }
    void pop_path_var() {
        if (!path_vars.empty()) {
            path_vars.pop_back();
        }
    }

    std::uint32_t location_index = kInvalidIndex;
    std::vector<std::pair<std::string_view, std::string_view>> path_vars;
};

class RequestDispatcher {
public:
    RequestDispatcher(std::shared_ptr<const RuntimeConfig> runtime,
                      std::shared_ptr<upstream::UpstreamRegistry> upstreams, upstream::ConnectionPool &connection_pool,
                      DnsService &dns, fiber::http_script::HttpScriptServices *script_services,
                      logging::AccessLogger access_logger) noexcept :
        runtime_(std::move(runtime)), upstreams_(std::move(upstreams)), proxy_(*upstreams_, connection_pool, dns),
        script_services_(script_services), access_logger_(std::move(access_logger)) {}

    fiber::async::Task<void> handle(std::uint32_t listener_index, fiber::http::HttpExchange &exchange) const {
        logging::RequestLogContext log_context{
                .started_at = fiber::event::EventLoop::current().now(),
                .request_id = logging::next_request_id(),
                .access_log = runtime_ ? runtime_->access_log : kDisabledAccessLog,
        };
        co_await handle_inner(listener_index, exchange, log_context);
        access_logger_.write(exchange, log_context, fiber::event::EventLoop::current().now());
    }

private:
    fiber::async::Task<void> handle_inner(std::uint32_t listener_index, fiber::http::HttpExchange &exchange,
                                          logging::RequestLogContext &log_context) const {
        if (!runtime_ || listener_index >= runtime_->listeners.size()) {
            auto response = fiber::http::make_http_response_writer(exchange);
            co_await send_plain_response(exchange, response, 404, kNotFoundBody);
            co_return;
        }

        const ListenerRuntime &listener = runtime_->listeners[listener_index];
        log_context.connection = {
                .scheme = listener.tls ? std::string_view("https") : std::string_view("http"),
                .tls = listener.tls,
        };
        std::string_view host_name;
        if (const auto *host = exchange.host_header(); host != nullptr) {
            host_name = strip_host_port(host->value_view());
        }

        const std::uint32_t server_index = find_server_index(listener, host_name);
        if (server_index >= runtime_->servers.size()) {
            RequestResponseScope response_scope(exchange, runtime_->gzip, log_context);
            co_await send_plain_response(exchange, response_scope.writer(), 404, kNotFoundBody, &listener);
            co_return;
        }

        const ServerRuntime &server = runtime_->servers[server_index];
        log_context.access_log = server.access_log;
        if (!server.server_names.empty()) {
            log_context.server_name = server.server_names.front();
        }
        LocationMatchContext match_context;
        std::string_view path = exchange.uri().path.empty() ? std::string_view("/") : exchange.uri().path;
        if (!server.location_matcher.match_path(path, match_context) ||
            match_context.location_index >= server.locations.size()) {
            RequestResponseScope response_scope(exchange, server.gzip, log_context);
            co_await send_plain_response(exchange, response_scope.writer(), 404, kNotFoundBody, &listener);
            co_return;
        }

        const LocationRuntime &location = server.locations[match_context.location_index];
        log_context.access_log = location.access_log;
        log_context.location_pattern = location.pattern;
        log_context.path_vars = std::move(match_context.path_vars);
        RequestResponseScope response_scope(exchange, location.gzip, log_context);
        RequestBodyLimiter body_limiter(exchange, location.client_max_body_size);
        if (body_limiter.exceeded()) {
            co_await send_plain_response(exchange, response_scope.writer(), 413, kPayloadTooLargeBody, &listener,
                                         fiber::http::ResponseConnectionMode::Close);
            co_return;
        }
        const fiber::http_script::ScriptRequestBody request_body =
                fiber::http_script::make_script_request_body(body_limiter);
        if (location.script) {
            assert(location.const_package != nullptr);
            co_await run_script(exchange, response_scope.writer(), *location.script, *location.const_package,
                                log_context.path_vars, script_services_, log_context, request_body, body_limiter);
        } else {
            co_await proxy_.handle(exchange, response_scope.writer(), request_body.pipe_reader(), listener, location,
                                   log_context.path_vars, script_services_, log_context);
        }
        if (body_limiter.exceeded() && !exchange.response_stats().header_sent) {
            co_await send_plain_response(exchange, response_scope.writer(), 413, kPayloadTooLargeBody, &listener,
                                         fiber::http::ResponseConnectionMode::Close);
        }
    }

    std::shared_ptr<const RuntimeConfig> runtime_;
    std::shared_ptr<upstream::UpstreamRegistry> upstreams_;
    proxy::ProxyHandler proxy_;
    fiber::http_script::HttpScriptServices *script_services_;
    logging::AccessLogger access_logger_;
};

} // namespace

ServerLauncher::ServerLauncher(fiber::event::EventLoop &accept_loop) : accept_loop_(accept_loop) {}

ServerLauncher::~ServerLauncher() { close(); }

std::expected<void, RuntimeError> ServerLauncher::start(const RuntimeConfig &runtime,
                                                        const fiber::dns::SystemResolverConfig &resolver_config) {
    if (started_) {
        return std::unexpected(make_error({}, "lite-nginx runtime already started"));
    }

    runtime_ = std::make_shared<RuntimeConfig>(runtime);
    logging::AccessLogger access_logger;
    auto access_log_result = access_logger.bind(*runtime_);
    if (!access_log_result) {
        close();
        return std::unexpected(access_log_result.error());
    }
    worker_group_ =
            std::make_unique<fiber::event::EventLoopGroup>(std::max<std::size_t>(runtime_->worker_processes, 1));
    worker_group_->start();

    upstreams_ = std::make_shared<upstream::UpstreamRegistry>(*worker_group_, *runtime_);
    if (!upstreams_->init()) {
        close();
        return std::unexpected(make_error({}, "failed to initialize upstream registry"));
    }

    connection_pool_ = std::make_unique<upstream::ConnectionPool>(*worker_group_, runtime_->connection_pool);
    if (!connection_pool_->init()) {
        close();
        return std::unexpected(make_error({}, "failed to initialize connection pool"));
    }

    dns_ = std::make_unique<DnsService>();
    if (!dns_->init(*worker_group_, resolver_config)) {
        close();
        return std::unexpected(make_error({}, "failed to initialize DNS service"));
    }
    script_services_ = std::make_unique<HttpScriptServicesImpl>(*upstreams_, *connection_pool_, *dns_);

    auto dispatcher = std::make_shared<RequestDispatcher>(runtime_, upstreams_, *connection_pool_, *dns_,
                                                          script_services_.get(), std::move(access_logger));

    servers_.reserve(runtime_->listeners.size());
    tls_credentials_.reserve(runtime_->listeners.size());
    bound_listeners_.reserve(runtime_->listeners.size());

    for (std::uint32_t listener_index = 0; listener_index < runtime_->listeners.size(); ++listener_index) {
        const auto &listener = runtime_->listeners[listener_index];
        auto addr_result = make_socket_address(listener);
        if (!addr_result) {
            close();
            return std::unexpected(addr_result.error());
        }

        std::unique_ptr<ListenerTlsCredentials> tls_credentials;
        if (listener.tls) {
            auto created_credentials = make_tls_credentials(listener);
            if (!created_credentials) {
                close();
                return std::unexpected(created_credentials.error());
            }
            tls_credentials = std::move(*created_credentials);
        }
        auto options = make_server_options(listener, tls_credentials.get());
        auto server = std::make_unique<fiber::http::HttpServer>(
                accept_loop_,
                [dispatcher, listener_index](fiber::http::HttpExchange &exchange) {
                    return dispatcher->handle(listener_index, exchange);
                },
                std::move(options), worker_group_.get());

        fiber::net::ListenOptions listen_options{};
        auto bind_result = server->bind(*addr_result, listen_options);
        if (!bind_result) {
            close();
            return std::unexpected(make_error(listener.location,
                                              "bind failed for listen " + addr_result->to_string() + ": " +
                                                      std::string(fiber::common::io_err_name(bind_result.error()))));
        }

        auto bound_port_result = resolve_port(server->fd());
        if (!bound_port_result) {
            close();
            return std::unexpected(make_error(listener.location,
                                              "failed to resolve bound port for listen " + addr_result->to_string()));
        }

        fiber::net::SocketAddress bound_address(addr_result->ip(), *bound_port_result);
        bound_listeners_.push_back({
                .address = bound_address,
                .tls = listener.tls,
                .http3 = listener.http3,
        });

        auto *server_ptr = server.get();
        fiber::async::spawn(accept_loop_, [server_ptr]() { return server_ptr->serve(); });
        servers_.push_back(std::move(server));
        tls_credentials_.push_back(std::move(tls_credentials));
    }

    started_ = true;
    return {};
}

void ServerLauncher::close() {
    if (accept_loop_.in_loop()) {
        // A caller on the owner loop cannot synchronously wait without
        // deadlocking that loop. Leave the facades and dependent loops alive;
        // the next close (normally the launcher destructor after the loop
        // stops) completes the barrier and performs the destruction.
        for (auto &server: servers_) {
            if (server) {
                server->request_close();
            }
        }
        return;
    }

    if (!servers_.empty()) {
        // HttpServer::close() only requests shutdown. Keep the server facades
        // and worker loops alive until every owner-loop cleanup has completed;
        // otherwise the posted close callbacks can never run after the
        // accept loop has stopped, and loop-affine resources outlive their
        // owning server.
        std::promise<void> shutdown_promise;
        auto shutdown_future = shutdown_promise.get_future();
        fiber::async::spawn(accept_loop_, [this, &shutdown_promise]() -> fiber::async::DetachedTask {
            for (auto &server: servers_) {
                if (server) {
                    co_await server->shutdown_and_wait();
                }
            }
            shutdown_promise.set_value();
            accept_loop_.stop();
            co_return;
        });
        if (!accept_loop_.running()) {
            accept_loop_.run();
        }
        shutdown_future.get();
    }

    servers_.clear();
    tls_credentials_.clear();
    bound_listeners_.clear();
    started_ = false;

    if (upstreams_) {
        upstreams_->shutdown();
    }

    script_services_.reset();
    if (dns_) {
        dns_->shutdown();
        dns_.reset();
    }

    // Connection pool shutdown dispatches to worker loops (must run before they stop/join).
    if (connection_pool_) {
        connection_pool_->shutdown();
        connection_pool_.reset();
    }

    if (worker_group_) {
        worker_group_->stop();
        worker_group_->join();
    }

    upstreams_.reset();
    runtime_.reset();
    worker_group_.reset();
}

} // namespace fiber::lite_nginx::runtime
