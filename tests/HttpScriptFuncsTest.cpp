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

#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/common/mem/BufPool.h>
#include <fiber/common/mem/IoBuf.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/ClientHttp1Exchange.h>
#include <fiber/http/ClientHttp1Types.h>
#include <fiber/http/Http1ClientConnection.h>
#include <fiber/http/HttpBodySpec.h>
#include <fiber/http/HttpCommon.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/http/HttpHeaders.h>
#include <fiber/http/HttpServer.h>
#include <fiber/net/IpAddress.h>
#include <fiber/net/SocketAddress.h>

#include <fiber/common/util/RoutePathMatcher.h>
#include <fiber/http_script/ExchangeConstExtension.h>
#include <fiber/http_script/HttpScriptLib.h>
#include <fiber/http_script/RouteScriptExtension.h>
#include <fiber/http_script/ScriptExchangeCtx.h>
#include <fiber/script/JsGc.h>
#include <fiber/script/JsValue.h>
#include <fiber/script/Script.h>
#include <fiber/script/ScriptCompiler.h>
#include <fiber/script/ScriptResult.h>
#include <fiber/script/std/StdLibrary.h>

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
        co_await server->shutdown_and_wait();
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

// Compiles a script against a per-location constant builder seeded with path_var_names and a
// fresh StdLibrary with the HTTP functions. The immutable package owns constant userdata.
struct CompiledRouteScript {
    std::shared_ptr<fiber::script::Script> script; // nullptr on compile failure
    std::shared_ptr<const fiber::http_script::ConstPackage> const_package;
    std::shared_ptr<fiber::script::std_lib::StdLibrary> shared_lib;
    std::shared_ptr<fiber::http_script::ExchangeConstExtension> exchange_extension;
    std::shared_ptr<fiber::http_script::RouteScriptExtension> route_extension;
    bool ok = false;
    std::string error;
};

CompiledRouteScript compile_route_script(std::string_view source, const std::vector<std::string> &path_var_names) {
    CompiledRouteScript out;
    out.shared_lib = std::make_shared<fiber::script::std_lib::StdLibrary>();
    fiber::http_script::register_http_functions_to_lib(*out.shared_lib);
    out.exchange_extension = std::make_shared<fiber::http_script::ExchangeConstExtension>();
    out.route_extension = std::make_shared<fiber::http_script::RouteScriptExtension>();
    out.shared_lib->add_ext_ops(out.exchange_extension.get(), fiber::http_script::ExchangeConstExtension::ops());
    out.shared_lib->add_ext_ops(out.route_extension.get(), fiber::http_script::RouteScriptExtension::ops());
    fiber::http_script::ConstPackage::Builder constants;
    fiber::http_script::RouteScriptExtension::CompileScope compile_scope(*out.route_extension, constants,
                                                                         path_var_names, true);
    auto compiled = fiber::script::compile_script(*out.shared_lib, source);
    if (!compiled) {
        out.error = std::string(compiled.error().message);
        return out; // script stays nullptr, ok stays false
    }
    out.script = std::make_shared<fiber::script::Script>(std::move(*compiled));
    out.const_package = constants.build();
    if (!out.const_package) {
        out.error = "failed to build constant package";
        out.script.reset();
        return out;
    }
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
    auto const_package = compiled.const_package;
    auto exchange_extension = compiled.exchange_extension;
    auto route_extension = compiled.route_extension;
    auto shared_lib = compiled.shared_lib;
    return [script, const_package, matcher, exchange_extension, route_extension,
            shared_lib](fiber::http::HttpExchange &exchange) -> Task<void> {
        fiber::script::GcHeap heap;
        fiber::http_script::ScriptExchangeCtx ctx{
                exchange, heap, fiber::http_script::ScriptConnectionInfo{.scheme = "http", .tls = false}};
        RouteMatchCollector mc;
        std::string_view path = exchange.uri().path;
        (void) matcher->match_path(path, mc);
        auto constants_ready = ctx.prepare_constants(*const_package);
        EXPECT_TRUE(constants_ready.has_value());
        EXPECT_TRUE(ctx.bind_path_constants(*const_package, mc.vars));
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

    const std::vector<std::string> id_path_vars{"id"};
    fiber::http_script::ConstPackage::Builder first_constants;
    {
        fiber::http_script::RouteScriptExtension::CompileScope compile_scope(route_extension, first_constants,
                                                                             id_path_vars, true);
        auto first = fiber::script::compile_script(library, "resp.sendJson(200, $path.id);");
        ASSERT_TRUE(first.has_value()) << first.error().message;
    }
    ASSERT_TRUE(first_constants.build());

    const std::vector<std::string> slug_path_vars{"slug"};
    fiber::http_script::ConstPackage::Builder second_constants;
    {
        fiber::http_script::RouteScriptExtension::CompileScope compile_scope(route_extension, second_constants,
                                                                             slug_path_vars, true);
        auto stale = fiber::script::compile_script(library, "resp.sendJson(200, $path.id);");
        EXPECT_FALSE(stale.has_value());
        auto current = fiber::script::compile_script(library, "resp.sendJson(200, $path.slug);");
        EXPECT_TRUE(current.has_value()) << current.error().message;
    }
    ASSERT_TRUE(second_constants.build());
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

TEST(RouteVarTest, ExchangeFieldsCompileWithoutRouteScopeOrPackageSlots) {
    fiber::script::std_lib::StdLibrary library;
    fiber::http_script::ExchangeConstExtension exchange_extension;
    library.add_ext_ops(&exchange_extension, fiber::http_script::ExchangeConstExtension::ops());

    auto compiled = fiber::script::compile_script(
            library, "return [$req.uri, $req.method, $req.path, $req.query, $conn.scheme, $conn.http_version];");
    ASSERT_TRUE(compiled.has_value()) << compiled.error().message;

    auto route_compiled = compile_route_script(
            "return [$req.uri, $req.method, $req.path, $req.query, $conn.scheme, $conn.http_version];", {});
    ASSERT_TRUE(route_compiled.ok) << route_compiled.error;
    ASSERT_NE(route_compiled.const_package, nullptr);
    EXPECT_TRUE(route_compiled.const_package->empty());
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

TEST(RouteVarTest, ExchangeStringsBorrowRequestMemoryAndCacheRemoteAddress) {
    fiber::script::std_lib::StdLibrary library;
    fiber::http_script::ExchangeConstExtension exchange_extension;
    library.add_ext_ops(&exchange_extension, fiber::http_script::ExchangeConstExtension::ops());
    const auto *path_constant = library.resolve_constant("$req", "path");
    const auto *remote_addr_constant = library.resolve_constant("$conn", "remote_addr");
    ASSERT_NE(path_constant, nullptr);
    ASSERT_NE(remote_addr_constant, nullptr);

    ServerFixture s;
    s.start([path_constant, remote_addr_constant](fiber::http::HttpExchange &exchange) -> Task<void> {
        fiber::script::GcHeap heap;
        fiber::http_script::ScriptExchangeCtx context{
                exchange, heap, fiber::http_script::ScriptConnectionInfo{.scheme = "http", .tls = false}};
        fiber::script::Library::HostCallFrame frame(heap, fiber::script::JsValue::make_undefined(), &context);

        auto path = path_constant->constant(path_constant->userdata, frame);
        EXPECT_TRUE(path.is_success());
        if (!path.is_success()) {
            (void) co_await context.write_empty(500);
            co_return;
        }
        EXPECT_TRUE(fiber::script::js_value_is_borrowed_string(path.value()));
        if (!fiber::script::js_value_is_borrowed_string(path.value())) {
            (void) co_await context.write_empty(500);
            co_return;
        }
        const fiber::script::NativeStr path_string = fiber::script::js_value_native_string(path.value());
        EXPECT_EQ(path_string.data, exchange.uri().path.data());
        EXPECT_EQ(path_string.len, exchange.uri().path.size());

        auto first_remote = remote_addr_constant->constant(remote_addr_constant->userdata, frame);
        auto second_remote = remote_addr_constant->constant(remote_addr_constant->userdata, frame);
        EXPECT_TRUE(first_remote.is_success());
        EXPECT_TRUE(second_remote.is_success());
        if (!first_remote.is_success() || !second_remote.is_success()) {
            (void) co_await context.write_empty(500);
            co_return;
        }
        EXPECT_TRUE(fiber::script::js_value_is_borrowed_string(first_remote.value()));
        EXPECT_TRUE(fiber::script::js_value_is_borrowed_string(second_remote.value()));
        if (!fiber::script::js_value_is_borrowed_string(first_remote.value()) ||
            !fiber::script::js_value_is_borrowed_string(second_remote.value())) {
            (void) co_await context.write_empty(500);
            co_return;
        }
        const fiber::script::NativeStr first_string = fiber::script::js_value_native_string(first_remote.value());
        const fiber::script::NativeStr second_string = fiber::script::js_value_native_string(second_remote.value());
        EXPECT_EQ(first_string.data, second_string.data);
        EXPECT_EQ(first_string.len, second_string.len);

        (void) co_await context.write_empty(204);
    });

    ClientRequest req;
    req.method = fiber::http::HttpMethod::Get;
    req.target = "/native";
    ClientResult result = s.request(std::move(req));
    EXPECT_EQ(result.status_code, 204);
    s.stop();
}

TEST(RouteVarTest, DynamicNamespacesAlwaysResolve) {
    // $query/$header/$cookie always compile (slot exists; value resolved at request time).
    auto compiled = compile_route_script(
            "resp.sendJson(200, {q: $query.a, h: $header.x_forwarded_for, c: $cookie.session});", {});
    EXPECT_TRUE(compiled.ok) << compiled.error;
}

TEST(RouteVarTest, RejectsExecutionAgainstAnotherConstantPackage) {
    ServerFixture s;
    auto compiled = compile_route_script("resp.sendJson(200, $header.first);", {});
    auto other = compile_route_script("resp.sendJson(200, $header.second);", {});
    ASSERT_TRUE(compiled.ok) << compiled.error;
    ASSERT_TRUE(other.ok) << other.error;
    // Keep the script's userdata owner alive while preparing slots from another package.
    auto userdata_package = compiled.const_package;
    compiled.const_package = std::move(other.const_package);
    s.start(make_route_script_handler(std::move(compiled), "/*"));

    ClientRequest req;
    req.method = fiber::http::HttpMethod::Get;
    req.target = "/identity";
    ClientResult result = s.request(std::move(req));

    EXPECT_EQ(result.status_code, 500);
    s.stop();
    userdata_package.reset();
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
    // nothing, so the prepared path slot remains null rather than failing.) Mirrors Java NullNode.
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
