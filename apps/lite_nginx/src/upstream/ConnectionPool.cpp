#include "ConnectionPool.h"

#include <future>
#include <utility>

#include "async/Spawn.h"
#include "event/EventLoopGroup.h"

namespace fiber::lite_nginx::upstream {
namespace {

fiber::http::Http1ConnectionPoolCore::Options
make_pool_options(const fiber::lite_nginx::runtime::ConnectionPoolRuntime &cp) noexcept {
    fiber::http::Http1ConnectionPoolCore::Options options{};
    options.max_idle_per_group = cp.keepalive_size;
    // 0 => derive the historical default (per-group cap * 64 peer groups). normalize_options
    // treats a literal 0 as "disable pooling", so the derive must happen here, not at the pool.
    options.max_idle_total = cp.max_idle_total != 0 ? cp.max_idle_total : cp.keepalive_size * 64;
    options.initial_group_capacity = cp.initial_group_capacity != 0 ? cp.initial_group_capacity : 16;
    options.idle_timeout = cp.keepalive_timeout;
    return options;
}

} // namespace

ConnectionPool::ConnectionLease::ConnectionLease(fiber::http::LocalHttp1ConnectionPoolSet::Lease local) noexcept :
    kind_(Kind::Local), local_(std::move(local)) {}

ConnectionPool::ConnectionLease::ConnectionLease(fiber::http::StealableHttp1ConnectionPoolSet::Lease stealable) noexcept
    : kind_(Kind::Stealable), stealable_(std::move(stealable)) {}

ConnectionPool::ConnectionLease::ConnectionLease(ConnectionLease &&other) noexcept : kind_(other.kind_) {
    switch (kind_) {
        case Kind::Local:
            local_ = std::move(other.local_);
            break;
        case Kind::Stealable:
            stealable_ = std::move(other.stealable_);
            break;
        case Kind::Empty:
            break;
    }
    other.kind_ = Kind::Empty;
}

ConnectionPool::ConnectionLease &ConnectionPool::ConnectionLease::operator=(ConnectionLease &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    kind_ = other.kind_;
    switch (kind_) {
        case Kind::Local:
            local_ = std::move(other.local_);
            break;
        case Kind::Stealable:
            stealable_ = std::move(other.stealable_);
            break;
        case Kind::Empty:
            break;
    }
    other.kind_ = Kind::Empty;
    return *this;
}

ConnectionPool::ConnectionLease::~ConnectionLease() { reset(); }

bool ConnectionPool::ConnectionLease::valid() const noexcept {
    switch (kind_) {
        case Kind::Local:
            return local_.valid();
        case Kind::Stealable:
            return stealable_.valid();
        case Kind::Empty:
            return false;
    }
    return false;
}

bool ConnectionPool::ConnectionLease::has_connection() const noexcept {
    switch (kind_) {
        case Kind::Local:
            return local_.has_connection();
        case Kind::Stealable:
            return stealable_.has_connection();
        case Kind::Empty:
            return false;
    }
    return false;
}

fiber::http::Http1ClientConnection *ConnectionPool::ConnectionLease::get() noexcept {
    switch (kind_) {
        case Kind::Local:
            return local_.get();
        case Kind::Stealable:
            return stealable_.get();
        case Kind::Empty:
            return nullptr;
    }
    return nullptr;
}

fiber::common::IoResult<fiber::http::Http1ClientConnection *>
ConnectionPool::ConnectionLease::emplace_connection(fiber::http::Http1ClientConnectionOptions options) noexcept {
    switch (kind_) {
        case Kind::Local:
            return local_.emplace_connection(std::move(options));
        case Kind::Stealable:
            return stealable_.emplace_connection(std::move(options));
        case Kind::Empty:
            return std::unexpected(fiber::common::IoErr::Invalid);
    }
    return std::unexpected(fiber::common::IoErr::Invalid);
}

void ConnectionPool::ConnectionLease::reset() noexcept {
    switch (kind_) {
        case Kind::Local:
            local_.reset();
            break;
        case Kind::Stealable:
            stealable_.reset();
            break;
        case Kind::Empty:
            break;
    }
    kind_ = Kind::Empty;
}

ConnectionPool::ConnectionPool(fiber::event::EventLoopGroup &group,
                               const fiber::lite_nginx::runtime::ConnectionPoolRuntime &cp) noexcept : group_(&group) {
    // keepalive_size == 0 => no pool; callers fall back to a transient connection (impl_ stays
    // empty). Otherwise pick the backing type by the resolved `steal` flag.
    if (cp.keepalive_size == 0) {
        return;
    }
    if (cp.steal) {
        impl_ = std::make_unique<fiber::http::StealableHttp1ConnectionPoolSet>(group, make_pool_options(cp));
    } else {
        impl_ = std::make_unique<fiber::http::LocalHttp1ConnectionPoolSet>(group, make_pool_options(cp));
    }
}

ConnectionPool::~ConnectionPool() = default;

bool ConnectionPool::init() noexcept {
    return std::visit(
            [](auto &pool) -> bool {
                if (!pool) {
                    return true;
                }
                return pool->init();
            },
            impl_);
}

fiber::async::Task<void> ConnectionPool::shutdown_async() noexcept {
    co_await std::visit(
            [](auto &pool) -> fiber::async::Task<void> {
                if (pool) {
                    co_await pool->shutdown_async();
                }
                co_return;
            },
            impl_);
}

void ConnectionPool::shutdown() noexcept {
    if (!group_ || group_->size() == 0 || !group_->running()) {
        return;
    }
    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    fiber::async::spawn(group_->at(0), [this, done]() -> fiber::async::DetachedTask {
        co_await shutdown_async();
        done->set_value();
    });
    future.wait();
}

fiber::async::Task<ConnectionPool::ConnectionLease>
ConnectionPool::acquire(const fiber::http::Http1ConnectionGroupKey &key) noexcept {
    // Empty impl_ => no pool configured; return an empty lease so the caller uses a transient
    // connection. Keep this synchronous (co_return) to avoid suspending on nothing.
    if (std::holds_alternative<std::unique_ptr<fiber::http::LocalHttp1ConnectionPoolSet>>(impl_)) {
        auto &pool = std::get<std::unique_ptr<fiber::http::LocalHttp1ConnectionPoolSet>>(impl_);
        if (pool) {
            auto lease = pool->acquire(key);
            co_return ConnectionLease(std::move(lease));
        }
    } else if (std::holds_alternative<std::unique_ptr<fiber::http::StealableHttp1ConnectionPoolSet>>(impl_)) {
        auto &pool = std::get<std::unique_ptr<fiber::http::StealableHttp1ConnectionPoolSet>>(impl_);
        if (pool) {
            auto lease = co_await pool->acquire(key);
            co_return ConnectionLease(std::move(lease));
        }
    }
    co_return ConnectionLease{};
}

} // namespace fiber::lite_nginx::upstream
