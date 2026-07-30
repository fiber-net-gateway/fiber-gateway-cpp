#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <utility>
#include <vector>

#include "async/Spawn.h"
#include "async/Task.h"
#include "common/IoError.h"
#include "common/mem/BufPool.h"
#include "common/mem/IoBuf.h"
#include "common/mem/IoBufChain.h"
#include "event/EventLoopGroup.h"
#include "http/ClientHttp1Exchange.h"
#include "http/ClientHttp1Types.h"
#include "http/Http1ClientConnection.h"
#include "http/HttpBodySpec.h"
#include "http/HttpCommon.h"
#include "http/HttpExchange.h"
#include "http/HttpHeaders.h"
#include "http/HttpServer.h"
#include "net/IpAddress.h"
#include "net/SocketAddress.h"

#include "common/util/RoutePathMatcher.h"
#include "http_script/HttpScriptLib.h"
#include "http_script/RouteScriptExtension.h"
#include "http_script/ScriptExchangeCtx.h"
#include "script/JsGc.h"
#include "script/JsValue.h"
#include "script/Script.h"
#include "script/ScriptCompiler.h"
#include "script/ScriptResult.h"
#include "script/std/StdLibrary.h"

namespace {

using fiber::async::DetachedTask;
using fiber::async::Task;

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

std::string chain_to_string(fiber::mem::IoBufChain chain) {
    std::string out;
    while (auto *front = chain.front()) {
        if (front->readable() == 0) {
            chain.drop_empty_front();
            continue;
        }
        out.append(reinterpret_cast<const char *>(front->readable_data()), front->readable());
        chain.consume_and_compact(front->readable());
    }
    return out;
}

Task<fiber::common::IoResult<std::string>> read_body_to_string(fiber::http::ClientHttp1Exchange &exchange) {
    std::string out;
    for (;;) {
        auto chunk_result = co_await exchange.read_body(64);
        if (!chunk_result) {
            co_return std::unexpected(chunk_result.error());
        }
        const bool last = chunk_result->complete();
        out.append(chain_to_string(std::move(*chunk_result)));
        if (last) {
            break;
        }
    }
    co_return out;
}

DetachedTask start_http_server(fiber::event::EventLoop *loop, fiber::http::HttpHandler handler,
                               std::promise<std::uint16_t> *port_promise,
                               std::promise<fiber::http::HttpServer *> *server_promise) {
    auto *server = new fiber::http::HttpServer(*loop, std::move(handler), fiber::http::HttpServerOptions{}, nullptr);
    fiber::net::ListenOptions listen_options{};
    fiber::net::SocketAddress addr(fiber::net::IpAddress::loopback_v4(), 0);
    auto bind_result = server->bind(addr, listen_options);
    if (!bind_result) {
        delete server;
        port_promise->set_value(0);
        server_promise->set_value(nullptr);
        co_return;
    }
    auto port_result = resolve_port(server->fd());
    port_promise->set_value(port_result ? *port_result : 0);
    server_promise->set_value(server);
    fiber::async::spawn(*loop, [server]() { return server->serve(); });
    co_return;
}

DetachedTask close_server_on_loop(fiber::http::HttpServer *server, std::promise<void> *done_promise) {
    if (server) {
        server->close();
    }
    done_promise->set_value();
    co_return;
}

// Compiles a script source against a fresh StdLibrary with the HTTP functions registered.
std::shared_ptr<fiber::script::Script> compile_http_script(std::string_view source) {
    auto lib = std::make_shared<fiber::script::std_lib::StdLibrary>();
    fiber::http_script::register_http_functions_to_lib(*lib);
    auto compiled = fiber::script::compile_script(*lib, source);
    EXPECT_TRUE(compiled.has_value()) << (compiled ? "" : compiled.error().message);
    if (!compiled) {
        return nullptr;
    }
    // The compiled Script bakes in function pointers; lib is only needed at compile time,
    // so keep it alive alongside the script for safety.
    return std::make_shared<fiber::script::Script>(std::move(*compiled));
}

fiber::http::HttpHandler make_script_handler(std::shared_ptr<fiber::script::Script> script) {
    return [script](fiber::http::HttpExchange &exchange) -> Task<void> {
        fiber::script::GcHeap heap;
        fiber::http_script::ScriptExchangeCtx ctx{exchange, heap};
        fiber::script::JsValue root = fiber::script::JsValue::make_undefined();
        auto result = co_await script->exec_async(root, &ctx, heap);
        (void) result;
        co_return;
    };
}

struct ClientResult {
    fiber::common::IoErr err = fiber::common::IoErr::None;
    int status_code = 0;
    std::string body;
    std::string content_type;
    std::vector<std::pair<std::string, std::string>> response_headers;
};

struct ClientRequest {
    fiber::http::HttpMethod method = fiber::http::HttpMethod::Get;
    std::string target;
    std::vector<std::pair<std::string, std::string>> headers;
    std::optional<std::string> body;
};

DetachedTask run_http1_client(fiber::event::EventLoop *loop, std::uint16_t port, ClientRequest req,
                              std::promise<ClientResult> *promise) {
    ClientResult result;
    fiber::http::Http1ClientConnectionOptions options;
    options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);
    fiber::http::Http1ClientConnection connection(*loop, options);
    auto connect_result = co_await connection.connect(std::chrono::seconds(5));
    if (!connect_result) {
        result.err = connect_result.error();
        promise->set_value(std::move(result));
        co_return;
    }

    fiber::mem::BufPool pool;
    fiber::http::HttpHeaders headers(pool);
    headers.set("host", "localhost");
    for (const auto &[name, value]: req.headers) {
        headers.set(name, value);
    }

    fiber::http::ClientHttp1Exchange exchange(connection, pool);
    fiber::http::Http1RequestHead head;
    head.method = req.method;
    head.target = req.target;
    head.headers = &headers;
    bool end_stream = true;
    if (req.body) {
        head.body = fiber::http::HttpBodySpec::ContentLength(req.body->size());
        end_stream = false;
    }

    auto send_result = co_await exchange.send_header(head, end_stream);
    if (!send_result) {
        result.err = send_result.error();
        promise->set_value(std::move(result));
        co_return;
    }
    if (req.body) {
        auto write_result = co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(req.body->data()),
                                                        req.body->size(), true);
        if (!write_result) {
            result.err = write_result.error();
            promise->set_value(std::move(result));
            co_return;
        }
    }

    auto header_result = co_await exchange.read_header();
    if (!header_result) {
        result.err = header_result.error();
        promise->set_value(std::move(result));
        co_return;
    }
    result.status_code = (*header_result)->status_code;
    result.content_type = std::string((*header_result)->headers.get("content-type"));
    for (const fiber::http::HttpHeaders::HeaderField &field: (*header_result)->headers) {
        result.response_headers.emplace_back(std::string(field.name_view()), std::string(field.value_view()));
    }

    auto body_result = co_await read_body_to_string(exchange);
    if (!body_result) {
        result.err = body_result.error();
        promise->set_value(std::move(result));
        co_return;
    }
    result.body = std::move(*body_result);
    promise->set_value(std::move(result));
}

struct ServerFixture {
    fiber::event::EventLoopGroup group{1};
    fiber::http::HttpServer *server = nullptr;
    std::uint16_t port = 0;

    void start(fiber::http::HttpHandler handler) {
        group.start();
        std::promise<std::uint16_t> port_promise;
        std::promise<fiber::http::HttpServer *> server_promise;
        auto port_future = port_promise.get_future();
        auto server_future = server_promise.get_future();
        fiber::async::spawn(group.at(0), [&]() {
            return start_http_server(&group.at(0), std::move(handler), &port_promise, &server_promise);
        });
        server = server_future.get();
        port = port_future.get();
        ASSERT_NE(server, nullptr);
        ASSERT_NE(port, 0);
    }

    ClientResult request(ClientRequest req) {
        std::promise<ClientResult> client_promise;
        auto client_future = client_promise.get_future();
        fiber::async::spawn(group.at(0),
                            [&]() { return run_http1_client(&group.at(0), port, std::move(req), &client_promise); });
        return client_future.get();
    }

    void stop() {
        std::promise<void> close_promise;
        auto close_future = close_promise.get_future();
        fiber::async::spawn(group.at(0), [&]() { return close_server_on_loop(server, &close_promise); });
        close_future.get();
        group.stop();
        group.join();
        delete server;
    }
};

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

// ---- Route-variable ($path/$query/$header/$cookie/$req) test helpers ----

// Compiles a script against a per-location route extension seeded with path_var_names and a
// fresh StdLibrary with the HTTP functions. The extension outlives the compiled script because
// its state is referenced through the compiled host-call userdata.
struct CompiledRouteScript {
    std::shared_ptr<fiber::script::Script> script; // nullptr on compile failure
    std::shared_ptr<fiber::script::std_lib::StdLibrary> shared_lib;
    std::shared_ptr<fiber::http_script::RouteScriptExtension> route_extension;
    bool ok = false;
    std::string error;
};

CompiledRouteScript compile_route_script(std::string_view source, const std::vector<std::string> &path_var_names) {
    CompiledRouteScript out;
    out.shared_lib = std::make_shared<fiber::script::std_lib::StdLibrary>();
    fiber::http_script::register_http_functions_to_lib(*out.shared_lib);
    out.route_extension = std::make_shared<fiber::http_script::RouteScriptExtension>();
    out.shared_lib->add_ext_ops(out.route_extension.get(), fiber::http_script::RouteScriptExtension::ops());
    out.route_extension->set_compile_path_vars(path_var_names);
    out.route_extension->set_http_directives_enabled(true);
    auto compiled = fiber::script::compile_script(*out.shared_lib, source);
    if (!compiled) {
        out.error = std::string(compiled.error().message);
        return out; // script stays nullptr, ok stays false
    }
    out.script = std::make_shared<fiber::script::Script>(std::move(*compiled));
    out.ok = true;
    return out;
}

// Route matcher context that collects path vars (mirrors lite_nginx LocationMatchContext).
struct RouteMatchCollector {
    bool matched(std::uint32_t, const std::uint32_t &) { return true; }
    void add_path_var(std::string_view name, std::string_view value) { vars.emplace_back(name, value); }
    void pop_path_var() {
        if (!vars.empty()) {
            vars.pop_back();
        }
    }
    std::vector<std::pair<std::string_view, std::string_view>> vars;
};

struct TestRouteVarDefiner {
    void add_path_var_definer(int &, std::string_view, std::uint32_t) {}
    std::uint32_t on_route_mount(std::uint32_t, std::string_view, int &) { return 0; }
};

// Builds a handler that matches the request path against `pattern` (collecting path vars)
// and feeds the captures to the script's ScriptExchangeCtx.
fiber::http::HttpHandler make_route_script_handler(CompiledRouteScript compiled, std::string_view pattern) {
    auto matcher = std::make_shared<fiber::util::RoutePathMatcher<std::uint32_t>>();
    TestRouteVarDefiner definer;
    fiber::util::RoutePathMatcher<std::uint32_t>::Builder<int, TestRouteVarDefiner> builder(definer);
    builder.add_route(pattern, 0);
    *matcher = builder.build();
    auto script = compiled.script;
    auto route_extension = compiled.route_extension;
    auto shared_lib = compiled.shared_lib;
    return [script, matcher, route_extension, shared_lib](fiber::http::HttpExchange &exchange) -> Task<void> {
        fiber::script::GcHeap heap;
        fiber::http_script::ScriptExchangeCtx ctx{
                exchange, heap, fiber::http_script::ScriptConnectionInfo{.scheme = "http", .tls = false}};
        RouteMatchCollector mc;
        std::string_view path = exchange.uri().path;
        (void) matcher->match_path(path, mc);
        ctx.set_path_vars(mc.vars);
        fiber::script::JsValue root = fiber::script::JsValue::make_undefined();
        auto result = co_await script->exec_async(root, &ctx, heap);
        (void) result;
        if (!ctx.response_header_sent()) {
            fiber::http::HttpHeaders headers(exchange.pool());
            headers.set("Content-Type", "text/plain");
            co_await exchange.send_header({
                    .kind = fiber::http::OutgoingHeaderKind::Final,
                    .status_code = 500,
                    .headers = &headers,
                    .body = fiber::http::HttpBodySpec::ContentLength(0),
                    .connection_mode = fiber::http::ResponseConnectionMode::Auto,
                    .end_stream = true,
            });
        }
        co_return;
    };
}

} // namespace

TEST(HttpScriptFuncsTest, RequestViewsAndSendJson) {
    ServerFixture s;
    auto script = compile_http_script(
            "let h = req.getHeader();"
            "let q = req.getQuery();"
            "let c = req.getCookie();"
            "resp.sendJson(200, {"
            "method: req.getMethod(), path: req.getPath(), uri: req.getUri(), qstr: req.getQueryStr(),"
            "xtest: req.getHeader(\"x-test\"), qa: q.a, qb: q.b, session: c.session, theme: c.theme, host: h.host"
            "});");
    ASSERT_NE(script, nullptr);
    s.start(make_script_handler(script));

    ClientRequest req;
    req.method = fiber::http::HttpMethod::Get;
    req.target = "/script?a=1&b=2";
    req.headers = {{"x-test", "hello"}, {"cookie", "session=abc; theme=dark"}};
    ClientResult result = s.request(std::move(req));

    EXPECT_EQ(result.err, fiber::common::IoErr::None);
    EXPECT_EQ(result.status_code, 200);
    EXPECT_EQ(result.content_type, "application/json");
    EXPECT_TRUE(contains(result.body, "\"method\":\"GET\"")) << result.body;
    EXPECT_TRUE(contains(result.body, "\"path\":\"/script\"")) << result.body;
    EXPECT_TRUE(contains(result.body, "\"uri\":\"/script?a=1&b=2\"")) << result.body;
    EXPECT_TRUE(contains(result.body, "\"qstr\":\"a=1&b=2\"")) << result.body;
    EXPECT_TRUE(contains(result.body, "\"xtest\":\"hello\"")) << result.body;
    EXPECT_TRUE(contains(result.body, "\"qa\":\"1\"")) << result.body;
    EXPECT_TRUE(contains(result.body, "\"qb\":\"2\"")) << result.body;
    EXPECT_TRUE(contains(result.body, "\"session\":\"abc\"")) << result.body;
    EXPECT_TRUE(contains(result.body, "\"theme\":\"dark\"")) << result.body;
    EXPECT_TRUE(contains(result.body, "\"host\":\"localhost\"")) << result.body;

    s.stop();
}

TEST(HttpScriptFuncsTest, ReadJsonBodyAndEcho) {
    ServerFixture s;
    auto script = compile_http_script("let b = req.readJson();"
                                      "resp.sendJson(200, {name: b.name, n: b.n});");
    ASSERT_NE(script, nullptr);
    s.start(make_script_handler(script));

    ClientRequest req;
    req.method = fiber::http::HttpMethod::Post;
    req.target = "/echo";
    req.headers = {{"content-type", "application/json"}};
    req.body = std::string("{\"name\":\"fiber\",\"n\":42}");
    ClientResult result = s.request(std::move(req));

    EXPECT_EQ(result.err, fiber::common::IoErr::None) << "err=" << static_cast<int>(result.err);
    EXPECT_EQ(result.status_code, 200) << "body=" << result.body;
    EXPECT_EQ(result.content_type, "application/json");
    EXPECT_TRUE(contains(result.body, "\"name\":\"fiber\"")) << result.body;
    EXPECT_TRUE(contains(result.body, "\"n\":42")) << result.body;

    s.stop();
}

TEST(HttpScriptFuncsTest, SetHeadersAndAddCookieAndSend) {
    ServerFixture s;
    auto script = compile_http_script(
            "resp.setHeader(\"x-from\", \"script\");"
            "resp.addHeader(\"x-multi\", \"a\");"
            "resp.addHeader(\"x-multi\", \"b\");"
            "resp.addCookie({name: \"sid\", value: \"v\", path: \"/\", httpOnly: true, sameSite: \"Lax\"});"
            "resp.send(200, \"plain text body\");");
    ASSERT_NE(script, nullptr);
    s.start(make_script_handler(script));

    ClientRequest req;
    req.method = fiber::http::HttpMethod::Get;
    req.target = "/headers";
    ClientResult result = s.request(std::move(req));

    EXPECT_EQ(result.err, fiber::common::IoErr::None);
    EXPECT_EQ(result.status_code, 200);
    EXPECT_EQ(result.content_type, "text/plain;charset=utf-8");
    EXPECT_EQ(result.body, "plain text body");

    // resp.setHeader -> x-from: script (single, last wins)
    // resp.addHeader -> x-multi: a and x-multi: b (both retained)
    // resp.addCookie -> Set-Cookie with the encoded cookie
    std::vector<std::string> x_multi;
    std::string set_cookie;
    std::string x_from;
    for (const auto &[name, value]: result.response_headers) {
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower == "x-multi") {
            x_multi.push_back(value);
        } else if (lower == "set-cookie") {
            set_cookie = value;
        } else if (lower == "x-from") {
            x_from = value;
        }
    }
    EXPECT_EQ(x_from, "script");
    ASSERT_EQ(x_multi.size(), 2u);
    EXPECT_EQ(x_multi[0], "a");
    EXPECT_EQ(x_multi[1], "b");
    EXPECT_TRUE(contains(set_cookie, "sid=v")) << set_cookie;
    EXPECT_TRUE(contains(set_cookie, "Path=/")) << set_cookie;
    EXPECT_TRUE(contains(set_cookie, "HttpOnly")) << set_cookie;
    EXPECT_TRUE(contains(set_cookie, "SameSite=Lax")) << set_cookie;

    s.stop();
}

// ---- Route-variable ($path/$query/$header/$cookie/$req/$conn) tests ----

TEST(RouteVarTest, PathVarCompileTimeExistence) {
    // $path.id compiles when "id" is a declared path var; $path.missing does not.
    auto ok = compile_route_script("resp.sendJson(200, $path.id);", {"id"});
    EXPECT_TRUE(ok.ok) << ok.error;

    auto missing = compile_route_script("resp.sendJson(200, $path.missing);", {"id"});
    EXPECT_FALSE(missing.ok);
    EXPECT_NE(missing.error.find("constant not found"), std::string::npos) << missing.error;

    // An exact-match location (no path vars) rejects any $path reference at compile time.
    auto none = compile_route_script("resp.sendJson(200, $path.id);", {});
    EXPECT_FALSE(none.ok);
}

TEST(RouteVarTest, SharedExtensionUsesCurrentRouteInfoBeforeCallableCache) {
    fiber::script::std_lib::StdLibrary library;
    fiber::http_script::register_http_functions_to_lib(library);
    fiber::http_script::RouteScriptExtension route_extension;
    library.add_ext_ops(&route_extension, fiber::http_script::RouteScriptExtension::ops());

    route_extension.set_compile_path_vars({"id"});
    auto first = fiber::script::compile_script(library, "resp.sendJson(200, $path.id);");
    ASSERT_TRUE(first.has_value()) << first.error().message;

    route_extension.set_compile_path_vars({"slug"});
    auto stale = fiber::script::compile_script(library, "resp.sendJson(200, $path.id);");
    EXPECT_FALSE(stale.has_value());
    auto current = fiber::script::compile_script(library, "resp.sendJson(200, $path.slug);");
    EXPECT_TRUE(current.has_value()) << current.error().message;
}

TEST(RouteVarTest, ReqFieldCompileTimeExistence) {
    for (std::string_view field: {"uri", "method", "path", "query"}) {
        std::string src = "resp.sendJson(200, $req.";
        src += field;
        src += ");";
        auto compiled = compile_route_script(src, {});
        EXPECT_TRUE(compiled.ok) << field << ": " << compiled.error;
    }
    auto bad = compile_route_script("resp.sendJson(200, $req.bogus);", {});
    EXPECT_FALSE(bad.ok);
    EXPECT_NE(bad.error.find("constant not found"), std::string::npos) << bad.error;
}

TEST(RouteVarTest, ConnectionFieldCompileTimeExistence) {
    for (std::string_view field: {"remote_addr", "remote_port", "http_version", "scheme", "tls"}) {
        std::string src = "resp.sendJson(200, $conn.";
        src += field;
        src += ");";
        auto compiled = compile_route_script(src, {});
        EXPECT_TRUE(compiled.ok) << field << ": " << compiled.error;
    }
    auto bad = compile_route_script("resp.sendJson(200, $conn.bogus);", {});
    EXPECT_FALSE(bad.ok);
    EXPECT_NE(bad.error.find("constant not found"), std::string::npos) << bad.error;
}

TEST(RouteVarTest, ConnectionFieldsDescribeDownstreamConnection) {
    ServerFixture s;
    auto compiled = compile_route_script("resp.sendJson(200, {addr: $conn.remote_addr, port: $conn.remote_port, "
                                         "version: $conn.http_version, scheme: $conn.scheme, tls: $conn.tls});",
                                         {});
    ASSERT_TRUE(compiled.ok) << compiled.error;
    s.start(make_route_script_handler(std::move(compiled), "/*"));

    ClientRequest req;
    req.method = fiber::http::HttpMethod::Get;
    req.target = "/connection";
    ClientResult result = s.request(std::move(req));

    EXPECT_EQ(result.status_code, 200) << result.body;
    EXPECT_TRUE(contains(result.body, "\"addr\":\"127.0.0.1\"")) << result.body;
    EXPECT_TRUE(contains(result.body, "\"port\":")) << result.body;
    EXPECT_TRUE(contains(result.body, "\"version\":\"HTTP/1.1\"")) << result.body;
    EXPECT_TRUE(contains(result.body, "\"scheme\":\"http\"")) << result.body;
    EXPECT_TRUE(contains(result.body, "\"tls\":false")) << result.body;
    s.stop();
}

TEST(RouteVarTest, DynamicNamespacesAlwaysResolve) {
    // $query/$header/$cookie always compile (slot exists; value resolved at request time).
    auto compiled = compile_route_script(
            "resp.sendJson(200, {q: $query.a, h: $header.x_forwarded_for, c: $cookie.session});", {});
    EXPECT_TRUE(compiled.ok) << compiled.error;
}

TEST(RouteVarTest, PathVarResolvesCapturedValue) {
    ServerFixture s;
    auto compiled = compile_route_script(
            "resp.sendJson(200, {id: $path.id, uri: $req.uri, method: $req.method, path: $req.path, q: $req.query});",
            {"id"});
    ASSERT_TRUE(compiled.ok) << compiled.error;
    s.start(make_route_script_handler(std::move(compiled), "/users/:id"));

    ClientRequest req;
    req.method = fiber::http::HttpMethod::Get;
    req.target = "/users/42?x=1";
    ClientResult result = s.request(std::move(req));

    EXPECT_EQ(result.status_code, 200) << result.body;
    EXPECT_TRUE(contains(result.body, "\"id\":\"42\"")) << result.body;
    EXPECT_TRUE(contains(result.body, "\"uri\":\"/users/42?x=1\"")) << result.body;
    EXPECT_TRUE(contains(result.body, "\"method\":\"GET\"")) << result.body;
    EXPECT_TRUE(contains(result.body, "\"path\":\"/users/42\"")) << result.body;
    EXPECT_TRUE(contains(result.body, "\"q\":\"x=1\"")) << result.body;
    s.stop();
}

TEST(RouteVarTest, QueryHeaderCookieVars) {
    ServerFixture s;
    auto compiled = compile_route_script(
            "resp.sendJson(200, {q: $query.a, h: $header.x_forwarded_for, c: $cookie.session});", {});
    ASSERT_TRUE(compiled.ok) << compiled.error;
    s.start(make_route_script_handler(std::move(compiled), "/*"));

    ClientRequest req;
    req.method = fiber::http::HttpMethod::Get;
    req.target = "/?a=hello";
    req.headers = {{"X-Forwarded-For", "1.2.3.4"}, {"cookie", "session=abc"}};
    ClientResult result = s.request(std::move(req));

    EXPECT_EQ(result.status_code, 200) << result.body;
    EXPECT_TRUE(contains(result.body, "\"q\":\"hello\"")) << result.body;
    EXPECT_TRUE(contains(result.body, "\"h\":\"1.2.3.4\"")) << result.body;
    EXPECT_TRUE(contains(result.body, "\"c\":\"abc\"")) << result.body;
    s.stop();
}

TEST(RouteVarTest, AbsentVarsResolveToNull) {
    ServerFixture s;
    // No query param "a", no X-Forwarded-For header, no session cookie, and $path.id on a
    // catch-all location (no capture) -> all null. ($path.id compiles because the route
    // extension is configured with {"id"}, but the request matches /* which captures
    // nothing, so path_var returns null rather than failing.) Mirrors Java NullNode.
    auto compiled = compile_route_script(
            "resp.sendJson(200, {p: $path.id, q: $query.a, h: $header.x_forwarded_for, c: $cookie.session});", {"id"});
    ASSERT_TRUE(compiled.ok) << compiled.error;
    s.start(make_route_script_handler(std::move(compiled), "/*"));

    ClientRequest req;
    req.method = fiber::http::HttpMethod::Get;
    req.target = "/";
    ClientResult result = s.request(std::move(req));

    EXPECT_EQ(result.status_code, 200) << result.body;
    EXPECT_TRUE(contains(result.body, "\"p\":null")) << result.body;
    EXPECT_TRUE(contains(result.body, "\"q\":null")) << result.body;
    EXPECT_TRUE(contains(result.body, "\"h\":null")) << result.body;
    EXPECT_TRUE(contains(result.body, "\"c\":null")) << result.body;
    s.stop();
}
