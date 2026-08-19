#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/common/mem/BufPool.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/ClientHttp3Exchange.h>
#include <fiber/http/Http3Client.h>
#include <fiber/http/Http3Server.h>
#include "QuicTestTlsCertificate.h"

namespace {

using namespace std::chrono_literals;

struct ServerObservation {
    fiber::common::IoErr error = fiber::common::IoErr::None;
    fiber::http::HttpVersion version = fiber::http::HttpVersion::HTTP_0_9;
    std::string path;
    std::string host;
    std::string body;
    std::string trailer;
};

struct ClientObservation {
    fiber::common::IoErr error = fiber::common::IoErr::None;
    fiber::http::Http3ClientConnectPhase connect_phase = fiber::http::Http3ClientConnectPhase::ClientInit;
    int informational_status = 0;
    int status = 0;
    int head_status = 0;
    std::string body;
    std::string trailer;
    fiber::http::Http3RequestOutcome outcome = fiber::http::Http3RequestOutcome::NotSent;
    bool body_complete = false;
    bool head_body_complete = false;
    bool trailer_end_stream = false;
};

struct PartialClientObservation {
    fiber::common::IoErr error = fiber::common::IoErr::None;
    std::size_t first_written = 0;
    std::size_t total_written = 0;
    std::size_t write_calls = 0;
    int status = 0;
    std::string response_body;
    bool response_complete = false;
};

std::string chain_to_string(fiber::mem::IoBufChain chain) {
    std::string result;
    while (fiber::mem::IoBuf *buf = chain.first_readable()) {
        result.append(reinterpret_cast<const char *>(buf->readable_data()), buf->readable());
        chain.consume_and_compact(buf->readable());
    }
    return result;
}

fiber::async::Task<void> echo_handler(fiber::http::HttpExchange &exchange,
                                      std::shared_ptr<std::promise<ServerObservation>> promise) {
    ServerObservation observation{};
    observation.version = exchange.version();
    observation.path.assign(exchange.uri().path);
    observation.host.assign(exchange.request_headers().get("host"));
    if (observation.path == "/http3/head") {
        fiber::http::HttpHeaders headers(exchange.pool());
        headers.set("content-length", "17");
        (void) co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 200,
                .headers = &headers,
                .body = fiber::http::HttpBodySpec::None(),
                .end_stream = true,
        });
        co_return;
    }
    for (;;) {
        auto chunk = co_await exchange.read_body(64 * 1024, 2s);
        if (!chunk) {
            observation.error = chunk.error();
            promise->set_value(std::move(observation));
            co_return;
        }
        const bool complete = chunk->complete();
        observation.body.append(chain_to_string(std::move(*chunk)));
        if (complete) {
            break;
        }
    }
    observation.trailer.assign(exchange.request_trailers().get("digest"));
    promise->set_value(observation);

    auto sent_informational = co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Informational,
            .status_code = 103,
            .end_stream = false,
    });
    if (!sent_informational) {
        co_return;
    }
    fiber::http::HttpHeaders headers(exchange.pool());
    const std::string content_length = std::to_string(observation.body.size());
    headers.set("content-length", content_length);
    auto sent_head = co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Final,
            .status_code = 200,
            .headers = &headers,
            .body = fiber::http::HttpBodySpec::ContentLength(observation.body.size()),
            .end_stream = observation.body.empty(),
    });
    if (!sent_head || observation.body.empty()) {
        co_return;
    }
    auto sent_body = co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(observation.body.data()),
                                                 observation.body.size(), false, 2s);
    if (!sent_body) {
        co_return;
    }
    fiber::http::HttpHeaders trailers(exchange.pool());
    trailers.set("digest", "test-trailer");
    (void) co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Trailer,
            .headers = &trailers,
            .end_stream = true,
    });
}

fiber::async::DetachedTask run_client(fiber::quic::QuicUdpEndpoint *endpoint,
                                      const fiber::net::SocketAddress *server_addr, const std::string *cert_path,
                                      std::promise<ClientObservation> *promise) {
    ClientObservation observation{};
    fiber::http::Http3Client::Options client_options{};
    client_options.tls.ca_file = *cert_path;
    client_options.verify_peer = true;
    fiber::http::Http3Client client(*endpoint, std::move(client_options));

    auto endpoint_started = endpoint->start();
    if (!endpoint_started) {
        observation.error = endpoint_started.error();
        promise->set_value(std::move(observation));
        co_return;
    }
    auto initialized = client.init();
    if (!initialized) {
        observation.error = initialized.error();
        promise->set_value(std::move(observation));
        co_return;
    }

    fiber::quic::QuicClientConnectOptions connect_options{};
    connect_options.remote_addr = *server_addr;
    connect_options.server_name = "localhost";
    connect_options.handshake_timeout = 2s;
    auto connected = co_await client.connect(std::move(connect_options));
    if (!connected) {
        observation.error = connected.error().io_error;
        observation.connect_phase = connected.error().phase;
        promise->set_value(std::move(observation));
        co_return;
    }

    {
        fiber::mem::BufPool pool;
        fiber::http::HttpHeaders headers(pool);
        headers.set("content-length", "17");
        fiber::http::ClientHttp3Exchange exchange = connected->open_exchange(pool);
        fiber::http::Http3RequestHead head{
                .method = fiber::http::HttpMethod::Post,
                .scheme = "https",
                .authority = "localhost",
                .path = "/http3/echo",
                .headers = &headers,
        };
        auto sent_head = co_await exchange.send_request_header(head, false, 2s);
        if (!sent_head) {
            observation.error = sent_head.error();
        } else {
            constexpr std::string_view kBody = "http3-client-body";
            auto sent_body = co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(kBody.data()),
                                                         kBody.size(), false, 2s);
            if (!sent_body) {
                observation.error = sent_body.error();
            } else {
                fiber::http::HttpHeaders trailers(pool);
                trailers.set("digest", "request-trailer");
                auto sent_trailer = co_await exchange.write_trailer(trailers, 2s);
                if (!sent_trailer) {
                    observation.error = sent_trailer.error();
                }
            }
        }

        if (observation.error == fiber::common::IoErr::None) {
            for (;;) {
                auto response_head = co_await exchange.read_header(2s);
                if (!response_head || *response_head == nullptr) {
                    observation.error = response_head ? fiber::common::IoErr::Invalid : response_head.error();
                    break;
                }
                if ((*response_head)->kind == fiber::http::OutgoingHeaderKind::Informational) {
                    observation.informational_status = (*response_head)->status_code;
                    continue;
                }
                observation.status = (*response_head)->status_code;
                break;
            }
        }
        while (observation.error == fiber::common::IoErr::None && !observation.body_complete) {
            auto chunk = co_await exchange.read_body(64 * 1024, 2s);
            if (!chunk) {
                observation.error = chunk.error();
                break;
            }
            observation.body_complete = chunk->complete();
            observation.body.append(chain_to_string(std::move(*chunk)));
        }
        observation.outcome = exchange.outcome();
        if (observation.error == fiber::common::IoErr::None) {
            auto trailer = co_await exchange.read_header(2s);
            if (!trailer || *trailer == nullptr || (*trailer)->kind != fiber::http::OutgoingHeaderKind::Trailer) {
                observation.error = trailer ? fiber::common::IoErr::Invalid : trailer.error();
            } else {
                observation.trailer.assign((*trailer)->headers.get("digest"));
                observation.trailer_end_stream = (*trailer)->end_stream;
            }
        }

        fiber::http::ClientHttp3Exchange head_exchange = connected->open_exchange(pool);
        fiber::http::Http3RequestHead head_request{
                .method = fiber::http::HttpMethod::Head,
                .scheme = "https",
                .authority = "localhost",
                .path = "/http3/head",
        };
        auto sent_head_request = co_await head_exchange.send_request_header(head_request, true, 2s);
        if (!sent_head_request) {
            observation.error = sent_head_request.error();
        }
        if (observation.error == fiber::common::IoErr::None) {
            auto response_head = co_await head_exchange.read_header(2s);
            if (!response_head || *response_head == nullptr) {
                observation.error = response_head ? fiber::common::IoErr::Invalid : response_head.error();
            } else {
                observation.head_status = (*response_head)->status_code;
            }
        }
        if (observation.error == fiber::common::IoErr::None) {
            auto head_body = co_await head_exchange.read_body(64 * 1024, 2s);
            if (!head_body) {
                observation.error = head_body.error();
            } else {
                observation.head_body_complete = head_body->complete();
                if (head_body->readable_bytes() != 0) {
                    observation.error = fiber::common::IoErr::Invalid;
                }
            }
        }
    }

    connected->shutdown(fiber::http::Http3ErrorCode::NoError);
    *connected = fiber::http::Http3ClientConnection{};
    endpoint->close();
    promise->set_value(std::move(observation));
}

fiber::async::DetachedTask run_nginx_client(fiber::quic::QuicUdpEndpoint *endpoint,
                                            const fiber::net::SocketAddress *server_addr,
                                            std::promise<ClientObservation> *promise) {
    ClientObservation observation{};
    fiber::http::Http3Client::Options client_options{};
    client_options.verify_peer = false;
    fiber::http::Http3Client client(*endpoint, std::move(client_options));

    auto endpoint_started = endpoint->start();
    if (!endpoint_started) {
        observation.error = endpoint_started.error();
        promise->set_value(std::move(observation));
        co_return;
    }
    auto initialized = client.init();
    if (!initialized) {
        observation.error = initialized.error();
        promise->set_value(std::move(observation));
        co_return;
    }

    fiber::quic::QuicClientConnectOptions connect_options{};
    connect_options.remote_addr = *server_addr;
    connect_options.server_name = "localhost";
    connect_options.handshake_timeout = 2s;
    connect_options.allow_insecure = true;
    auto connected = co_await client.connect(std::move(connect_options));
    if (!connected) {
        observation.error = connected.error().io_error;
        observation.connect_phase = connected.error().phase;
        promise->set_value(std::move(observation));
        co_return;
    }

    {
        fiber::mem::BufPool pool;
        fiber::http::ClientHttp3Exchange exchange = connected->open_exchange(pool);
        fiber::http::Http3RequestHead head{
                .method = fiber::http::HttpMethod::Get,
                .scheme = "https",
                .authority = "localhost",
                .path = "/",
        };
        auto sent_head = co_await exchange.send_request_header(head, true, 2s);
        if (!sent_head) {
            observation.error = sent_head.error();
        }
        if (observation.error == fiber::common::IoErr::None) {
            auto response_head = co_await exchange.read_header(2s);
            if (!response_head || *response_head == nullptr) {
                observation.error = response_head ? fiber::common::IoErr::Invalid : response_head.error();
            } else {
                observation.status = (*response_head)->status_code;
            }
        }
        while (observation.error == fiber::common::IoErr::None && !observation.body_complete) {
            auto chunk = co_await exchange.read_body(64 * 1024, 2s);
            if (!chunk) {
                observation.error = chunk.error();
                break;
            }
            observation.body_complete = chunk->complete();
            observation.body.append(chain_to_string(std::move(*chunk)));
        }
        observation.outcome = exchange.outcome();
    }

    connected->shutdown(fiber::http::Http3ErrorCode::NoError);
    *connected = fiber::http::Http3ClientConnection{};
    endpoint->close();
    promise->set_value(std::move(observation));
}

fiber::async::DetachedTask run_partial_client(fiber::quic::QuicUdpEndpoint *endpoint,
                                              const fiber::net::SocketAddress *server_addr,
                                              const std::string *cert_path,
                                              std::promise<PartialClientObservation> *promise) {
    PartialClientObservation observation{};
    fiber::http::Http3Client::Options client_options{};
    client_options.tls.ca_file = *cert_path;
    client_options.verify_peer = true;
    fiber::http::Http3Client client(*endpoint, std::move(client_options));

    auto endpoint_started = endpoint->start();
    if (!endpoint_started) {
        observation.error = endpoint_started.error();
        promise->set_value(std::move(observation));
        co_return;
    }
    auto initialized = client.init();
    if (!initialized) {
        observation.error = initialized.error();
        promise->set_value(std::move(observation));
        co_return;
    }

    fiber::quic::QuicClientConnectOptions connect_options{};
    connect_options.remote_addr = *server_addr;
    connect_options.server_name = "localhost";
    connect_options.handshake_timeout = 2s;
    auto connected = co_await client.connect(std::move(connect_options));
    if (!connected) {
        observation.error = connected.error().io_error;
        promise->set_value(std::move(observation));
        co_return;
    }

    {
        constexpr std::size_t kBodySize = 2048;
        std::string body(kBodySize, '\0');
        for (std::size_t i = 0; i < body.size(); ++i) {
            body[i] = static_cast<char>('a' + i % 26);
        }

        fiber::mem::BufPool pool;
        fiber::http::HttpHeaders headers(pool);
        const std::string content_length = std::to_string(body.size());
        headers.set("content-length", content_length);
        fiber::http::ClientHttp3Exchange exchange = connected->open_exchange(pool);
        fiber::http::Http3RequestHead head{
                .method = fiber::http::HttpMethod::Post,
                .scheme = "https",
                .authority = "localhost",
                .path = "/http3/partial",
                .headers = &headers,
        };
        auto sent_head = co_await exchange.send_request_header(head, false, 2s);
        if (!sent_head) {
            observation.error = sent_head.error();
        } else {
            const auto *data = reinterpret_cast<const std::uint8_t *>(body.data());
            std::size_t remaining = body.size();
            while (remaining != 0) {
                auto written = co_await exchange.write(data + observation.total_written, remaining, true, 2s);
                ++observation.write_calls;
                if (!written) {
                    observation.error = written.error();
                    break;
                }
                if (observation.write_calls == 1) {
                    observation.first_written = *written;
                }
                observation.total_written += *written;
                remaining -= *written;
            }
        }

        while (observation.error == fiber::common::IoErr::None && observation.status == 0) {
            auto response_head = co_await exchange.read_header(2s);
            if (!response_head || *response_head == nullptr) {
                observation.error = response_head ? fiber::common::IoErr::Invalid : response_head.error();
                break;
            }
            if ((*response_head)->kind == fiber::http::OutgoingHeaderKind::Final) {
                observation.status = (*response_head)->status_code;
            }
        }
        while (observation.error == fiber::common::IoErr::None && !observation.response_complete) {
            auto chunk = co_await exchange.read_body(64 * 1024, 2s);
            if (!chunk) {
                observation.error = chunk.error();
                break;
            }
            observation.response_complete = chunk->complete();
            observation.response_body.append(chain_to_string(std::move(*chunk)));
        }
    }

    connected->shutdown(fiber::http::Http3ErrorCode::NoError);
    *connected = fiber::http::Http3ClientConnection{};
    endpoint->close();
    promise->set_value(std::move(observation));
}

} // namespace

TEST(Http3ClientTest, RoundTripsStreamingRequestAndResponse) {
    fiber::test::QuicTestTlsFile cert("h3-client-cert", fiber::test::kQuicTestCertificatePem);
    fiber::test::QuicTestTlsFile key("h3-client-key", fiber::test::kQuicTestPrivateKeyPem);
    ASSERT_TRUE(cert.valid());
    ASSERT_TRUE(key.valid());

    fiber::event::EventLoopGroup group(1);
    group.start();

    auto server_promise = std::make_shared<std::promise<ServerObservation>>();
    auto server_future = server_promise->get_future();
    fiber::http::HttpServerOptions server_options{};
    server_options.tls.enabled = true;
    server_options.tls.cert_file = cert.path();
    server_options.tls.key_file = key.path();
    server_options.http3.enabled = true;
    fiber::http::HttpHandler handler = [server_promise](fiber::http::HttpExchange &exchange) {
        return echo_handler(exchange, server_promise);
    };
    fiber::http::Http3Server server(group.at(0), std::move(handler), std::move(server_options));
    ASSERT_TRUE(server.bind({fiber::net::IpAddress::loopback_v4(), 0}));
    server.serve();

    fiber::quic::QuicUdpEndpoint client_endpoint;
    fiber::quic::QuicUdpEndpoint::EndpointOptions endpoint_options{};
    endpoint_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    ASSERT_TRUE(client_endpoint.init(group.at(0), endpoint_options));

    std::promise<ClientObservation> client_promise;
    auto client_future = client_promise.get_future();
    const fiber::net::SocketAddress server_addr = server.local_addr();
    const std::string cert_path = cert.path();
    fiber::async::spawn(group.at(0),
                        [&]() { return run_client(&client_endpoint, &server_addr, &cert_path, &client_promise); });

    ASSERT_EQ(client_future.wait_for(5s), std::future_status::ready);
    ASSERT_EQ(server_future.wait_for(5s), std::future_status::ready);
    ClientObservation client = client_future.get();
    ServerObservation observed = server_future.get();

    EXPECT_EQ(client.error, fiber::common::IoErr::None);
    EXPECT_EQ(client.informational_status, 103);
    EXPECT_EQ(client.status, 200);
    EXPECT_EQ(client.head_status, 200);
    EXPECT_EQ(client.body, "http3-client-body");
    EXPECT_EQ(client.trailer, "test-trailer");
    EXPECT_TRUE(client.body_complete);
    EXPECT_TRUE(client.head_body_complete);
    EXPECT_TRUE(client.trailer_end_stream);
    EXPECT_EQ(client.outcome, fiber::http::Http3RequestOutcome::Complete);

    EXPECT_EQ(observed.error, fiber::common::IoErr::None);
    EXPECT_EQ(observed.version, fiber::http::HttpVersion::HTTP_3_0);
    EXPECT_EQ(observed.path, "/http3/echo");
    EXPECT_EQ(observed.host, "localhost");
    EXPECT_EQ(observed.body, "http3-client-body");
    EXPECT_EQ(observed.trailer, "request-trailer");

    std::promise<void> close_promise;
    auto close_future = close_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        co_await server.shutdown_and_wait();
        close_promise.set_value();
        co_return;
    });
    close_future.get();
    group.stop();
    group.join();
}

TEST(Http3ClientTest, PartialWriteContinuesDataFrameWithoutRepeatingHeader) {
    fiber::test::QuicTestTlsFile cert("h3-client-partial-cert", fiber::test::kQuicTestCertificatePem);
    fiber::test::QuicTestTlsFile key("h3-client-partial-key", fiber::test::kQuicTestPrivateKeyPem);
    ASSERT_TRUE(cert.valid());
    ASSERT_TRUE(key.valid());

    fiber::event::EventLoopGroup group(1);
    group.start();

    auto server_promise = std::make_shared<std::promise<ServerObservation>>();
    auto server_future = server_promise->get_future();
    fiber::http::HttpServerOptions server_options{};
    server_options.tls.enabled = true;
    server_options.tls.cert_file = cert.path();
    server_options.tls.key_file = key.path();
    server_options.http3.enabled = true;
    server_options.http3.recv_flow.stream_buffer_limit = 128;
    server_options.http3.recv_flow.stream_low_water = 64;
    fiber::http::HttpHandler handler = [server_promise](fiber::http::HttpExchange &exchange) {
        return echo_handler(exchange, server_promise);
    };
    fiber::http::Http3Server server(group.at(0), std::move(handler), std::move(server_options));
    ASSERT_TRUE(server.bind({fiber::net::IpAddress::loopback_v4(), 0}));
    server.serve();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::EndpointOptions endpoint_options{};
    endpoint_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    ASSERT_TRUE(endpoint.init(group.at(0), endpoint_options));
    std::promise<PartialClientObservation> client_promise;
    auto client_future = client_promise.get_future();
    const fiber::net::SocketAddress server_addr = server.local_addr();
    const std::string cert_path = cert.path();
    fiber::async::spawn(group.at(0),
                        [&]() { return run_partial_client(&endpoint, &server_addr, &cert_path, &client_promise); });

    ASSERT_EQ(client_future.wait_for(5s), std::future_status::ready);
    PartialClientObservation client_observation = client_future.get();
    ASSERT_EQ(server_future.wait_for(5s), std::future_status::ready);
    ServerObservation server_observation = server_future.get();

    EXPECT_EQ(client_observation.error, fiber::common::IoErr::None);
    EXPECT_GT(client_observation.first_written, 0U);
    EXPECT_LT(client_observation.first_written, 2048U);
    EXPECT_EQ(client_observation.total_written, 2048U);
    EXPECT_GT(client_observation.write_calls, 1U);
    EXPECT_EQ(client_observation.status, 200);
    EXPECT_TRUE(client_observation.response_complete);
    EXPECT_EQ(client_observation.response_body.size(), 2048U);

    EXPECT_EQ(server_observation.error, fiber::common::IoErr::None);
    EXPECT_EQ(server_observation.path, "/http3/partial");
    EXPECT_EQ(server_observation.body.size(), 2048U);
    EXPECT_EQ(server_observation.body, client_observation.response_body);

    std::promise<void> close_promise;
    auto close_future = close_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        co_await server.shutdown_and_wait();
        close_promise.set_value();
        co_return;
    });
    close_future.get();
    group.stop();
    group.join();
}

TEST(Http3ClientTest, NginxInterop) {
    const char *port_text = std::getenv("FIBER_HTTP3_NGINX_PORT");
    if (port_text == nullptr || port_text[0] == '\0') {
        GTEST_SKIP() << "set FIBER_HTTP3_NGINX_PORT for repository Nginx interoperability";
    }
    char *end = nullptr;
    const unsigned long port = std::strtoul(port_text, &end, 10);
    ASSERT_NE(end, port_text);
    ASSERT_EQ(*end, '\0');
    ASSERT_GT(port, 0U);
    ASSERT_LE(port, 65535U);

    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::EndpointOptions endpoint_options{};
    endpoint_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    ASSERT_TRUE(endpoint.init(group.at(0), endpoint_options));

    std::promise<ClientObservation> promise;
    auto future = promise.get_future();
    const fiber::net::SocketAddress server_addr{fiber::net::IpAddress::loopback_v4(), static_cast<std::uint16_t>(port)};
    fiber::async::spawn(group.at(0), [&]() { return run_nginx_client(&endpoint, &server_addr, &promise); });

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    ClientObservation observed = future.get();
    EXPECT_EQ(observed.error, fiber::common::IoErr::None);
    EXPECT_EQ(observed.status, 200);
    EXPECT_EQ(observed.body, "nginx debug backend is running\n");
    EXPECT_TRUE(observed.body_complete);
    EXPECT_EQ(observed.outcome, fiber::http::Http3RequestOutcome::Complete);

    group.stop();
    group.join();
}
