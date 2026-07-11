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

#include "http_script/HttpScriptLib.h"
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
    auto connect_result = co_await connection.connect();
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
        auto write_result = co_await exchange.write_body(reinterpret_cast<const std::uint8_t *>(req.body->data()),
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
