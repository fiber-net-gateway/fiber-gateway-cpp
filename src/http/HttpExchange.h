#ifndef FIBER_HTTP_HTTP_EXCHANGE_H
#define FIBER_HTTP_HTTP_EXCHANGE_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/BufPool.h"
#include "../common/mem/IoBufChain.h"
#include "../net/SocketAddress.h"
#include "../net/TcpSocketOptions.h"
#include "../net/TlsOptions.h"
#include "../net/UdpSocket.h"
#include "../quic/QuicConnection.h"
#include "../quic/QuicSendScheduler.h"
#include "Http3Protocol.h"
#include "HttpCommon.h"
#include "HttpExchangeIo.h"
#include "HttpHeaders.h"

namespace fiber::http {

struct HttpServerOptions {
    struct Http3Options {
        bool enabled = false;
        std::size_t max_connections_per_shard = 1024;
        net::UdpBindOptions udp{};
        quic::QuicSendScheduler::Options send{};
        quic::QuicTransportSettings transport{};
        quic::QuicRecvFlowControlSettings recv_flow{};
        Http3Settings settings{};
        std::chrono::milliseconds keepalive_interval{0};
        std::chrono::milliseconds max_ack_delay{25};
        std::uint64_t ack_delay_exponent = 3;
        bool retry = false;
        bool issue_new_token = false;
        bool enable_early_data = false;
    };

    net::TcpSocketOptions tcp{.no_delay = net::TcpOptionMode::Enabled};
    std::chrono::seconds keep_alive_timeout{70};
    std::chrono::seconds header_timeout{10};
    std::chrono::seconds body_timeout{60};
    std::chrono::seconds write_timeout{30};
    std::size_t header_init_size = 8 * 1024;
    std::size_t header_large_size = 32 * 1024;
    std::size_t header_large_num = 4;
    bool drain_unread_body = false;
    bool enable_extended_connect = false;
    net::TlsOptions tls{};
    Http3Options http3{};
};

class Http1Connection;
class Http1ExchangeIo;
class HttpTransport;
class RequestLineParser;
class HeaderLineParser;
class ServerHttp2Request;
class ServerHttp3Request;

struct HttpResponseStats {
    int status_code = 0;
    std::size_t body_bytes_sent = 0;
    common::IoErr terminal_error = common::IoErr::None;
    bool header_sent = false;
    bool completed = false;
};

class HttpExchange : public common::NonCopyable, public common::NonMovable {
public:
    struct RequestHeaderRefs {
        const HttpHeaders::HeaderField *host = nullptr;
        const HttpHeaders::HeaderField *content_type = nullptr;
        const HttpHeaders::HeaderField *range = nullptr;
        const HttpHeaders::HeaderField *if_range = nullptr;
        const HttpHeaders::HeaderField *expect = nullptr;
    };

    HttpExchange(mem::IoBufNodePool &node_pool, const HttpServerOptions &options, net::SocketAddress remote_addr);
    ~HttpExchange();

    [[nodiscard]] HttpMethod method() const noexcept { return method_; }
    [[nodiscard]] HttpVersion version() const noexcept { return version_; }
    [[nodiscard]] const HttpUri &uri() const noexcept { return uri_; }
    std::string_view version_view() const noexcept { return version_view_; }
    std::string_view method_view() const noexcept { return method_view_; }
    std::string_view scheme() const noexcept { return scheme_view_; }
    std::string_view protocol() const noexcept { return protocol_view_; }
    std::string_view header(std::string_view name) const noexcept;
    const RequestHeaderRefs &request_header_refs() const noexcept { return request_header_refs_; }
    const HttpHeaders::HeaderField *host_header() const noexcept { return request_header_refs_.host; }
    const HttpHeaders::HeaderField *content_type_header() const noexcept { return request_header_refs_.content_type; }
    const HttpHeaders::HeaderField *range_header() const noexcept { return request_header_refs_.range; }
    const HttpHeaders::HeaderField *if_range_header() const noexcept { return request_header_refs_.if_range; }
    const HttpHeaders::HeaderField *expect_header() const noexcept { return request_header_refs_.expect; }
    const HttpHeaders &request_headers() const noexcept { return request_headers_; };
    const HttpHeaders &request_trailers() const noexcept { return request_trailers_; };
    [[nodiscard]] HttpBodySpec request_body_spec() const noexcept { return request_body_spec_; }
    bool request_trailers_complete() const noexcept { return request_trailers_complete_; }
    mem::BufPool &pool() noexcept { return pool_; }
    [[nodiscard]] const net::SocketAddress &remote_addr() const noexcept { return remote_addr_; }
    [[nodiscard]] const HttpResponseStats &response_stats() const noexcept { return response_stats_; }

    fiber::async::Task<common::IoResult<mem::IoBufChain>>
    read_body(std::size_t max_bytes, std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    fiber::async::Task<common::IoResult<void>>
    discard_body(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;

    fiber::async::Task<common::IoResult<void>>
    send_header(const OutgoingHeaderBlockView &header,
                std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
    fiber::async::Task<common::IoResult<void>>
    send_continue_header(std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
    fiber::async::Task<common::IoResult<void>>
    send_informational_header(int status_code, const HttpHeaders *headers = nullptr,
                              std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
    fiber::async::Task<common::IoResult<size_t>>
    write_body(mem::IoBufChain chunk, std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    fiber::async::Task<common::IoResult<size_t>>
    write_body(const uint8_t *buf, size_t len, bool end,
               std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    common::IoResult<void> abort(common::IoErr reason = common::IoErr::Canceled) noexcept;


private:
    void set_io(HttpExchangeIo *io) noexcept;
    void cache_request_header_field(const HttpHeaders::HeaderField &field) noexcept;
    void record_io_error(common::IoErr error) noexcept;

    friend class RequestLineParser;
    friend class HeaderLineParser;
    friend class Http1Connection;
    friend class Http1ExchangeIo;
    friend class ServerHttp2Request;
    friend class ServerHttp3Request;

    fiber::mem::BufPool pool_;
    fiber::mem::IoBufChain header_bufs_;
    fiber::mem::IoBufChain trailer_bufs_;
    HttpMethod method_{};
    HttpVersion version_{};
    HttpUri uri_;
    std::string_view method_view_;
    std::string_view version_view_;
    std::string_view scheme_view_;
    std::string_view protocol_view_;
    HttpHeaders request_headers_;
    HttpHeaders request_trailers_;
    bool request_trailers_complete_ = false;
    RequestHeaderRefs request_header_refs_;
    HttpBodySpec request_body_spec_{HttpBodySpec::None()};
    net::SocketAddress remote_addr_{};
    HttpResponseStats response_stats_{};
    bool request_close_ = false;
    bool request_keep_alive_ = false;
    HttpExchangeIo *io_ = nullptr;
};

using HttpHandler = std::function<fiber::async::Task<void>(HttpExchange &)>;

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP_EXCHANGE_H
