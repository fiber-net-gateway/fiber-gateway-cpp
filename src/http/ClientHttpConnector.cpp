#include <fiber/http/ClientHttpConnector.h>

#include <memory>
#include <string_view>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/http/HttpTransport.h>

namespace fiber::http {

namespace {

constexpr std::string_view kHttp1Alpn = "http/1.1";
constexpr std::string_view kHttp2Alpn = "h2";
constexpr std::string_view kNegotiateAlpnList[] = {kHttp2Alpn, kHttp1Alpn};
constexpr std::string_view kHttp1AlpnList[] = {kHttp1Alpn};
constexpr std::string_view kHttp2AlpnList[] = {kHttp2Alpn};

[[nodiscard]] bool allows_http1(HttpProtocolPreference preference) noexcept {
    return preference != HttpProtocolPreference::Http2Only;
}

[[nodiscard]] bool allows_http2(HttpProtocolPreference preference) noexcept {
    return preference != HttpProtocolPreference::Http1Only;
}

// Hands an already-established transport to the connection the HTTP/2 pool allocated. This reuses
// the pool's existing dial inversion instead of teaching it to adopt connections: the pool still
// decides whether a new connection is wanted, and only calls back when it is.
struct AdoptingHttp2Connector {
    std::unique_ptr<HttpTransport> transport{};
    std::optional<net::SocketAddress> local{};
    bool consumed = false;

    static async::Task<common::IoResult<void>> connect(void *ctx, Http2ClientConnection &conn,
                                                       const HttpConnectionGroupKey &) noexcept {
        auto *self = static_cast<AdoptingHttp2Connector *>(ctx);
        FIBER_ASSERT(self != nullptr);
        // One transport, one connection: a pool that asks twice gets a failure for the second ask
        // rather than an empty adopt.
        if (self->consumed || !self->transport) {
            co_return std::unexpected(common::IoErr::Canceled);
        }
        self->consumed = true;
        const common::IoErr error = conn.adopt(std::move(self->transport), std::move(self->local));
        if (error != common::IoErr::None) {
            co_return std::unexpected(error);
        }
        co_return common::IoResult<void>{};
    }

    // The pool may satisfy an acquire from capacity that appeared while we were dialing, leaving
    // the transport untouched. Nothing else owns it at that point, so close it here.
    void discard_unused() noexcept {
        if (!consumed && transport) {
            transport->close();
            transport.reset();
        }
    }
};

} // namespace

PooledClientHttpExchange::~PooledClientHttpExchange() { reset(); }

ClientHttpExchange PooledClientHttpExchange::exchange() noexcept {
    if (http1_exchange_) {
        return ClientHttpExchange(*http1_exchange_);
    }
    if (http2_exchange_) {
        return ClientHttpExchange(*http2_exchange_);
    }
    return {};
}

void PooledClientHttpExchange::reset() noexcept {
    // Release in the order the pools expect: the exchange gives back its stream or marks the
    // connection reusable, and only then does the lease return the slot.
    http1_exchange_.reset();
    http2_exchange_.reset();
    http1_lease_.reset();
    http2_lease_.reset();
    http1_transient_.reset();
    protocol_ = HttpProtocol::Http1;
    reused_ = false;
}

async::Task<common::IoResult<void>> ClientHttpConnector::resolve_targets(const HttpConnectionGroupKey &key,
                                                                         DialTargets &out) noexcept {
    out.count = 0;
    if (key.is_ip()) {
        out.storage[0] = net::SocketAddress(key.ip_address(), key.port());
        out.count = 1;
        co_return common::IoResult<void>{};
    }
    if (resolver_.resolve == nullptr) {
        co_return std::unexpected(common::IoErr::NotSupported);
    }

    auto resolved = co_await resolver_.resolve(resolver_.ctx, key, std::span<net::SocketAddress>(out.storage));
    if (!resolved) {
        co_return std::unexpected(resolved.error());
    }
    if (*resolved == 0) {
        co_return std::unexpected(common::IoErr::NotFound);
    }
    out.count = *resolved > out.storage.size() ? out.storage.size() : *resolved;
    // A resolver that answers with names still owes us the port; the key is the only authority on
    // it, so stamp it rather than trusting whatever came back.
    for (std::size_t i = 0; i < out.count; ++i) {
        out.storage[i] = net::SocketAddress(out.storage[i].ip(), key.port());
    }
    co_return common::IoResult<void>{};
}

common::IoResult<void> ClientHttpConnector::attach_http1(PooledClientHttpExchange &out, mem::BufPool &pool,
                                                         bool reused) noexcept {
    Http1ClientConnection *conn = out.http1_transient_ ? out.http1_transient_.get() : out.http1_lease_.get();
    if (conn == nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }
    out.protocol_ = HttpProtocol::Http1;
    out.reused_ = reused;
    out.http1_exchange_.emplace(*conn, pool);
    return {};
}

void ClientHttpConnector::attach_http2(PooledClientHttpExchange &out, Http2ConnectionPoolCore::Lease lease,
                                       mem::BufPool &pool, bool reused) noexcept {
    out.protocol_ = HttpProtocol::Http2;
    out.reused_ = reused;
    out.http2_lease_ = std::move(lease);
    out.http2_exchange_.emplace(out.http2_lease_.open_exchange(pool));
}

async::Task<common::IoResult<void>> ClientHttpConnector::acquire(const HttpConnectionGroupKey &key, mem::BufPool &pool,
                                                                 const HttpClientAcquireOptions &options,
                                                                 PooledClientHttpExchange &out) noexcept {
    FIBER_ASSERT(http1_ != nullptr);
    FIBER_ASSERT(http2_ != nullptr);
    out.reset();

    const bool secure = key.scheme() == HttpConnectionGroupKey::Scheme::Https;
    if (secure && options.tls == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    // Cleartext has no ALPN to negotiate with, and h2c Upgrade is deprecated, so Auto can only
    // mean HTTP/1 there. Http2Only still works in the clear: that is prior knowledge, not
    // negotiation.
    HttpProtocolPreference preference = options.preference;
    if (!secure && preference == HttpProtocolPreference::Auto) {
        preference = HttpProtocolPreference::Http1Only;
    }

    const auto now = event::EventLoop::current().now();
    const std::optional<HttpProtocol> hint = hints_.lookup(key, now);

    // Reuse first, cheapest path first. try_acquire never dials, waits, or creates a group, so a
    // miss costs nothing beyond the lookup.
    if (allows_http2(preference) && (preference == HttpProtocolPreference::Http2Only || hint == HttpProtocol::Http2)) {
        if (auto lease = http2_->try_acquire(key)) {
            attach_http2(out, std::move(*lease), pool, true);
            co_return common::IoResult<void>{};
        }
    }

    // An HTTP/1 lease doubles as the slot a fresh HTTP/1 connection is dialed into, so hold on to
    // it through the dial rather than taking it twice.
    if (allows_http1(preference) && hint != HttpProtocol::Http2) {
        out.http1_lease_ = co_await http1_->acquire(key);
        if (out.http1_lease_.valid() && out.http1_lease_.has_connection()) {
            co_return attach_http1(out, pool, true);
        }
    }

    DialTargets targets;
    auto resolved = co_await resolve_targets(key, targets);
    if (!resolved) {
        out.reset();
        co_return std::unexpected(resolved.error());
    }

    HttpClientDialRequest request;
    request.addresses = targets.span();
    request.happy = options.happy;
    request.tcp = options.tcp;
    if (secure) {
        request.tls = options.tls;
        switch (preference) {
            case HttpProtocolPreference::Http1Only:
                request.alpn = kHttp1AlpnList;
                break;
            case HttpProtocolPreference::Http2Only:
                request.alpn = kHttp2AlpnList;
                break;
            case HttpProtocolPreference::Auto:
                request.alpn = kNegotiateAlpnList;
                break;
        }
    }
    request.default_protocol =
            preference == HttpProtocolPreference::Http2Only ? HttpProtocol::Http2 : HttpProtocol::Http1;
    request.need_local_addr = allows_http2(preference);

    auto dial = co_await http_client_dial(event::EventLoop::current(), std::move(request));
    if (!dial) {
        out.reset();
        co_return std::unexpected(dial.error());
    }

    if (dial->protocol == HttpProtocol::Http3) {
        // Nothing offers "h3" over TCP, so reaching here means the peer answered outside the set
        // it was given. Reject rather than hand a QUIC-only protocol a stream transport.
        dial->transport->close();
        out.reset();
        co_return std::unexpected(common::IoErr::NotSupported);
    }

    if (dial->protocol == HttpProtocol::Http2) {
        if (!allows_http2(preference)) {
            dial->transport->close();
            out.reset();
            co_return std::unexpected(common::IoErr::NotSupported);
        }
        // The HTTP/1 slot is not part of this outcome; give it back before waiting on the HTTP/2
        // pool so it cannot sit idle behind an admission wait.
        out.http1_lease_.reset();

        AdoptingHttp2Connector adopting{.transport = std::move(dial->transport), .local = std::move(dial->local)};
        Http2ConnectionPoolCore::Connector connector{.connect = &AdoptingHttp2Connector::connect, .ctx = &adopting};
        auto lease = co_await http2_->acquire(key, connector, options.pool_timeout);
        adopting.discard_unused();
        if (!lease) {
            out.reset();
            co_return std::unexpected(lease.error());
        }
        hints_.note(key, HttpProtocol::Http2, event::EventLoop::current().now());
        attach_http2(out, std::move(*lease), pool, !adopting.consumed);
        co_return common::IoResult<void>{};
    }

    if (!allows_http1(preference)) {
        dial->transport->close();
        out.reset();
        co_return std::unexpected(common::IoErr::NotSupported);
    }

    // The hint may have sent us past the HTTP/1 pool; take a slot now that the peer has settled it.
    if (!out.http1_lease_.valid()) {
        out.http1_lease_ = co_await http1_->acquire(key);
        if (out.http1_lease_.valid() && out.http1_lease_.has_connection()) {
            // Someone else finished an HTTP/1 connection while we dialed. Use theirs and drop this
            // one rather than displacing a warm connection.
            dial->transport->close();
            hints_.note(key, HttpProtocol::Http1, event::EventLoop::current().now());
            co_return attach_http1(out, pool, true);
        }
    }

    Http1ClientConnection *conn = nullptr;
    if (out.http1_lease_.valid()) {
        auto emplaced = out.http1_lease_.emplace_connection();
        if (!emplaced) {
            dial->transport->close();
            out.reset();
            co_return std::unexpected(emplaced.error());
        }
        conn = *emplaced;
    } else {
        // No pool slot available (a shut-down or full pool). The request still deserves to go out,
        // so run it on a connection this checkout owns outright.
        out.http1_transient_ = std::make_unique<Http1ClientConnection>(event::EventLoop::current());
        if (!out.http1_transient_) {
            dial->transport->close();
            out.reset();
            co_return std::unexpected(common::IoErr::NoMem);
        }
        conn = out.http1_transient_.get();
    }

    const common::IoErr adopt_error = conn->adopt(std::move(dial->transport), std::move(dial->peer));
    if (adopt_error != common::IoErr::None) {
        out.reset();
        co_return std::unexpected(adopt_error);
    }
    hints_.note(key, HttpProtocol::Http1, event::EventLoop::current().now());
    co_return attach_http1(out, pool, false);
}

} // namespace fiber::http
