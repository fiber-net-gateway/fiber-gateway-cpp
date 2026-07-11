#ifndef FIBER_LITE_NGINX_UPSTREAM_CONNECTION_POOL_H
#define FIBER_LITE_NGINX_UPSTREAM_CONNECTION_POOL_H

#include <memory>
#include <variant>

#include "async/Task.h"
#include "common/IoError.h"
#include "common/NonCopyable.h"
#include "common/NonMovable.h"
#include "event/EventLoopGroup.h"
#include "http/Http1ClientConnection.h"
#include "http/Http1ConnectionGroupKey.h"
#include "http/LocalHttp1ConnectionPoolSet.h"
#include "http/StealableHttp1ConnectionPoolSet.h"

#include "../runtime/RuntimeConfig.h"

namespace fiber::lite_nginx::upstream {

// The global keepalive pool abstraction. Backed by either a per-loop LocalHttp1ConnectionPoolSet
// (steal=false: N independent per-loop pools, no cross-loop reuse) or a StealableHttp1ConnectionPoolSet
// (steal=true: one pool whose idle connections can be borrowed across worker loops). Both are keyed
// by Http1ConnectionGroupKey (peer identity = host/ip + port + scheme). The selection is fixed at
// construction from ConnectionPoolRuntime::steal.
//
// Callers drive the lifecycle: acquire(key) -> ConnectionLease; if !has_connection(), the caller
// connects a fresh Http1ClientConnection and emplaces it; destroying the lease returns the
// connection to its home pool.
class ConnectionPool : public fiber::common::NonCopyable, public fiber::common::NonMovable {
public:
    // Owns one checkout. Move-only; destroying it returns a pooled connection to its pool.
    class ConnectionLease {
    public:
        ConnectionLease() noexcept = default;
        ConnectionLease(ConnectionLease &&) noexcept;
        ConnectionLease &operator=(ConnectionLease &&) noexcept;
        ~ConnectionLease();

        [[nodiscard]] bool valid() const noexcept;
        // True when the lease carries an already-connected idle connection (a pool hit).
        [[nodiscard]] bool has_connection() const noexcept;
        [[nodiscard]] fiber::http::Http1ClientConnection *get() noexcept;
        // Create + insert a fresh connection into the pool slot. Only valid when !has_connection().
        // On Stealable, this lands on the Local fallback lease that acquire returned on a steal miss.
        [[nodiscard]] fiber::common::IoResult<fiber::http::Http1ClientConnection *>
        emplace_connection(fiber::http::Http1ClientConnectionOptions options) noexcept;
        void reset() noexcept;

    private:
        friend class ConnectionPool;
        explicit ConnectionLease(fiber::http::LocalHttp1ConnectionPoolSet::Lease local) noexcept;
        explicit ConnectionLease(fiber::http::StealableHttp1ConnectionPoolSet::Lease stealable) noexcept;

        enum class Kind : std::uint8_t { Empty, Local, Stealable };
        Kind kind_ = Kind::Empty;
        // Active member selected by kind_. The inactive member is default-constructed/empty.
        fiber::http::LocalHttp1ConnectionPoolSet::Lease local_{};
        fiber::http::StealableHttp1ConnectionPoolSet::Lease stealable_{};
    };

    ConnectionPool(fiber::event::EventLoopGroup &group,
                   const fiber::lite_nginx::runtime::ConnectionPoolRuntime &cp) noexcept;
    ~ConnectionPool();

    [[nodiscard]] bool init() noexcept;
    [[nodiscard]] fiber::async::Task<void> shutdown_async() noexcept;
    void shutdown() noexcept;

    // Acquire a lease for the peer key. The lease is a pool hit (has_connection) when an idle
    // connection exists; otherwise the caller must emplace+connect.
    [[nodiscard]] fiber::async::Task<ConnectionLease> acquire(const fiber::http::Http1ConnectionGroupKey &key) noexcept;

    [[nodiscard]] bool stealable() const noexcept {
        return std::holds_alternative<std::unique_ptr<fiber::http::StealableHttp1ConnectionPoolSet>>(impl_);
    }

private:
    fiber::event::EventLoopGroup *group_ = nullptr;
    std::variant<std::unique_ptr<fiber::http::LocalHttp1ConnectionPoolSet>,
                 std::unique_ptr<fiber::http::StealableHttp1ConnectionPoolSet>>
            impl_;
};

} // namespace fiber::lite_nginx::upstream

#endif // FIBER_LITE_NGINX_UPSTREAM_CONNECTION_POOL_H
