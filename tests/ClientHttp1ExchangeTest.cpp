#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <future>
#include <string>

#include "async/Spawn.h"
#include "async/Timeout.h"
#include "common/IoError.h"
#include "common/mem/BufPool.h"
#include "event/EventLoop.h"
#include "event/EventLoopGroup.h"
#include "http/ClientHttp1Exchange.h"
#include "http/Http1ClientConnection.h"
#include "net/TcpListener.h"
#include "net/TcpStream.h"

namespace {

using namespace std::chrono_literals;
using fiber::async::DetachedTask;

struct CaptureOutcome {
    fiber::common::IoErr err = fiber::common::IoErr::Unknown;
    std::string bytes;
};

struct ReadHeaderOutcome {
    fiber::common::IoErr err = fiber::common::IoErr::Unknown;
    int first_status = 0;
    int second_status = 0;
    int first_status_after_second = 0;
    std::string first_reason_after_second;
    std::string reason;
    std::string header_value;
    bool response_complete = false;
    bool reusable_after_scope = false;
};

struct ReadBodyOutcome {
    fiber::common::IoErr err = fiber::common::IoErr::Unknown;
    std::string first_body;
    std::string second_body;
    bool first_last = false;
    bool second_last = false;
    bool first_pool_is_current = false;
    bool second_pool_is_current = false;
    std::string trailer_value;
    bool response_complete = false;
    bool reusable_after_scope = false;
};

struct RawStreamOutcome {
    fiber::common::IoErr err = fiber::common::IoErr::Unknown;
    int status = 0;
    std::string first_body;
    bool first_body_complete = false;
    bool eof_complete = false;
    bool raw_stream_active = false;
    bool reusable_after_scope = false;
};

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

DetachedTask run_capture_server(fiber::event::EventLoop *loop, std::promise<std::uint16_t> *port_promise,
                                std::size_t expected_size, std::promise<CaptureOutcome> *outcome_promise) {
    CaptureOutcome outcome;

    fiber::net::TcpListener listener(*loop);
    fiber::net::ListenOptions listen_options{};
    auto bind_result =
            listener.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), listen_options);
    if (!bind_result) {
        port_promise->set_value(0);
        outcome.err = bind_result.error();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }

    auto port_result = resolve_port(listener.fd());
    port_promise->set_value(port_result ? *port_result : 0);
    if (!port_result) {
        outcome.err = port_result.error();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }

    auto accept_result = co_await listener.accept();
    listener.close();
    if (!accept_result) {
        outcome.err = accept_result.error();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }

    fiber::net::TcpStream stream(*loop, accept_result->release_fd(), accept_result->take_peer());
    while (outcome.bytes.size() < expected_size) {
        char chunk[256];
        const std::size_t remaining = expected_size - outcome.bytes.size();
        auto read_result = co_await stream.read(chunk, std::min<std::size_t>(remaining, sizeof(chunk)), 2s);
        if (!read_result) {
            outcome.err = read_result.error();
            outcome_promise->set_value(std::move(outcome));
            co_return;
        }
        if (*read_result == 0) {
            outcome.err = fiber::common::IoErr::ConnReset;
            outcome_promise->set_value(std::move(outcome));
            co_return;
        }
        outcome.bytes.append(chunk, *read_result);
    }

    stream.close();
    outcome.err = fiber::common::IoErr::None;
    outcome_promise->set_value(std::move(outcome));
}

fiber::async::Task<fiber::common::IoResult<void>> read_until_header_end(fiber::net::TcpStream &stream,
                                                                        std::string &out) {
    while (out.find("\r\n\r\n") == std::string::npos) {
        char chunk[256];
        auto read_result = co_await stream.read(chunk, sizeof(chunk), 2s);
        if (!read_result) {
            co_return std::unexpected(read_result.error());
        }
        if (*read_result == 0) {
            co_return std::unexpected(fiber::common::IoErr::ConnReset);
        }
        out.append(chunk, *read_result);
    }
    co_return fiber::common::IoResult<void>{};
}

fiber::async::Task<fiber::common::IoResult<void>> read_exact(fiber::net::TcpStream &stream, std::size_t bytes,
                                                             std::string &out) {
    while (out.size() < bytes) {
        char chunk[256];
        std::size_t remaining = bytes - out.size();
        auto read_result = co_await stream.read(chunk, std::min<std::size_t>(remaining, sizeof(chunk)), 2s);
        if (!read_result) {
            co_return std::unexpected(read_result.error());
        }
        if (*read_result == 0) {
            co_return std::unexpected(fiber::common::IoErr::ConnReset);
        }
        out.append(chunk, *read_result);
    }
    co_return fiber::common::IoResult<void>{};
}

fiber::async::Task<fiber::common::IoResult<void>> write_all(fiber::net::TcpStream &stream, std::string_view bytes) {
    const char *ptr = bytes.data();
    std::size_t remaining = bytes.size();
    while (remaining > 0) {
        auto write_result = co_await stream.write(ptr, remaining, 2s);
        if (!write_result) {
            co_return std::unexpected(write_result.error());
        }
        if (*write_result == 0) {
            co_return std::unexpected(fiber::common::IoErr::ConnReset);
        }
        ptr += *write_result;
        remaining -= *write_result;
    }
    co_return fiber::common::IoResult<void>{};
}

std::string flatten_body_chunk(fiber::mem::IoBufChain &chunk) {
    std::string out;
    while (chunk.readable_bytes() > 0) {
        fiber::mem::IoBuf *front = chunk.first_readable();
        if (!front) {
            break;
        }
        out.append(reinterpret_cast<const char *>(front->readable_data()), front->readable());
        chunk.consume_and_compact(front->readable());
    }
    return out;
}

DetachedTask run_response_header_server(fiber::event::EventLoop *loop, std::promise<std::uint16_t> *port_promise,
                                        std::promise<fiber::common::IoErr> *result_promise) {
    fiber::net::TcpListener listener(*loop);
    fiber::net::ListenOptions listen_options{};
    auto bind_result =
            listener.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), listen_options);
    if (!bind_result) {
        port_promise->set_value(0);
        result_promise->set_value(bind_result.error());
        co_return;
    }

    auto port_result = resolve_port(listener.fd());
    port_promise->set_value(port_result ? *port_result : 0);
    if (!port_result) {
        result_promise->set_value(port_result.error());
        co_return;
    }

    auto accept_result = co_await listener.accept();
    listener.close();
    if (!accept_result) {
        result_promise->set_value(accept_result.error());
        co_return;
    }

    fiber::net::TcpStream stream(*loop, accept_result->release_fd(), accept_result->take_peer());
    std::string request;
    auto header_result = co_await read_until_header_end(stream, request);
    if (!header_result) {
        result_promise->set_value(header_result.error());
        co_return;
    }

    auto write_result = co_await write_all(stream, "HTTP/1.1 200 OK\r\n"
                                                   "Content-Length: 5\r\n"
                                                   "X-Test: one\r\n"
                                                   "\r\n"
                                                   "hello");
    stream.close();
    result_promise->set_value(write_result ? fiber::common::IoErr::None : write_result.error());
}

DetachedTask run_expect_continue_server(fiber::event::EventLoop *loop, std::promise<std::uint16_t> *port_promise,
                                        std::promise<fiber::common::IoErr> *result_promise) {
    fiber::net::TcpListener listener(*loop);
    fiber::net::ListenOptions listen_options{};
    auto bind_result =
            listener.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), listen_options);
    if (!bind_result) {
        port_promise->set_value(0);
        result_promise->set_value(bind_result.error());
        co_return;
    }

    auto port_result = resolve_port(listener.fd());
    port_promise->set_value(port_result ? *port_result : 0);
    if (!port_result) {
        result_promise->set_value(port_result.error());
        co_return;
    }

    auto accept_result = co_await listener.accept();
    listener.close();
    if (!accept_result) {
        result_promise->set_value(accept_result.error());
        co_return;
    }

    fiber::net::TcpStream stream(*loop, accept_result->release_fd(), accept_result->take_peer());
    std::string request_header;
    auto header_result = co_await read_until_header_end(stream, request_header);
    if (!header_result) {
        result_promise->set_value(header_result.error());
        co_return;
    }

    auto continue_result = co_await write_all(stream, "HTTP/1.1 100 Continue\r\n\r\n");
    if (!continue_result) {
        stream.close();
        result_promise->set_value(continue_result.error());
        co_return;
    }

    std::string request_body;
    auto body_result = co_await read_exact(stream, 5, request_body);
    if (!body_result) {
        stream.close();
        result_promise->set_value(body_result.error());
        co_return;
    }

    auto final_result = co_await write_all(stream, "HTTP/1.1 204 No Content\r\nX-Test: done\r\n\r\n");
    stream.close();
    result_promise->set_value(final_result ? fiber::common::IoErr::None : final_result.error());
}

DetachedTask run_large_response_header_server(fiber::event::EventLoop *loop, std::promise<std::uint16_t> *port_promise,
                                              std::promise<fiber::common::IoErr> *result_promise) {
    fiber::net::TcpListener listener(*loop);
    fiber::net::ListenOptions listen_options{};
    auto bind_result =
            listener.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), listen_options);
    if (!bind_result) {
        port_promise->set_value(0);
        result_promise->set_value(bind_result.error());
        co_return;
    }

    auto port_result = resolve_port(listener.fd());
    port_promise->set_value(port_result ? *port_result : 0);
    if (!port_result) {
        result_promise->set_value(port_result.error());
        co_return;
    }

    auto accept_result = co_await listener.accept();
    listener.close();
    if (!accept_result) {
        result_promise->set_value(accept_result.error());
        co_return;
    }

    fiber::net::TcpStream stream(*loop, accept_result->release_fd(), accept_result->take_peer());
    std::string request;
    auto header_result = co_await read_until_header_end(stream, request);
    if (!header_result) {
        result_promise->set_value(header_result.error());
        co_return;
    }

    std::string response = "HTTP/1.1 200 OK\r\n"
                           "Content-Length: 0\r\n"
                           "X-Large: " +
                           std::string(180, 'a') +
                           "\r\n"
                           "\r\n";
    auto write_result = co_await write_all(stream, response);
    stream.close();
    result_promise->set_value(write_result ? fiber::common::IoErr::None : write_result.error());
}

DetachedTask run_content_length_response_server(fiber::event::EventLoop *loop,
                                                std::promise<std::uint16_t> *port_promise,
                                                std::promise<fiber::common::IoErr> *result_promise) {
    fiber::net::TcpListener listener(*loop);
    fiber::net::ListenOptions listen_options{};
    auto bind_result =
            listener.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), listen_options);
    if (!bind_result) {
        port_promise->set_value(0);
        result_promise->set_value(bind_result.error());
        co_return;
    }

    auto port_result = resolve_port(listener.fd());
    port_promise->set_value(port_result ? *port_result : 0);
    if (!port_result) {
        result_promise->set_value(port_result.error());
        co_return;
    }

    auto accept_result = co_await listener.accept();
    listener.close();
    if (!accept_result) {
        result_promise->set_value(accept_result.error());
        co_return;
    }

    fiber::net::TcpStream stream(*loop, accept_result->release_fd(), accept_result->take_peer());
    std::string request;
    auto header_result = co_await read_until_header_end(stream, request);
    if (!header_result) {
        result_promise->set_value(header_result.error());
        co_return;
    }

    auto write_result = co_await write_all(stream, "HTTP/1.1 200 OK\r\n"
                                                   "Content-Length: 5\r\n"
                                                   "\r\n"
                                                   "hello");
    stream.close();
    result_promise->set_value(write_result ? fiber::common::IoErr::None : write_result.error());
}

DetachedTask run_chunked_response_with_trailer_server(fiber::event::EventLoop *loop,
                                                      std::promise<std::uint16_t> *port_promise,
                                                      std::promise<fiber::common::IoErr> *result_promise) {
    fiber::net::TcpListener listener(*loop);
    fiber::net::ListenOptions listen_options{};
    auto bind_result =
            listener.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), listen_options);
    if (!bind_result) {
        port_promise->set_value(0);
        result_promise->set_value(bind_result.error());
        co_return;
    }

    auto port_result = resolve_port(listener.fd());
    port_promise->set_value(port_result ? *port_result : 0);
    if (!port_result) {
        result_promise->set_value(port_result.error());
        co_return;
    }

    auto accept_result = co_await listener.accept();
    listener.close();
    if (!accept_result) {
        result_promise->set_value(accept_result.error());
        co_return;
    }

    fiber::net::TcpStream stream(*loop, accept_result->release_fd(), accept_result->take_peer());
    std::string request;
    auto header_result = co_await read_until_header_end(stream, request);
    if (!header_result) {
        result_promise->set_value(header_result.error());
        co_return;
    }

    auto first_write = co_await write_all(stream, "HTTP/1.1 200 OK\r\n"
                                                  "Transfer-Encoding: chunked\r\n"
                                                  "\r\n"
                                                  "5\r\n"
                                                  "hello\r\n"
                                                  "0\r\n");
    if (!first_write) {
        stream.close();
        result_promise->set_value(first_write.error());
        co_return;
    }

    auto second_write = co_await write_all(stream, "x-checksum: 123\r\n"
                                                   "X-Very-Long-Trailer-Name-That-Exceeds-Parser-Cache: 456\r\n"
                                                   "\r\n");
    stream.close();
    result_promise->set_value(second_write ? fiber::common::IoErr::None : second_write.error());
}

DetachedTask run_raw_stream_server(fiber::event::EventLoop *loop, std::promise<std::uint16_t> *port_promise,
                                   std::promise<CaptureOutcome> *result_promise) {
    CaptureOutcome outcome;
    fiber::net::TcpListener listener(*loop);
    fiber::net::ListenOptions listen_options{};
    auto bind_result =
            listener.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), listen_options);
    if (!bind_result) {
        port_promise->set_value(0);
        outcome.err = bind_result.error();
        result_promise->set_value(std::move(outcome));
        co_return;
    }

    auto port_result = resolve_port(listener.fd());
    port_promise->set_value(port_result ? *port_result : 0);
    if (!port_result) {
        outcome.err = port_result.error();
        result_promise->set_value(std::move(outcome));
        co_return;
    }

    auto accept_result = co_await listener.accept();
    listener.close();
    if (!accept_result) {
        outcome.err = accept_result.error();
        result_promise->set_value(std::move(outcome));
        co_return;
    }

    fiber::net::TcpStream stream(*loop, accept_result->release_fd(), accept_result->take_peer());
    std::string request;
    auto header_result = co_await read_until_header_end(stream, request);
    if (!header_result) {
        outcome.err = header_result.error();
        result_promise->set_value(std::move(outcome));
        co_return;
    }

    auto response_result = co_await write_all(stream, "HTTP/1.1 101 Switching Protocols\r\n"
                                                      "Upgrade: websocket\r\n"
                                                      "Connection: Upgrade\r\n"
                                                      "\r\n"
                                                      "server-first");
    if (!response_result) {
        stream.close();
        outcome.err = response_result.error();
        result_promise->set_value(std::move(outcome));
        co_return;
    }

    auto body_result = co_await read_exact(stream, std::string_view("client-frame").size(), outcome.bytes);
    stream.close();
    outcome.err = body_result ? fiber::common::IoErr::None : body_result.error();
    result_promise->set_value(std::move(outcome));
}

DetachedTask run_content_length_client(fiber::event::EventLoop *loop, std::uint16_t port,
                                       std::promise<fiber::common::IoErr> *result_promise,
                                       std::promise<bool> *request_done_promise) {
    fiber::http::Http1ClientConnectionOptions conn_options;
    conn_options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);

    fiber::http::Http1ClientConnection connection(*loop, std::move(conn_options));
    auto connect_result = co_await connection.connect(5s);
    if (!connect_result) {
        result_promise->set_value(connect_result.error());
        request_done_promise->set_value(false);
        co_return;
    }

    fiber::common::IoErr result = fiber::common::IoErr::Unknown;
    bool request_done = false;
    {
        fiber::mem::BufPool pool;
        fiber::http::HttpHeaders headers(pool);
        headers.add_view("host", "example.com");
        headers.add_view("x-test", "1");

        fiber::http::ClientHttp1Exchange exchange(connection, pool);
        fiber::http::Http1RequestHead head;
        head.method = fiber::http::HttpMethod::Post;
        head.target = "/submit";
        head.headers = &headers;
        head.body = fiber::http::HttpBodySpec::ContentLength(5);

        auto header_result = co_await exchange.send_header(head, false);
        if (!header_result) {
            result = header_result.error();
        } else {
            auto body_result = co_await exchange.write_body(reinterpret_cast<const std::uint8_t *>("hello"), 5, true);
            if (!body_result) {
                result = body_result.error();
            } else {
                result = fiber::common::IoErr::None;
                request_done = exchange.request_complete();
            }
        }
    }

    connection.close();
    result_promise->set_value(result);
    request_done_promise->set_value(request_done);
}

DetachedTask run_chunked_client(fiber::event::EventLoop *loop, std::uint16_t port,
                                std::promise<fiber::common::IoErr> *result_promise,
                                std::promise<bool> *request_done_promise) {
    fiber::http::Http1ClientConnectionOptions conn_options;
    conn_options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);

    fiber::http::Http1ClientConnection connection(*loop, std::move(conn_options));
    auto connect_result = co_await connection.connect(5s);
    if (!connect_result) {
        result_promise->set_value(connect_result.error());
        request_done_promise->set_value(false);
        co_return;
    }

    fiber::common::IoErr result = fiber::common::IoErr::Unknown;
    bool request_done = false;
    {
        fiber::mem::BufPool pool;
        fiber::http::HttpHeaders headers(pool);
        headers.add_view("host", "example.com");
        headers.add_view("x-test", "1");
        fiber::http::HttpHeaders trailers(pool);
        trailers.add_view("x-checksum", "123");

        fiber::http::ClientHttp1Exchange exchange(connection, pool);
        fiber::http::Http1RequestHead head;
        head.method = fiber::http::HttpMethod::Post;
        head.target = "/upload";
        head.headers = &headers;
        head.body = fiber::http::HttpBodySpec::Chunked();

        auto header_result = co_await exchange.send_header(head, false);
        if (!header_result) {
            result = header_result.error();
        } else {
            auto body_result = co_await exchange.write_body(reinterpret_cast<const std::uint8_t *>("hello"), 5, false);
            if (!body_result) {
                result = body_result.error();
            } else {
                auto trailer_result = co_await exchange.send_trailer(trailers);
                if (!trailer_result) {
                    result = trailer_result.error();
                } else {
                    result = fiber::common::IoErr::None;
                    request_done = exchange.request_complete();
                }
            }
        }
    }

    connection.close();
    result_promise->set_value(result);
    request_done_promise->set_value(request_done);
}

// Exercises the explicit Chunked write_body(IoBufChain) path.
// (ClientHttp1Exchange merges [prefix][body][suffix] into a single writev).
DetachedTask run_chunked_client_iobufchain(fiber::event::EventLoop *loop, std::uint16_t port,
                                           std::promise<fiber::common::IoErr> *result_promise,
                                           std::promise<bool> *request_done_promise) {
    fiber::http::Http1ClientConnectionOptions conn_options;
    conn_options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);

    fiber::http::Http1ClientConnection connection(*loop, std::move(conn_options));
    auto connect_result = co_await connection.connect(5s);
    if (!connect_result) {
        result_promise->set_value(connect_result.error());
        request_done_promise->set_value(false);
        co_return;
    }

    fiber::common::IoErr result = fiber::common::IoErr::Unknown;
    bool request_done = false;
    {
        fiber::mem::BufPool pool;
        fiber::http::HttpHeaders headers(pool);
        headers.add_view("host", "example.com");
        headers.add_view("x-test", "1");
        fiber::http::HttpHeaders trailers(pool);
        trailers.add_view("x-checksum", "123");

        fiber::http::ClientHttp1Exchange exchange(connection, pool);
        fiber::http::Http1RequestHead head;
        head.method = fiber::http::HttpMethod::Post;
        head.target = "/upload";
        head.headers = &headers;
        head.body = fiber::http::HttpBodySpec::Chunked();

        auto header_result = co_await exchange.send_header(head, false);
        if (!header_result) {
            result = header_result.error();
        } else {
            fiber::mem::IoBufNodePool node_pool;
            fiber::mem::IoBufChain body_chain(node_pool);
            fiber::mem::IoBuf body_buf = fiber::mem::IoBuf::allocate(5);
            std::memcpy(body_buf.writable_data(), "hello", 5);
            body_buf.commit(5);
            body_chain.append(std::move(body_buf));
            // chain.complete() == false -> trailing CRLF only, no terminator (trailer follows).

            auto body_result = co_await exchange.write_body(std::move(body_chain));
            if (!body_result) {
                result = body_result.error();
            } else {
                auto trailer_result = co_await exchange.send_trailer(trailers);
                if (!trailer_result) {
                    result = trailer_result.error();
                } else {
                    result = fiber::common::IoErr::None;
                    request_done = exchange.request_complete();
                }
            }
        }
    }

    connection.close();
    result_promise->set_value(result);
    request_done_promise->set_value(request_done);
}

DetachedTask run_empty_chunked_client(fiber::event::EventLoop *loop, std::uint16_t port,
                                      std::promise<fiber::common::IoErr> *result_promise,
                                      std::promise<bool> *request_done_promise) {
    fiber::http::Http1ClientConnectionOptions conn_options;
    conn_options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);

    fiber::http::Http1ClientConnection connection(*loop, std::move(conn_options));
    auto connect_result = co_await connection.connect(5s);
    if (!connect_result) {
        result_promise->set_value(connect_result.error());
        request_done_promise->set_value(false);
        co_return;
    }

    fiber::common::IoErr result = fiber::common::IoErr::Unknown;
    bool request_done = false;
    {
        fiber::mem::BufPool pool;
        fiber::http::HttpHeaders headers(pool);
        headers.add_view("host", "example.com");
        headers.add_view("x-test", "1");

        fiber::http::ClientHttp1Exchange exchange(connection, pool);
        fiber::http::Http1RequestHead head;
        head.method = fiber::http::HttpMethod::Post;
        head.target = "/empty";
        head.headers = &headers;
        head.body = fiber::http::HttpBodySpec::Chunked();

        auto header_result = co_await exchange.send_header(head, true);
        if (!header_result) {
            result = header_result.error();
        } else {
            result = fiber::common::IoErr::None;
            request_done = exchange.request_complete();
        }
    }

    connection.close();
    result_promise->set_value(result);
    request_done_promise->set_value(request_done);
}

DetachedTask run_auto_body_spec_client(fiber::event::EventLoop *loop, std::uint16_t port,
                                       std::promise<fiber::common::IoErr> *result_promise) {
    fiber::http::Http1ClientConnectionOptions conn_options;
    conn_options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);

    fiber::http::Http1ClientConnection connection(*loop, std::move(conn_options));
    auto connect_result = co_await connection.connect(5s);
    if (!connect_result) {
        result_promise->set_value(connect_result.error());
        co_return;
    }

    fiber::mem::BufPool pool;
    fiber::http::HttpHeaders headers(pool);
    headers.add_view("host", "example.com");

    fiber::http::ClientHttp1Exchange exchange(connection, pool);
    fiber::http::Http1RequestHead head;
    head.method = fiber::http::HttpMethod::Post;
    head.target = "/auto";
    head.headers = &headers;
    head.body = fiber::http::HttpBodySpec::Auto();

    auto header_result = co_await exchange.send_header(head, false);
    result_promise->set_value(header_result ? fiber::common::IoErr::None : header_result.error());
    connection.close();
}

DetachedTask run_read_header_client(fiber::event::EventLoop *loop, std::uint16_t port,
                                    std::promise<ReadHeaderOutcome> *result_promise) {
    ReadHeaderOutcome outcome;

    fiber::http::Http1ClientConnectionOptions conn_options;
    conn_options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);

    fiber::http::Http1ClientConnection connection(*loop, std::move(conn_options));
    auto connect_result = co_await connection.connect(5s);
    if (!connect_result) {
        outcome.err = connect_result.error();
        result_promise->set_value(std::move(outcome));
        co_return;
    }

    fiber::mem::BufPool pool;
    fiber::http::HttpHeaders headers(pool);
    headers.add_view("host", "example.com");

    {
        fiber::http::ClientHttp1Exchange exchange(connection, pool);
        fiber::http::Http1RequestHead head;
        head.method = fiber::http::HttpMethod::Get;
        head.target = "/status";
        head.headers = &headers;

        auto send_result = co_await exchange.send_header(head, true);
        if (!send_result) {
            outcome.err = send_result.error();
            result_promise->set_value(std::move(outcome));
            co_return;
        }

        auto header_result = co_await exchange.read_header();
        if (!header_result) {
            outcome.err = header_result.error();
            result_promise->set_value(std::move(outcome));
            co_return;
        }

        outcome.first_status = (*header_result)->status_code;
        outcome.reason = std::string((*header_result)->reason);
        outcome.header_value = std::string((*header_result)->headers.get("x-test"));
        outcome.response_complete = exchange.response_complete();
        outcome.err = fiber::common::IoErr::None;
    }

    outcome.reusable_after_scope = connection.reusable();
    connection.close();
    result_promise->set_value(std::move(outcome));
}

DetachedTask run_expect_continue_client(fiber::event::EventLoop *loop, std::uint16_t port,
                                        std::promise<ReadHeaderOutcome> *result_promise) {
    ReadHeaderOutcome outcome;

    fiber::http::Http1ClientConnectionOptions conn_options;
    conn_options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);

    fiber::http::Http1ClientConnection connection(*loop, std::move(conn_options));
    auto connect_result = co_await connection.connect(5s);
    if (!connect_result) {
        outcome.err = connect_result.error();
        result_promise->set_value(std::move(outcome));
        co_return;
    }

    fiber::mem::BufPool pool;
    fiber::http::HttpHeaders headers(pool);
    headers.add_view("host", "example.com");
    headers.add_view("expect", "100-continue");

    {
        fiber::http::ClientHttp1Exchange exchange(connection, pool);
        fiber::http::Http1RequestHead head;
        head.method = fiber::http::HttpMethod::Post;
        head.target = "/continue";
        head.headers = &headers;
        head.body = fiber::http::HttpBodySpec::ContentLength(5);

        auto send_result = co_await exchange.send_header(head, false);
        if (!send_result) {
            outcome.err = send_result.error();
            result_promise->set_value(std::move(outcome));
            co_return;
        }

        auto informational_result = co_await exchange.read_header();
        if (!informational_result) {
            outcome.err = informational_result.error();
            result_promise->set_value(std::move(outcome));
            co_return;
        }
        outcome.first_status = (*informational_result)->status_code;

        auto body_result = co_await exchange.write_body(reinterpret_cast<const std::uint8_t *>("hello"), 5, true);
        if (!body_result) {
            outcome.err = body_result.error();
            result_promise->set_value(std::move(outcome));
            co_return;
        }

        auto final_result = co_await exchange.read_header();
        if (!final_result) {
            outcome.err = final_result.error();
            result_promise->set_value(std::move(outcome));
            co_return;
        }
        outcome.second_status = (*final_result)->status_code;
        outcome.reason = std::string((*final_result)->reason);
        outcome.header_value = std::string((*final_result)->headers.get("x-test"));
        outcome.first_status_after_second = (*informational_result)->status_code;
        outcome.first_reason_after_second = std::string((*informational_result)->reason);
        outcome.response_complete = exchange.response_complete();
        outcome.err = fiber::common::IoErr::None;
    }

    outcome.reusable_after_scope = connection.reusable();
    connection.close();
    result_promise->set_value(std::move(outcome));
}

DetachedTask run_read_header_small_buffer_client(fiber::event::EventLoop *loop, std::uint16_t port,
                                                 std::promise<ReadHeaderOutcome> *result_promise) {
    ReadHeaderOutcome outcome;

    fiber::http::Http1ClientConnectionOptions conn_options;
    conn_options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);

    fiber::http::Http1ClientConnection connection(*loop, std::move(conn_options));
    auto connect_result = co_await connection.connect(5s);
    if (!connect_result) {
        outcome.err = connect_result.error();
        result_promise->set_value(std::move(outcome));
        co_return;
    }

    fiber::mem::BufPool pool;
    fiber::http::HttpHeaders headers(pool);
    headers.add_view("host", "example.com");

    fiber::http::Http1ClientExchangeOptions exchange_options;
    exchange_options.response_header_init_size = 32;
    exchange_options.response_header_large_size = 32;
    exchange_options.response_header_large_num = 8;

    {
        fiber::http::ClientHttp1Exchange exchange(connection, pool, exchange_options);
        fiber::http::Http1RequestHead head;
        head.method = fiber::http::HttpMethod::Get;
        head.target = "/grow";
        head.headers = &headers;

        auto send_result = co_await exchange.send_header(head, true);
        if (!send_result) {
            outcome.err = send_result.error();
            result_promise->set_value(std::move(outcome));
            co_return;
        }

        auto header_result = co_await exchange.read_header();
        if (!header_result) {
            outcome.err = header_result.error();
            result_promise->set_value(std::move(outcome));
            co_return;
        }

        outcome.first_status = (*header_result)->status_code;
        outcome.header_value = std::string((*header_result)->headers.get("x-large"));
        outcome.response_complete = exchange.response_complete();
        outcome.err = fiber::common::IoErr::None;
    }

    outcome.reusable_after_scope = connection.reusable();
    connection.close();
    result_promise->set_value(std::move(outcome));
}

DetachedTask run_read_content_length_body_client(fiber::event::EventLoop *loop, std::uint16_t port,
                                                 std::promise<ReadBodyOutcome> *result_promise) {
    ReadBodyOutcome outcome;

    fiber::http::Http1ClientConnectionOptions conn_options;
    conn_options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);

    fiber::http::Http1ClientConnection connection(*loop, std::move(conn_options));
    auto connect_result = co_await connection.connect(5s);
    if (!connect_result) {
        outcome.err = connect_result.error();
        result_promise->set_value(std::move(outcome));
        co_return;
    }

    fiber::mem::BufPool pool;
    fiber::http::HttpHeaders headers(pool);
    headers.add_view("host", "example.com");

    {
        fiber::http::ClientHttp1Exchange exchange(connection, pool);
        fiber::http::Http1RequestHead head;
        head.method = fiber::http::HttpMethod::Get;
        head.target = "/body";
        head.headers = &headers;

        auto send_result = co_await exchange.send_header(head, true);
        if (!send_result) {
            outcome.err = send_result.error();
            result_promise->set_value(std::move(outcome));
            co_return;
        }

        auto header_result = co_await exchange.read_header();
        if (!header_result) {
            outcome.err = header_result.error();
            result_promise->set_value(std::move(outcome));
            co_return;
        }

        auto first_body_result = co_await exchange.read_body(3);
        if (!first_body_result) {
            outcome.err = first_body_result.error();
            result_promise->set_value(std::move(outcome));
            co_return;
        }
        outcome.first_pool_is_current =
                &first_body_result->node_pool() == &fiber::event::EventLoop::current().io_buf_node_pool();
        outcome.first_body = flatten_body_chunk(*first_body_result);
        outcome.first_last = first_body_result->complete();

        auto second_body_result = co_await exchange.read_body(3);
        if (!second_body_result) {
            outcome.err = second_body_result.error();
            result_promise->set_value(std::move(outcome));
            co_return;
        }
        outcome.second_pool_is_current =
                &second_body_result->node_pool() == &fiber::event::EventLoop::current().io_buf_node_pool();
        outcome.second_body = flatten_body_chunk(*second_body_result);
        outcome.second_last = second_body_result->complete();
        outcome.response_complete = exchange.response_complete();
        outcome.err = fiber::common::IoErr::None;
    }

    outcome.reusable_after_scope = connection.reusable();
    connection.close();
    result_promise->set_value(std::move(outcome));
}

DetachedTask run_read_content_length_body_on_borrowed_connection_client(fiber::http::Http1ClientConnection *connection,
                                                                        std::promise<ReadBodyOutcome> *result_promise) {
    ReadBodyOutcome outcome;
    if (connection == nullptr) {
        outcome.err = fiber::common::IoErr::Invalid;
        result_promise->set_value(std::move(outcome));
        co_return;
    }

    fiber::mem::BufPool pool;
    fiber::http::HttpHeaders headers(pool);
    headers.add_view("host", "example.com");

    {
        fiber::http::ClientHttp1Exchange exchange(*connection, pool);
        fiber::http::Http1RequestHead head;
        head.method = fiber::http::HttpMethod::Get;
        head.target = "/body";
        head.headers = &headers;

        auto send_result = co_await exchange.send_header(head, true);
        if (!send_result) {
            outcome.err = send_result.error();
            result_promise->set_value(std::move(outcome));
            co_return;
        }

        auto header_result = co_await exchange.read_header();
        if (!header_result) {
            outcome.err = header_result.error();
            result_promise->set_value(std::move(outcome));
            co_return;
        }

        auto first_body_result = co_await exchange.read_body(3);
        if (!first_body_result) {
            outcome.err = first_body_result.error();
            result_promise->set_value(std::move(outcome));
            co_return;
        }
        outcome.first_pool_is_current =
                &first_body_result->node_pool() == &fiber::event::EventLoop::current().io_buf_node_pool();
        outcome.first_body = flatten_body_chunk(*first_body_result);
        outcome.first_last = first_body_result->complete();

        auto second_body_result = co_await exchange.read_body(3);
        if (!second_body_result) {
            outcome.err = second_body_result.error();
            result_promise->set_value(std::move(outcome));
            co_return;
        }
        outcome.second_pool_is_current =
                &second_body_result->node_pool() == &fiber::event::EventLoop::current().io_buf_node_pool();
        outcome.second_body = flatten_body_chunk(*second_body_result);
        outcome.second_last = second_body_result->complete();
        outcome.response_complete = exchange.response_complete();
        outcome.err = fiber::common::IoErr::None;
    }

    outcome.reusable_after_scope = connection->reusable();
    result_promise->set_value(std::move(outcome));
}

DetachedTask run_read_chunked_body_with_trailer_client(fiber::event::EventLoop *loop, std::uint16_t port,
                                                       std::promise<ReadBodyOutcome> *result_promise) {
    ReadBodyOutcome outcome;

    fiber::http::Http1ClientConnectionOptions conn_options;
    conn_options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);

    fiber::http::Http1ClientConnection connection(*loop, std::move(conn_options));
    auto connect_result = co_await connection.connect(5s);
    if (!connect_result) {
        outcome.err = connect_result.error();
        result_promise->set_value(std::move(outcome));
        co_return;
    }

    fiber::mem::BufPool pool;
    fiber::http::HttpHeaders headers(pool);
    headers.add_view("host", "example.com");

    {
        fiber::http::ClientHttp1Exchange exchange(connection, pool);
        fiber::http::Http1RequestHead head;
        head.method = fiber::http::HttpMethod::Get;
        head.target = "/chunked";
        head.headers = &headers;

        auto send_result = co_await exchange.send_header(head, true);
        if (!send_result) {
            outcome.err = send_result.error();
            result_promise->set_value(std::move(outcome));
            co_return;
        }

        auto header_result = co_await exchange.read_header();
        if (!header_result) {
            outcome.err = header_result.error();
            result_promise->set_value(std::move(outcome));
            co_return;
        }

        auto body_result = co_await exchange.read_body(64);
        if (!body_result) {
            outcome.err = body_result.error();
            result_promise->set_value(std::move(outcome));
            co_return;
        }
        outcome.first_pool_is_current =
                &body_result->node_pool() == &fiber::event::EventLoop::current().io_buf_node_pool();
        outcome.first_body = flatten_body_chunk(*body_result);
        outcome.first_last = body_result->complete();
        outcome.trailer_value = std::string(exchange.response_trailers().get("x-checksum"));
        outcome.trailer_value.push_back('|');
        outcome.trailer_value.append(
                exchange.response_trailers().get("x-very-long-trailer-name-that-exceeds-parser-cache"));
        outcome.response_complete = exchange.response_complete();
        outcome.err = fiber::common::IoErr::None;
    }

    outcome.reusable_after_scope = connection.reusable();
    connection.close();
    result_promise->set_value(std::move(outcome));
}

DetachedTask run_discard_chunked_body_with_trailer_client(fiber::event::EventLoop *loop, std::uint16_t port,
                                                          std::promise<ReadBodyOutcome> *result_promise) {
    ReadBodyOutcome outcome;

    fiber::http::Http1ClientConnectionOptions conn_options;
    conn_options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);

    fiber::http::Http1ClientConnection connection(*loop, std::move(conn_options));
    auto connect_result = co_await connection.connect(5s);
    if (!connect_result) {
        outcome.err = connect_result.error();
        result_promise->set_value(std::move(outcome));
        co_return;
    }

    fiber::mem::BufPool pool;
    fiber::http::HttpHeaders headers(pool);
    headers.add_view("host", "example.com");

    {
        fiber::http::ClientHttp1Exchange exchange(connection, pool);
        fiber::http::Http1RequestHead head;
        head.method = fiber::http::HttpMethod::Get;
        head.target = "/discard";
        head.headers = &headers;

        auto send_result = co_await exchange.send_header(head, true);
        if (!send_result) {
            outcome.err = send_result.error();
            result_promise->set_value(std::move(outcome));
            co_return;
        }

        auto header_result = co_await exchange.read_header();
        if (!header_result) {
            outcome.err = header_result.error();
            result_promise->set_value(std::move(outcome));
            co_return;
        }

        auto discard_result = co_await exchange.discard_response_body();
        if (!discard_result) {
            outcome.err = discard_result.error();
            result_promise->set_value(std::move(outcome));
            co_return;
        }

        outcome.trailer_value = std::string(exchange.response_trailers().get("x-checksum"));
        outcome.trailer_value.push_back('|');
        outcome.trailer_value.append(
                exchange.response_trailers().get("x-very-long-trailer-name-that-exceeds-parser-cache"));
        outcome.response_complete = exchange.response_complete();
        outcome.err = fiber::common::IoErr::None;
    }

    outcome.reusable_after_scope = connection.reusable();
    connection.close();
    result_promise->set_value(std::move(outcome));
}

DetachedTask run_raw_stream_client(fiber::event::EventLoop *loop, std::uint16_t port,
                                   std::promise<RawStreamOutcome> *result_promise) {
    RawStreamOutcome outcome;
    fiber::http::Http1ClientConnectionOptions conn_options;
    conn_options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);
    fiber::http::Http1ClientConnection connection(*loop, std::move(conn_options));
    auto connect_result = co_await connection.connect(5s);
    if (!connect_result) {
        outcome.err = connect_result.error();
        result_promise->set_value(std::move(outcome));
        co_return;
    }

    {
        fiber::mem::BufPool pool;
        fiber::http::HttpHeaders headers(pool);
        headers.add_view("host", "example.com");
        headers.add_view("upgrade", "websocket");
        headers.add_view("connection", "Upgrade");

        fiber::http::ClientHttp1Exchange exchange(connection, pool);
        auto send_result = co_await exchange.send_header(
                {
                        .method = fiber::http::HttpMethod::Get,
                        .target = "/chat",
                        .headers = &headers,
                },
                true);
        if (!send_result) {
            outcome.err = send_result.error();
            result_promise->set_value(std::move(outcome));
            co_return;
        }

        auto header_result = co_await exchange.read_header();
        if (!header_result) {
            outcome.err = header_result.error();
            result_promise->set_value(std::move(outcome));
            co_return;
        }
        outcome.status = (*header_result)->status_code;

        auto switch_result = exchange.switch_to_raw_stream();
        if (!switch_result) {
            outcome.err = switch_result.error();
            result_promise->set_value(std::move(outcome));
            co_return;
        }
        outcome.raw_stream_active = exchange.raw_stream_active();

        auto first_body_result = co_await exchange.read_body(64);
        if (!first_body_result) {
            outcome.err = first_body_result.error();
            result_promise->set_value(std::move(outcome));
            co_return;
        }
        outcome.first_body_complete = first_body_result->complete();
        outcome.first_body = flatten_body_chunk(*first_body_result);

        auto write_result = co_await exchange.write_body(reinterpret_cast<const std::uint8_t *>("client-frame"),
                                                         std::string_view("client-frame").size(), true);
        if (!write_result) {
            outcome.err = write_result.error();
            result_promise->set_value(std::move(outcome));
            co_return;
        }

        auto eof_result = co_await exchange.read_body(64);
        if (!eof_result) {
            outcome.err = eof_result.error();
            result_promise->set_value(std::move(outcome));
            co_return;
        }
        outcome.eof_complete = eof_result->complete();
        outcome.err = fiber::common::IoErr::None;
    }

    outcome.reusable_after_scope = connection.reusable();
    connection.close();
    result_promise->set_value(std::move(outcome));
}

TEST(ClientHttp1ExchangeTest, SendHeaderAndContentLengthBodyWriteRawHttp1Request) {
    const std::string expected = "POST /submit HTTP/1.1\r\n"
                                 "Content-Length: 5\r\n"
                                 "host: example.com\r\n"
                                 "x-test: 1\r\n"
                                 "\r\n"
                                 "hello";

    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::promise<CaptureOutcome> capture_promise;
    auto capture_future = capture_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_capture_server(&group.at(0), &port_promise, expected.size(), &capture_promise);
    });

    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<fiber::common::IoErr> result_promise;
    auto result_future = result_promise.get_future();
    std::promise<bool> request_done_promise;
    auto request_done_future = request_done_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_content_length_client(&group.at(0), port, &result_promise, &request_done_promise);
    });

    EXPECT_EQ(result_future.get(), fiber::common::IoErr::None);
    EXPECT_TRUE(request_done_future.get());

    CaptureOutcome capture = capture_future.get();
    EXPECT_EQ(capture.err, fiber::common::IoErr::None);
    EXPECT_EQ(capture.bytes, expected);

    group.stop();
    group.join();
}

TEST(ClientHttp1ExchangeTest, SendChunkedBodyAndTrailerAsChunkedHttp1Request) {
    const std::string expected = "POST /upload HTTP/1.1\r\n"
                                 "Transfer-Encoding: chunked\r\n"
                                 "host: example.com\r\n"
                                 "x-test: 1\r\n"
                                 "\r\n"
                                 "5\r\n"
                                 "hello\r\n"
                                 "0\r\n"
                                 "x-checksum: 123\r\n"
                                 "\r\n";

    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::promise<CaptureOutcome> capture_promise;
    auto capture_future = capture_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_capture_server(&group.at(0), &port_promise, expected.size(), &capture_promise);
    });

    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<fiber::common::IoErr> result_promise;
    auto result_future = result_promise.get_future();
    std::promise<bool> request_done_promise;
    auto request_done_future = request_done_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_chunked_client(&group.at(0), port, &result_promise, &request_done_promise);
    });

    EXPECT_EQ(result_future.get(), fiber::common::IoErr::None);
    EXPECT_TRUE(request_done_future.get());

    CaptureOutcome capture = capture_future.get();
    EXPECT_EQ(capture.err, fiber::common::IoErr::None);
    EXPECT_EQ(capture.bytes, expected);

    group.stop();
    group.join();
}

TEST(ClientHttp1ExchangeTest, SendChunkedBodyIoBufChainWriteRawHttp1Request) {
    // Same wire output as SendChunkedBodyAndTrailerWriteRawHttp1Request, but the
    // body is sent via write_body(IoBufChain) -> single coalesced writev.
    const std::string expected = "POST /upload HTTP/1.1\r\n"
                                 "Transfer-Encoding: chunked\r\n"
                                 "host: example.com\r\n"
                                 "x-test: 1\r\n"
                                 "\r\n"
                                 "5\r\n"
                                 "hello\r\n"
                                 "0\r\n"
                                 "x-checksum: 123\r\n"
                                 "\r\n";

    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::promise<CaptureOutcome> capture_promise;
    auto capture_future = capture_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_capture_server(&group.at(0), &port_promise, expected.size(), &capture_promise);
    });

    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<fiber::common::IoErr> result_promise;
    auto result_future = result_promise.get_future();
    std::promise<bool> request_done_promise;
    auto request_done_future = request_done_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_chunked_client_iobufchain(&group.at(0), port, &result_promise, &request_done_promise);
    });

    EXPECT_EQ(result_future.get(), fiber::common::IoErr::None);
    EXPECT_TRUE(request_done_future.get());

    CaptureOutcome capture = capture_future.get();
    EXPECT_EQ(capture.err, fiber::common::IoErr::None);
    EXPECT_EQ(capture.bytes, expected);

    group.stop();
    group.join();
}

TEST(ClientHttp1ExchangeTest, SendEmptyChunkedRequestFromHeaderWriteRawHttp1Request) {
    const std::string expected = "POST /empty HTTP/1.1\r\n"
                                 "Transfer-Encoding: chunked\r\n"
                                 "host: example.com\r\n"
                                 "x-test: 1\r\n"
                                 "\r\n"
                                 "0\r\n"
                                 "\r\n";

    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::promise<CaptureOutcome> capture_promise;
    auto capture_future = capture_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_capture_server(&group.at(0), &port_promise, expected.size(), &capture_promise);
    });

    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<fiber::common::IoErr> result_promise;
    auto result_future = result_promise.get_future();
    std::promise<bool> request_done_promise;
    auto request_done_future = request_done_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_empty_chunked_client(&group.at(0), port, &result_promise, &request_done_promise);
    });

    EXPECT_EQ(result_future.get(), fiber::common::IoErr::None);
    EXPECT_TRUE(request_done_future.get());

    CaptureOutcome capture = capture_future.get();
    EXPECT_EQ(capture.err, fiber::common::IoErr::None);
    EXPECT_EQ(capture.bytes, expected);

    group.stop();
    group.join();
}

TEST(ClientHttp1ExchangeTest, RejectsAutoBodySpecForHttp1RequestHeader) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::promise<CaptureOutcome> capture_promise;
    auto capture_future = capture_promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return run_capture_server(&group.at(0), &port_promise, 0, &capture_promise); });

    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<fiber::common::IoErr> result_promise;
    auto result_future = result_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return run_auto_body_spec_client(&group.at(0), port, &result_promise); });

    EXPECT_EQ(result_future.get(), fiber::common::IoErr::Invalid);

    CaptureOutcome capture = capture_future.get();
    EXPECT_EQ(capture.err, fiber::common::IoErr::None);
    EXPECT_TRUE(capture.bytes.empty());

    group.stop();
    group.join();
}

TEST(ClientHttp1ExchangeTest, ReadFinalResponseHeaderParsesStatusReasonAndHeaders) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::promise<fiber::common::IoErr> server_result_promise;
    auto server_result_future = server_result_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_response_header_server(&group.at(0), &port_promise, &server_result_promise);
    });

    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<ReadHeaderOutcome> client_result_promise;
    auto client_result_future = client_result_promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return run_read_header_client(&group.at(0), port, &client_result_promise); });

    ReadHeaderOutcome outcome = client_result_future.get();
    EXPECT_EQ(outcome.err, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.first_status, 200);
    EXPECT_EQ(outcome.reason, "OK");
    EXPECT_EQ(outcome.header_value, "one");
    EXPECT_FALSE(outcome.response_complete);
    EXPECT_FALSE(outcome.reusable_after_scope);

    EXPECT_EQ(server_result_future.get(), fiber::common::IoErr::None);

    group.stop();
    group.join();
}

TEST(ClientHttp1ExchangeTest, ReadHeaderSupportsInformationalResponseBeforeRequestBody) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::promise<fiber::common::IoErr> server_result_promise;
    auto server_result_future = server_result_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_expect_continue_server(&group.at(0), &port_promise, &server_result_promise);
    });

    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<ReadHeaderOutcome> client_result_promise;
    auto client_result_future = client_result_promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return run_expect_continue_client(&group.at(0), port, &client_result_promise); });

    ReadHeaderOutcome outcome = client_result_future.get();
    EXPECT_EQ(outcome.err, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.first_status, 100);
    EXPECT_EQ(outcome.second_status, 204);
    EXPECT_EQ(outcome.first_status_after_second, 100);
    EXPECT_EQ(outcome.first_reason_after_second, "Continue");
    EXPECT_EQ(outcome.reason, "No Content");
    EXPECT_EQ(outcome.header_value, "done");
    EXPECT_TRUE(outcome.response_complete);
    EXPECT_TRUE(outcome.reusable_after_scope);

    EXPECT_EQ(server_result_future.get(), fiber::common::IoErr::None);

    group.stop();
    group.join();
}

TEST(ClientHttp1ExchangeTest, ReadHeaderGrowsResponseHeaderParseBufferWhenNeeded) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::promise<fiber::common::IoErr> server_result_promise;
    auto server_result_future = server_result_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_large_response_header_server(&group.at(0), &port_promise, &server_result_promise);
    });

    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<ReadHeaderOutcome> client_result_promise;
    auto client_result_future = client_result_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_read_header_small_buffer_client(&group.at(0), port, &client_result_promise);
    });

    ReadHeaderOutcome outcome = client_result_future.get();
    EXPECT_EQ(outcome.err, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.first_status, 200);
    EXPECT_EQ(outcome.header_value.size(), 180);
    EXPECT_TRUE(
            std::all_of(outcome.header_value.begin(), outcome.header_value.end(), [](char ch) { return ch == 'a'; }));
    EXPECT_TRUE(outcome.response_complete);
    EXPECT_TRUE(outcome.reusable_after_scope);

    EXPECT_EQ(server_result_future.get(), fiber::common::IoErr::None);

    group.stop();
    group.join();
}

TEST(ClientHttp1ExchangeTest, ReadContentLengthBodyReturnsLastOnFinalChunk) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::promise<fiber::common::IoErr> server_result_promise;
    auto server_result_future = server_result_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_content_length_response_server(&group.at(0), &port_promise, &server_result_promise);
    });

    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<ReadBodyOutcome> client_result_promise;
    auto client_result_future = client_result_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_read_content_length_body_client(&group.at(0), port, &client_result_promise);
    });

    ReadBodyOutcome outcome = client_result_future.get();
    EXPECT_EQ(outcome.err, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.first_body, "hel");
    EXPECT_FALSE(outcome.first_last);
    EXPECT_TRUE(outcome.first_pool_is_current);
    EXPECT_EQ(outcome.second_body, "lo");
    EXPECT_TRUE(outcome.second_last);
    EXPECT_TRUE(outcome.second_pool_is_current);
    EXPECT_TRUE(outcome.response_complete);
    EXPECT_TRUE(outcome.reusable_after_scope);

    EXPECT_EQ(server_result_future.get(), fiber::common::IoErr::None);

    group.stop();
    group.join();
}

TEST(ClientHttp1ExchangeTest, ReadBodyUsesCurrentLoopNodePoolForBorrowedConnection) {
    fiber::event::EventLoopGroup group(2);
    group.start();

    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::promise<fiber::common::IoErr> server_result_promise;
    auto server_result_future = server_result_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_content_length_response_server(&group.at(0), &port_promise, &server_result_promise);
    });

    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    fiber::http::Http1ClientConnectionOptions conn_options;
    conn_options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);
    fiber::http::Http1ClientConnection connection(group.at(0), std::move(conn_options));

    std::promise<fiber::common::IoErr> connect_promise;
    auto connect_future = connect_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        auto connect_result = co_await connection.connect(5s);
        connect_promise.set_value(connect_result ? fiber::common::IoErr::None : connect_result.error());
    });
    ASSERT_EQ(connect_future.get(), fiber::common::IoErr::None);

    std::promise<ReadBodyOutcome> client_result_promise;
    auto client_result_future = client_result_promise.get_future();
    fiber::async::spawn(group.at(1), [&]() {
        return run_read_content_length_body_on_borrowed_connection_client(&connection, &client_result_promise);
    });

    ReadBodyOutcome outcome = client_result_future.get();
    EXPECT_EQ(outcome.err, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.first_body, "hel");
    EXPECT_FALSE(outcome.first_last);
    EXPECT_TRUE(outcome.first_pool_is_current);
    EXPECT_EQ(outcome.second_body, "lo");
    EXPECT_TRUE(outcome.second_last);
    EXPECT_TRUE(outcome.second_pool_is_current);
    EXPECT_TRUE(outcome.response_complete);
    EXPECT_TRUE(outcome.reusable_after_scope);

    std::promise<void> close_promise;
    auto close_future = close_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        connection.close();
        close_promise.set_value();
        co_return;
    });
    close_future.get();

    EXPECT_EQ(server_result_future.get(), fiber::common::IoErr::None);

    group.stop();
    group.join();
}

TEST(ClientHttp1ExchangeTest, ReadChunkedBodyWaitsForTrailersBeforeLastChunk) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::promise<fiber::common::IoErr> server_result_promise;
    auto server_result_future = server_result_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_chunked_response_with_trailer_server(&group.at(0), &port_promise, &server_result_promise);
    });

    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<ReadBodyOutcome> client_result_promise;
    auto client_result_future = client_result_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_read_chunked_body_with_trailer_client(&group.at(0), port, &client_result_promise);
    });

    ReadBodyOutcome outcome = client_result_future.get();
    EXPECT_EQ(outcome.err, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.first_body, "hello");
    EXPECT_TRUE(outcome.first_last);
    EXPECT_TRUE(outcome.first_pool_is_current);
    EXPECT_EQ(outcome.trailer_value, "123|456");
    EXPECT_TRUE(outcome.response_complete);
    EXPECT_TRUE(outcome.reusable_after_scope);

    EXPECT_EQ(server_result_future.get(), fiber::common::IoErr::None);

    group.stop();
    group.join();
}

TEST(ClientHttp1ExchangeTest, DiscardResponseBodyConsumesChunkedTrailers) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::promise<fiber::common::IoErr> server_result_promise;
    auto server_result_future = server_result_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_chunked_response_with_trailer_server(&group.at(0), &port_promise, &server_result_promise);
    });

    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<ReadBodyOutcome> client_result_promise;
    auto client_result_future = client_result_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_discard_chunked_body_with_trailer_client(&group.at(0), port, &client_result_promise);
    });

    ReadBodyOutcome outcome = client_result_future.get();
    EXPECT_EQ(outcome.err, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.trailer_value, "123|456");
    EXPECT_TRUE(outcome.response_complete);
    EXPECT_TRUE(outcome.reusable_after_scope);

    EXPECT_EQ(server_result_future.get(), fiber::common::IoErr::None);

    group.stop();
    group.join();
}

TEST(ClientHttp1ExchangeTest, SwitchToRawStreamPreservesPendingBytesAndWritesWithoutFraming) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::promise<CaptureOutcome> server_result_promise;
    auto server_result_future = server_result_promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return run_raw_stream_server(&group.at(0), &port_promise, &server_result_promise); });

    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<RawStreamOutcome> client_result_promise;
    auto client_result_future = client_result_promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return run_raw_stream_client(&group.at(0), port, &client_result_promise); });

    RawStreamOutcome client = client_result_future.get();
    CaptureOutcome server = server_result_future.get();
    EXPECT_EQ(client.err, fiber::common::IoErr::None);
    EXPECT_EQ(client.status, 101);
    EXPECT_TRUE(client.raw_stream_active);
    EXPECT_EQ(client.first_body, "server-first");
    EXPECT_FALSE(client.first_body_complete);
    EXPECT_TRUE(client.eof_complete);
    EXPECT_FALSE(client.reusable_after_scope);
    EXPECT_EQ(server.err, fiber::common::IoErr::None);
    EXPECT_EQ(server.bytes, "client-frame");

    group.stop();
    group.join();
}

} // namespace
