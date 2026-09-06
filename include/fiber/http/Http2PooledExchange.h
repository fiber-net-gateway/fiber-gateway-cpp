#ifndef FIBER_HTTP_HTTP2_POOLED_EXCHANGE_H
#define FIBER_HTTP_HTTP2_POOLED_EXCHANGE_H

#include <fiber/http/Http2ConnectionPoolCore.h>

namespace fiber::http {

// Owns one exchange and its admission slot. Declare the lease first so the
// exchange always releases its stream before the slot goes back to the pool.
class Http2PooledExchange : public common::NonCopyable {
public:
    Http2PooledExchange() noexcept = default;
    Http2PooledExchange(Http2ConnectionPoolCore::Lease lease, mem::BufPool &pool) noexcept;
    Http2PooledExchange(Http2PooledExchange &&other) noexcept;
    Http2PooledExchange &operator=(Http2PooledExchange &&other) noexcept;
    ~Http2PooledExchange();
    [[nodiscard]] bool valid() const noexcept { return lease_.valid(); }
    [[nodiscard]] ClientHttp2Exchange &exchange() noexcept { return ex_; }
    [[nodiscard]] ClientHttp2Exchange *operator->() noexcept { return &ex_; }
    void reset() noexcept;

private:
    Http2ConnectionPoolCore::Lease lease_{};
    ClientHttp2Exchange ex_{};
};

} // namespace fiber::http
#endif // FIBER_HTTP_HTTP2_POOLED_EXCHANGE_H
