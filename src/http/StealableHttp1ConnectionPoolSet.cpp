#include "StealableHttp1ConnectionPoolSet.h"

#include <atomic>
#include <coroutine>
#include <memory>
#include <utility>

#include "../common/Assert.h"

namespace fiber::http {

class StealableHttp1ConnectionPoolSet::AdminAwaiter : public common::NonCopyable, public common::NonMovable {
public:
    enum class Operation : std::uint8_t { Clear, Shutdown };

    AdminAwaiter(StealableHttp1ConnectionPoolSet &set, Operation operation) noexcept :
        set_(&set), operation_(operation) {}

    bool await_ready() noexcept {
        if (!set_) {
            return true;
        }
        if (operation_ == Operation::Clear && set_->shutdown_requested_.load(std::memory_order_acquire)) {
            return true;
        }
        if (set_->group().running()) {
            return false;
        }
        for (std::size_t i = 0; i < set_->size(); ++i) {
            run_shard(set_->shard_at(i));
        }
        return true;
    }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
        FIBER_ASSERT(set_ != nullptr);
        handle_ = handle;
        caller_loop_ = event::EventLoop::current_or_null();
        FIBER_ASSERT(caller_loop_ != nullptr);

        const std::size_t shard_count = set_->size();
        if (shard_count == 0) {
            caller_loop_->post<AdminAwaiter, &AdminAwaiter::resume_notify_, &AdminAwaiter::resume_caller>(*this);
            return true;
        }

        remaining_.store(shard_count, std::memory_order_release);
        ops_ = std::make_unique<ShardOp[]>(shard_count);
        auto *current = event::EventLoop::current_or_null();
        for (std::size_t i = 0; i < shard_count; ++i) {
            Shard &shard = set_->shard_at(i);
            if (current == &shard.core.loop()) {
                run_shard(shard);
                on_shard_done();
                continue;
            }
            ops_[i].awaiter = this;
            ops_[i].shard = &shard;
            shard.core.loop().post<ShardOp, &ShardOp::notify, &ShardOp::run>(ops_[i]);
        }
        return true;
    }

    void await_resume() noexcept {}

private:
    struct ShardOp {
        AdminAwaiter *awaiter = nullptr;
        Shard *shard = nullptr;
        event::EventLoop::NotifyEntry notify{};

        static void run(ShardOp *op) {
            FIBER_ASSERT(op != nullptr);
            FIBER_ASSERT(op->awaiter != nullptr);
            FIBER_ASSERT(op->shard != nullptr);
            op->awaiter->run_shard(*op->shard);
            op->awaiter->on_shard_done();
        }
    };

    void run_shard(Shard &shard) noexcept {
        if (operation_ == Operation::Clear) {
            shard.core.clear();
        } else {
            shard.core.shutdown();
        }
        shard.hint.clear();
    }

    void on_shard_done() noexcept {
        if (remaining_.fetch_sub(1, std::memory_order_acq_rel) != 1) {
            return;
        }
        FIBER_ASSERT(caller_loop_ != nullptr);
        caller_loop_->post<AdminAwaiter, &AdminAwaiter::resume_notify_, &AdminAwaiter::resume_caller>(*this);
    }

    static void resume_caller(AdminAwaiter *awaiter) {
        FIBER_ASSERT(awaiter != nullptr);
        if (!awaiter->handle_) {
            return;
        }
        auto handle = awaiter->handle_;
        awaiter->handle_ = {};
        handle.resume();
    }

    StealableHttp1ConnectionPoolSet *set_ = nullptr;
    Operation operation_ = Operation::Clear;
    event::EventLoop *caller_loop_ = nullptr;
    std::coroutine_handle<> handle_{};
    std::unique_ptr<ShardOp[]> ops_{};
    std::atomic<std::size_t> remaining_{0};
    event::EventLoop::NotifyEntry resume_notify_{};
};

StealableHttp1ConnectionPoolSet::Lease::Lease(Http1ConnectionPoolCore::Lease &&local) noexcept :
    kind_(Kind::Local), local_(std::move(local)) {}

StealableHttp1ConnectionPoolSet::Lease::Lease(Http1ConnectionPoolCore &home_core, Http1ConnectionPoolEntry &entry,
                                              const Http1ConnectionGroupKey &key) noexcept :
    kind_(Kind::Remote), entry_(&entry), home_core_(&home_core), key_(key) {}

StealableHttp1ConnectionPoolSet::Lease::Lease(Lease &&other) noexcept :
    kind_(other.kind_), local_(std::move(other.local_)), entry_(other.entry_), home_core_(other.home_core_),
    key_(std::move(other.key_)) {
    other.kind_ = Kind::Empty;
    other.entry_ = nullptr;
    other.home_core_ = nullptr;
    other.key_.reset();
}

StealableHttp1ConnectionPoolSet::Lease &StealableHttp1ConnectionPoolSet::Lease::operator=(Lease &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    kind_ = other.kind_;
    local_ = std::move(other.local_);
    entry_ = other.entry_;
    home_core_ = other.home_core_;
    key_ = std::move(other.key_);
    other.kind_ = Kind::Empty;
    other.entry_ = nullptr;
    other.home_core_ = nullptr;
    other.key_.reset();
    return *this;
}

StealableHttp1ConnectionPoolSet::Lease::~Lease() { reset(); }

bool StealableHttp1ConnectionPoolSet::Lease::valid() const noexcept {
    switch (kind_) {
        case Kind::Local:
            return local_.valid();
        case Kind::Remote:
            return entry_ != nullptr && home_core_ != nullptr && key_.has_value();
        case Kind::Empty:
            return false;
    }
    return false;
}

bool StealableHttp1ConnectionPoolSet::Lease::has_connection() const noexcept {
    switch (kind_) {
        case Kind::Local:
            return local_.has_connection();
        case Kind::Remote:
            return entry_ != nullptr && entry_->has_connection();
        case Kind::Empty:
            return false;
    }
    return false;
}

bool StealableHttp1ConnectionPoolSet::Lease::hit() const noexcept {
    switch (kind_) {
        case Kind::Local:
            return local_.hit();
        case Kind::Remote:
            return true;
        case Kind::Empty:
            return false;
    }
    return false;
}

Http1ClientConnection *StealableHttp1ConnectionPoolSet::Lease::get() noexcept {
    switch (kind_) {
        case Kind::Local:
            return local_.get();
        case Kind::Remote:
            return entry_ ? entry_->connection() : nullptr;
        case Kind::Empty:
            return nullptr;
    }
    return nullptr;
}

const Http1ClientConnection *StealableHttp1ConnectionPoolSet::Lease::get() const noexcept {
    switch (kind_) {
        case Kind::Local:
            return local_.get();
        case Kind::Remote:
            return entry_ ? entry_->connection() : nullptr;
        case Kind::Empty:
            return nullptr;
    }
    return nullptr;
}

Http1ClientConnection &StealableHttp1ConnectionPoolSet::Lease::connection() noexcept {
    Http1ClientConnection *conn = get();
    FIBER_ASSERT(conn != nullptr);
    return *conn;
}

const Http1ConnectionGroupKey &StealableHttp1ConnectionPoolSet::Lease::key() const noexcept {
    switch (kind_) {
        case Kind::Local:
            return local_.key();
        case Kind::Remote:
            FIBER_ASSERT(key_.has_value());
            return *key_;
        case Kind::Empty:
            break;
    }
    FIBER_PANIC("empty stealable pool lease has no key");
}

common::IoResult<Http1ClientConnection *>
StealableHttp1ConnectionPoolSet::Lease::emplace_connection(Http1ClientConnectionOptions options) noexcept {
    if (kind_ != Kind::Local) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return local_.emplace_connection(std::move(options));
}

void StealableHttp1ConnectionPoolSet::Lease::reset() noexcept {
    if (kind_ == Kind::Local) {
        local_.reset();
        kind_ = Kind::Empty;
        key_.reset();
        return;
    }

    if (kind_ == Kind::Remote && entry_ && home_core_ && key_.has_value()) {
        auto *current_loop = event::EventLoop::current_or_null();
        if (current_loop == &home_core_->loop()) {
            home_core_->accept_returned_entry(*entry_, *key_);
        } else {
            entry_->post_remote_return(*home_core_, *key_);
        }
    }

    kind_ = Kind::Empty;
    entry_ = nullptr;
    home_core_ = nullptr;
    key_.reset();
}

StealableHttp1ConnectionPoolSet::StealableHttp1ConnectionPoolSet(event::EventLoopGroup &group,
                                                                 Options pool_options) noexcept :
    group_(&group), pool_options_(pool_options), storage_(std::make_unique<ShardSlot[]>(group.size())) {
    for (std::size_t i = 0; i < group.size(); ++i) {
        auto *shard = reinterpret_cast<Shard *>(storage_[i].storage);
        std::construct_at(shard, group.at(i), pool_options_);
        shard->core.set_external_shutdown_flag(&shutdown_requested_);
    }
    if (group.size() == 0) {
        return;
    }
    for (std::size_t i = 0; i < group.size(); ++i) {
        slot_at(i).next = &slot_at((i + 1) % group.size());
    }
}

StealableHttp1ConnectionPoolSet::StealableHttp1ConnectionPoolSet(event::EventLoopGroup &group) noexcept :
    StealableHttp1ConnectionPoolSet(group, Options{}) {}

StealableHttp1ConnectionPoolSet::~StealableHttp1ConnectionPoolSet() {
    for (std::size_t i = 0; i < group_->size(); ++i) {
        auto *shard = reinterpret_cast<Shard *>(storage_[i].storage);
        std::destroy_at(shard);
    }
}

bool StealableHttp1ConnectionPoolSet::init() noexcept {
    for (std::size_t i = 0; i < group_->size(); ++i) {
        if (!shard_at(i).core.init()) {
            return false;
        }
    }
    return true;
}

async::Task<void> StealableHttp1ConnectionPoolSet::clear_async() noexcept {
    if (shutdown_requested_.load(std::memory_order_acquire)) {
        co_return;
    }
    co_await AdminAwaiter(*this, AdminAwaiter::Operation::Clear);
}

async::Task<void> StealableHttp1ConnectionPoolSet::shutdown_async() noexcept {
    bool leader = false;
    {
        std::lock_guard guard(shutdown_mu_);
        if (!shutdown_requested_.load(std::memory_order_relaxed)) {
            shutdown_wg_.add();
            shutdown_requested_.store(true, std::memory_order_release);
            leader = true;
        }
    }

    if (!leader) {
        co_await shutdown_wg_.join();
        co_return;
    }

    co_await AdminAwaiter(*this, AdminAwaiter::Operation::Shutdown);
    shutdown_wg_.done();
}

StealableHttp1ConnectionPoolSet::AcquireAwaiter::AcquireAwaiter(StealableHttp1ConnectionPoolSet &set,
                                                                const Http1ConnectionGroupKey &key) noexcept :
    set_(&set), key_(key) {}

bool StealableHttp1ConnectionPoolSet::AcquireAwaiter::await_ready() noexcept { return prepare(); }

bool StealableHttp1ConnectionPoolSet::AcquireAwaiter::await_suspend(std::coroutine_handle<> handle) noexcept {
    if (!prepared_ && prepare()) {
        return false;
    }
    handle_ = handle;
    post_target();
    return true;
}

StealableHttp1ConnectionPoolSet::Lease StealableHttp1ConnectionPoolSet::AcquireAwaiter::await_resume() noexcept {
    return std::move(result_);
}

StealableHttp1ConnectionPoolSet::Shard &StealableHttp1ConnectionPoolSet::AcquireAwaiter::target_shard() const noexcept {
    FIBER_ASSERT(set_ != nullptr);
    FIBER_ASSERT(cursor_ != nullptr);
    return *std::launder(reinterpret_cast<Shard *>(cursor_->storage));
}

bool StealableHttp1ConnectionPoolSet::AcquireAwaiter::prepare() noexcept {
    if (prepared_) {
        return !cursor_;
    }
    FIBER_ASSERT(set_ != nullptr);
    FIBER_ASSERT(key_.has_value());
    caller_loop_ = &event::EventLoop::current();
    FIBER_ASSERT(caller_loop_->group() == set_->group_);
    FIBER_ASSERT(caller_loop_->has_group_index());

    home_slot_ = &set_->slot_at(caller_loop_->group_index());
    local_fallback_ = set_->current_shard().core.acquire(*key_);
    if (!local_fallback_.valid()) {
        prepared_ = true;
        cursor_ = nullptr;
        return true;
    }
    if (local_fallback_.hit()) {
        result_ = Lease(std::move(local_fallback_));
        prepared_ = true;
        cursor_ = nullptr;
        return true;
    }

    cursor_ = home_slot_->next;
    prepared_ = true;
    if (!advance_to_candidate()) {
        result_ = Lease(std::move(local_fallback_));
        cursor_ = nullptr;
        return true;
    }
    return false;
}

bool StealableHttp1ConnectionPoolSet::AcquireAwaiter::advance_to_candidate() noexcept {
    FIBER_ASSERT(home_slot_ != nullptr);
    while (cursor_ != home_slot_) {
        if (target_shard().hint.probe(*key_).may_have()) {
            return true;
        }
        cursor_ = cursor_->next;
    }
    return false;
}

void StealableHttp1ConnectionPoolSet::AcquireAwaiter::post_target() noexcept {
    FIBER_ASSERT(cursor_ != nullptr);
    phase_ = Phase::SubmitSteal;
    target_shard().core.loop().post<AcquireAwaiter, &AcquireAwaiter::notify_entry_, &AcquireAwaiter::run_notify>(*this);
}

void StealableHttp1ConnectionPoolSet::AcquireAwaiter::run_notify(AcquireAwaiter *awaiter) {
    FIBER_ASSERT(awaiter != nullptr);
    switch (awaiter->phase_) {
        case Phase::SubmitSteal:
            awaiter->result_home_core_ = &awaiter->target_shard().core;
            awaiter->result_entry_ = awaiter->result_home_core_->try_steal_idle_entry(*awaiter->key_);
            if (awaiter->result_entry_ && awaiter->result_home_core_) {
                awaiter->phase_ = Phase::ResumeCaller;
                awaiter->caller_loop_
                        ->post<AcquireAwaiter, &AcquireAwaiter::notify_entry_, &AcquireAwaiter::run_notify>(*awaiter);
                return;
            }

            awaiter->cursor_ = awaiter->cursor_->next;
            awaiter->result_entry_ = nullptr;
            awaiter->result_home_core_ = nullptr;
            if (awaiter->advance_to_candidate()) {
                awaiter->post_target();
                return;
            }

            awaiter->result_ = Lease(std::move(awaiter->local_fallback_));
            awaiter->cursor_ = nullptr;
            awaiter->phase_ = Phase::ResumeCaller;
            awaiter->caller_loop_->post<AcquireAwaiter, &AcquireAwaiter::notify_entry_, &AcquireAwaiter::run_notify>(
                    *awaiter);
            return;
        case Phase::ResumeCaller:
            if (awaiter->result_entry_ && awaiter->result_home_core_) {
                awaiter->result_ = Lease(*awaiter->result_home_core_, *awaiter->result_entry_, *awaiter->key_);
            }
            if (awaiter->handle_) {
                auto handle = awaiter->handle_;
                awaiter->handle_ = {};
                handle.resume();
            }
            return;
    }
}

StealableHttp1ConnectionPoolSet::AcquireAwaiter
StealableHttp1ConnectionPoolSet::acquire(const Http1ConnectionGroupKey &key) noexcept {
    return AcquireAwaiter(*this, key);
}

StealableHttp1ConnectionPoolSet::Shard &StealableHttp1ConnectionPoolSet::current_shard() noexcept {
    auto &loop = event::EventLoop::current();
    FIBER_ASSERT(loop.group() == group_);
    FIBER_ASSERT(loop.has_group_index());
    return shard_at(loop.group_index());
}

const StealableHttp1ConnectionPoolSet::Shard &StealableHttp1ConnectionPoolSet::current_shard() const noexcept {
    auto &loop = event::EventLoop::current();
    FIBER_ASSERT(loop.group() == group_);
    FIBER_ASSERT(loop.has_group_index());
    return shard_at(loop.group_index());
}

StealableHttp1ConnectionPoolSet::ShardSlot &StealableHttp1ConnectionPoolSet::current_slot() noexcept {
    auto &loop = event::EventLoop::current();
    FIBER_ASSERT(loop.group() == group_);
    FIBER_ASSERT(loop.has_group_index());
    return slot_at(loop.group_index());
}

const StealableHttp1ConnectionPoolSet::ShardSlot &StealableHttp1ConnectionPoolSet::current_slot() const noexcept {
    auto &loop = event::EventLoop::current();
    FIBER_ASSERT(loop.group() == group_);
    FIBER_ASSERT(loop.has_group_index());
    return slot_at(loop.group_index());
}

StealableHttp1ConnectionPoolSet::Shard &StealableHttp1ConnectionPoolSet::shard_at(std::size_t index) noexcept {
    FIBER_ASSERT(index < group_->size());
    return *std::launder(reinterpret_cast<Shard *>(storage_[index].storage));
}

const StealableHttp1ConnectionPoolSet::Shard &
StealableHttp1ConnectionPoolSet::shard_at(std::size_t index) const noexcept {
    FIBER_ASSERT(index < group_->size());
    return *std::launder(reinterpret_cast<const Shard *>(storage_[index].storage));
}

StealableHttp1ConnectionPoolSet::ShardSlot &StealableHttp1ConnectionPoolSet::slot_at(std::size_t index) noexcept {
    FIBER_ASSERT(index < group_->size());
    return storage_[index];
}

const StealableHttp1ConnectionPoolSet::ShardSlot &
StealableHttp1ConnectionPoolSet::slot_at(std::size_t index) const noexcept {
    FIBER_ASSERT(index < group_->size());
    return storage_[index];
}

} // namespace fiber::http
