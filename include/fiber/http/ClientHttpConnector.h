#ifndef FIBER_HTTP_CLIENT_HTTP_CONNECTOR_H
#define FIBER_HTTP_CLIENT_HTTP_CONNECTOR_H

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/BufPool.h"
#include "../net/HappyEyeballs.h"
#include "../net/SocketAddress.h"
#include "../net/TcpSocketOptions.h"
#include "ClientHttpExchange.h"
#include "HttpAlpnHintTable.h"
#include "HttpClientDialer.h"
#include "HttpClientTlsOptions.h"
#include "HttpConnectionGroupKey.h"
#include "LocalHttp2ConnectionPoolSet.h"
#include "StealableHttp1ConnectionPoolSet.h"

namespace fiber::http {

enum class HttpProtocolPreference : std::uint8_t {
    Http1Only,
    // Over TLS this offers only "h2"; in the clear it dials prior-knowledge HTTP/2, since the
    // HTTP/1 Upgrade path to h2c is deprecated and not implemented.
    Http2Only,
    // Over TLS this offers {"h2", "http/1.1"} and follows whatever the peer selects. In the clear
    // there is nothing to negotiate with, so it behaves as Http1Only.
    Auto,
};

// Turns a group key into dial targets. HTTP/2 and HTTP/1 both need this, and DNS lives outside
// this library, so name-keyed origins are resolved through a caller-supplied callback. Leaving it
// unset still works for IP-keyed origins, which need no resolution at all.
struct HttpClientAddressResolver {
    // Fills `out` with at most out.size() targets for `key` and returns how many it wrote.
    // Borrowed for the acquire's duration, and must tolerate coroutine destruction while
    // suspended (caller cancellation or a deadline).
    async::Task<common::IoResult<std::size_t>> (*resolve)(void *ctx, const HttpConnectionGroupKey &key,
                                                          std::span<net::SocketAddress> out) noexcept = nullptr;
    void *ctx = nullptr;
};

struct HttpClientAcquireOptions {
    HttpProtocolPreference preference = HttpProtocolPreference::Auto;
    // Required for an Https key and ignored for an Http one. Borrowed for the acquire's duration,
    // like every other view in HttpClientTlsOptions.
    const HttpClientTlsOptions *tls = nullptr;
    net::HappyEyeballsOptions happy{};
    net::TcpSocketOptions tcp = net::kNoDelayTcpSocketOptions;
    // Budget for the HTTP/2 pool's admission wait. The TCP phase is bounded by
    // `happy.total_timeout` and the TLS handshake by HttpClientTlsOptions::handshake_timeout.
    std::chrono::milliseconds pool_timeout{std::chrono::milliseconds::max()};
};

// One request's worth of pooled capacity: an HTTP/1 connection checkout, or one HTTP/2 stream
// slot, plus the exchange that runs on it.
//
// Non-movable, because ClientHttp1Exchange is: ClientHttpConnector::acquire fills one the caller
// declared instead of returning it. Destroying it releases the exchange first and the pool
// capacity second.
class PooledClientHttpExchange : public common::NonCopyable, public common::NonMovable {
public:
    PooledClientHttpExchange() noexcept = default;
    ~PooledClientHttpExchange();

    [[nodiscard]] bool valid() const noexcept { return http1_exchange_.has_value() || http2_exchange_.has_value(); }
    [[nodiscard]] HttpProtocol protocol() const noexcept { return protocol_; }
    // Empty until a successful acquire. The handle borrows storage owned here, so it must not
    // outlive this object.
    [[nodiscard]] ClientHttpExchange exchange() noexcept;
    // True when the request went out on a connection the pool already had, rather than a fresh
    // dial. Diagnostic only.
    [[nodiscard]] bool reused() const noexcept { return reused_; }
    void reset() noexcept;

private:
    friend class ClientHttpConnector;

    // Leases are declared before the exchanges so each exchange is destroyed first and gives back
    // its stream or connection before the slot returns to the pool.
    StealableHttp1ConnectionPoolSet::Lease http1_lease_{};
    Http2ConnectionPoolCore::Lease http2_lease_{};
    std::optional<ClientHttp1Exchange> http1_exchange_{};
    std::optional<ClientHttp2Exchange> http2_exchange_{};
    // Owned only when the HTTP/1 pool had no slot to give; normally the connection lives in the
    // pool and is reached through http1_lease_.
    std::unique_ptr<Http1ClientConnection> http1_transient_{};
    HttpProtocol protocol_ = HttpProtocol::Http1;
    bool reused_ = false;
};

// Picks the HTTP version for an origin and hands back pooled capacity on it.
//
// The negotiation it exists for cannot be expressed inside either pool: the protocol is only known
// after the TLS handshake, so a miss dials here and then donates the established transport to
// whichever pool matches the ALPN the peer chose. Repeat requests skip that by consulting the
// per-origin protocol hint first.
//
// Single-threaded: construct one per worker loop. The pool sets it borrows are shared and resolve
// their own per-loop state internally, but the hint table here is not synchronised.
class ClientHttpConnector : public common::NonCopyable, public common::NonMovable {
public:
    ClientHttpConnector(StealableHttp1ConnectionPoolSet &http1, LocalHttp2ConnectionPoolSet &http2) noexcept :
        http1_(&http1), http2_(&http2) {}
    ClientHttpConnector(StealableHttp1ConnectionPoolSet &http1, LocalHttp2ConnectionPoolSet &http2,
                        HttpClientAddressResolver resolver) noexcept :
        http1_(&http1), http2_(&http2), resolver_(resolver) {}

    // Fills `out` with capacity for one request to `key`. `pool` backs the exchange's headers and
    // must outlive `out`. Options and the storage behind them are borrowed until the task
    // completes.
    [[nodiscard]] async::Task<common::IoResult<void>> acquire(const HttpConnectionGroupKey &key, mem::BufPool &pool,
                                                              const HttpClientAcquireOptions &options,
                                                              PooledClientHttpExchange &out) noexcept;

    void set_address_resolver(HttpClientAddressResolver resolver) noexcept { resolver_ = resolver; }
    [[nodiscard]] HttpAlpnHintTable &hints() noexcept { return hints_; }
    [[nodiscard]] const HttpAlpnHintTable &hints() const noexcept { return hints_; }

private:
    struct DialTargets {
        std::array<net::SocketAddress, net::kHappyEyeballsMaxAddresses> storage{};
        std::size_t count = 0;

        [[nodiscard]] std::span<const net::SocketAddress> span() const noexcept {
            return std::span<const net::SocketAddress>(storage.data(), count);
        }
    };

    [[nodiscard]] async::Task<common::IoResult<void>> resolve_targets(const HttpConnectionGroupKey &key,
                                                                      DialTargets &out) noexcept;
    [[nodiscard]] common::IoResult<void> attach_http1(PooledClientHttpExchange &out, mem::BufPool &pool,
                                                      bool reused) noexcept;
    void attach_http2(PooledClientHttpExchange &out, Http2ConnectionPoolCore::Lease lease, mem::BufPool &pool,
                      bool reused) noexcept;

    StealableHttp1ConnectionPoolSet *http1_ = nullptr;
    LocalHttp2ConnectionPoolSet *http2_ = nullptr;
    HttpClientAddressResolver resolver_{};
    HttpAlpnHintTable hints_{};
};

} // namespace fiber::http

#endif // FIBER_HTTP_CLIENT_HTTP_CONNECTOR_H
