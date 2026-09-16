#include <fiber/http/StealableHttp1ConnectionPoolSet.h>

#include <atomic>
#include <coroutine>
#if FIBER_ENABLE_BENCHMARK_TRACE
#include <cstdio>
#include <cstdlib>
#endif
#include <memory>
#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::http {

#if FIBER_ENABLE_BENCHMARK_TRACE
namespace {

bool pool_trace_enabled() noexcept {
    static const bool enabled = [] {
        const char *trace = std::getenv("FIBER_HTTP_POOL_TRACE");
        return trace != nullptr && trace[0] != '\0' && trace[0] != '0';
    }();
    return enabled;
}

} // namespace
#endif

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

        static void run(ShardOp *op) noexcept {
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

    static void resume_caller(AdminAwaiter *awaiter) noexcept {
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

// Cross-thread steal journey. Phase one (owned by AcquireAwaiter): walk the
// shard ring and steal an entry on its home loop. Phase two (owned by the
// returned Lease): carry the entry home. Cancellation only exists in phase
// one; the awaiter destructor CAS-cancels and the State deletes itself.
class StealableHttp1ConnectionPoolSet::State : public common::NonCopyable, public common::NonMovable {
public:
    State(AcquireAwaiter &awaiter, std::coroutine_handle<> handle) noexcept :
        awaiter_(&awaiter), set_(awaiter.set_), caller_loop_(awaiter.caller_loop_), handle_(handle),
        key_(*awaiter.key_), local_fallback_(std::move(awaiter.local_fallback_)), home_slot_(awaiter.home_slot_),
        cursor_(awaiter.cursor_) {
        FIBER_ASSERT(set_ != nullptr);
        FIBER_ASSERT(caller_loop_ != nullptr);
        FIBER_ASSERT(home_slot_ != nullptr);
        FIBER_ASSERT(cursor_ != nullptr);
    }

    ~State() {
        FIBER_ASSERT(!registered_);
        // Every delete path nulls the result first: phase one only ends
        // deleted without a result, phase two hands it to the home pool.
        FIBER_ASSERT(result_entry_ == nullptr);
        FIBER_ASSERT(result_home_core_ == nullptr);
    }

    void mark_registered() noexcept {
        FIBER_ASSERT(!registered_);
        registered_ = true;
    }

    void start() noexcept { post_to(target_shard().core.loop(), Dispatch::TrySteal); }

    void request_cancel() noexcept {
        FIBER_ASSERT(caller_loop_ != nullptr);
        FIBER_ASSERT(caller_loop_->in_loop());

        Status status = status_.load(std::memory_order_acquire);
        for (;;) {
            switch (status) {
                case Status::Stealing:
                case Status::ResumeQueued:
                    if (status_.compare_exchange_weak(status, Status::Canceled, std::memory_order_acq_rel,
                                                      std::memory_order_acquire)) {
                        return;
                    }
                    break;
                case Status::Canceled:
                    return;
                case Status::Resumed:
                    FIBER_PANIC("cannot cancel a resumed HTTP/1 pool acquire");
            }
        }
    }

    // Hands the outcome to the caller loop. On a remote steal the State
    // itself travels with the lease as the return vehicle (`journey`); the
    // caller keeps it alive until the entry goes home. The active-acquire
    // window spans that whole journey: shutdown waits for borrowed leases and
    // in-flight returns, not only for the acquire up to delivery. Otherwise
    // the State dies with the acquire.
    Lease take_result(State *&journey) noexcept {
        FIBER_ASSERT(caller_loop_ != nullptr);
        FIBER_ASSERT(caller_loop_->in_loop());
        FIBER_ASSERT(status_.load(std::memory_order_acquire) == Status::Resumed);

        Lease result;
        if (result_entry_ && result_home_core_) {
            // The entry left its home loop detached (handover protocol): bind
            // it to this loop. First use re-registers lazily. The State keeps
            // the entry pointers: it is the return vehicle until the lease
            // goes home.
            const common::IoErr adopted = result_entry_->connection()->adopt_loop(*caller_loop_);
            if (adopted == common::IoErr::None) {
                result = Lease(*result_home_core_, *result_entry_, key_, this);
                journey = this;
            } else {
                // The caller loop is already stopping and refused the
                // adoption, which closed the connection. No lease is
                // delivered — the shutdown race returns an empty lease the
                // same way — and the entry recycles at home with this State
                // as the journey vehicle.
                post_to(result_home_core_->loop(), Dispatch::ReturnHome);
                journey = this;
            }
        } else {
            result = Lease(std::move(local_fallback_));
            journey = nullptr;
            // Local outcome: the acquire ends here and now.
            finish_registered();
        }
        // The awaiter and its coroutine die with the acquire; the return
        // journey must never touch them.
        awaiter_ = nullptr;
        handle_ = {};
        return result;
    }

    // Phase two, entered from Lease::reset on the loop the connection was
    // adopted to. The reset can run inside this fd's own event dispatch (the
    // exchange coroutine completes from a read callback); detaching and
    // publishing there would let the home loop re-arm the fd underneath a
    // live dispatch. A posted notify runs on this loop's next turn, after any
    // in-flight dispatch has unwound.
    void return_home() noexcept {
        FIBER_ASSERT(caller_loop_ != nullptr);
        FIBER_ASSERT(caller_loop_->in_loop());
        FIBER_ASSERT(result_entry_ != nullptr);
        FIBER_ASSERT(result_home_core_ != nullptr);
        post_to(*caller_loop_, Dispatch::DetachAndReturn);
    }

    void detach_and_return() noexcept {
        FIBER_ASSERT(caller_loop_->in_loop());
        FIBER_ASSERT(result_entry_ != nullptr);
        FIBER_ASSERT(result_home_core_ != nullptr);

        Http1ClientConnection *conn = result_entry_->connection();
        if (conn != nullptr && !conn->loop_detached() && conn->valid()) {
            // The fd is still bound to this loop: detach it — or close it,
            // when the connection is no longer reusable — before the entry
            // crosses threads. A detach failure already closed it inside.
            if (conn->idle()) {
                (void) conn->prepare_loop_handover();
            } else {
                conn->close();
            }
        }
        post_to(result_home_core_->loop(), Dispatch::ReturnHome);
    }

private:
    enum class Status : std::uint8_t { Stealing, ResumeQueued, Resumed, Canceled };
    enum class Dispatch : std::uint8_t {
        TrySteal,
        ResumeCaller,
        ReturnCanceledEntry,
        FinalizeCanceled,
        DetachAndReturn,
        ReturnHome
    };

    [[nodiscard]] Shard &target_shard() const noexcept {
        FIBER_ASSERT(cursor_ != nullptr);
        return *std::launder(reinterpret_cast<Shard *>(cursor_->storage));
    }

    [[nodiscard]] bool advance_to_candidate() noexcept {
        FIBER_ASSERT(home_slot_ != nullptr);
        while (cursor_ != home_slot_) {
            if (target_shard().hint.probe(key_).may_have()) {
                return true;
            }
            cursor_ = cursor_->next;
        }
        return false;
    }

    void post_to(event::EventLoop &loop, Dispatch dispatch) noexcept {
        dispatch_ = dispatch;
        loop.post<State, &State::notify_entry_, &State::run_notify>(*this);
    }

    static void run_notify(State *state) noexcept {
        FIBER_ASSERT(state != nullptr);
        switch (state->dispatch_) {
            case Dispatch::TrySteal:
                state->try_steal();
                return;
            case Dispatch::ResumeCaller:
                state->resume_caller();
                return;
            case Dispatch::ReturnCanceledEntry:
                state->return_canceled_entry();
                return;
            case Dispatch::FinalizeCanceled:
                state->finalize_canceled();
                return;
            case Dispatch::DetachAndReturn:
                state->detach_and_return();
                return;
            case Dispatch::ReturnHome:
                state->arrive_home();
                return;
        }
    }

    void try_steal() noexcept {
        FIBER_ASSERT(target_shard().core.loop().in_loop());
        if (status_.load(std::memory_order_acquire) == Status::Canceled) {
            post_to(*caller_loop_, Dispatch::FinalizeCanceled);
            return;
        }

#if FIBER_ENABLE_BENCHMARK_TRACE
        set_->trace_remote_attempt_.fetch_add(1, std::memory_order_relaxed);
#endif
        result_home_core_ = &target_shard().core;
        result_entry_ = result_home_core_->try_steal_idle_entry(key_);
        if (result_entry_ && result_entry_->connection()->prepare_loop_handover() != common::IoErr::None) {
            // The handover detach failed and closed the connection: hand the
            // entry back for recycling and keep hunting. The canceled-entry
            // path below also routes through accept_returned_entry, which
            // re-adopts entries that left their loop detached.
            result_home_core_->accept_returned_entry(*result_entry_, key_);
            result_entry_ = nullptr;
        }
        if (result_entry_) {
#if FIBER_ENABLE_BENCHMARK_TRACE
            set_->trace_remote_hit();
#endif
            Status expected = Status::Stealing;
            if (status_.compare_exchange_strong(expected, Status::ResumeQueued, std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
                post_to(*caller_loop_, Dispatch::ResumeCaller);
                return;
            }

            FIBER_ASSERT(expected == Status::Canceled);
            result_home_core_->accept_returned_entry(*result_entry_, key_);
            result_entry_ = nullptr;
            result_home_core_ = nullptr;
            post_to(*caller_loop_, Dispatch::FinalizeCanceled);
            return;
        }

#if FIBER_ENABLE_BENCHMARK_TRACE
        set_->trace_remote_attempt_miss_.fetch_add(1, std::memory_order_relaxed);
#endif
        result_home_core_ = nullptr;
        cursor_ = cursor_->next;
        if (advance_to_candidate()) {
            if (status_.load(std::memory_order_acquire) == Status::Canceled) {
                post_to(*caller_loop_, Dispatch::FinalizeCanceled);
            } else {
                post_to(target_shard().core.loop(), Dispatch::TrySteal);
            }
            return;
        }

        cursor_ = nullptr;
        Status expected = Status::Stealing;
        if (status_.compare_exchange_strong(expected, Status::ResumeQueued, std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
            post_to(*caller_loop_, Dispatch::ResumeCaller);
            return;
        }

        FIBER_ASSERT(expected == Status::Canceled);
        post_to(*caller_loop_, Dispatch::FinalizeCanceled);
    }

    void resume_caller() noexcept {
        FIBER_ASSERT(caller_loop_ != nullptr);
        FIBER_ASSERT(caller_loop_->in_loop());

        Status expected = Status::ResumeQueued;
        if (status_.compare_exchange_strong(expected, Status::Resumed, std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
            FIBER_ASSERT(awaiter_ != nullptr);
            awaiter_->completed_ = true;
            auto handle = std::exchange(handle_, {});
            FIBER_ASSERT(handle);
            handle.resume();
            return;
        }

        FIBER_ASSERT(expected == Status::Canceled);
        if (result_entry_ && result_home_core_) {
            post_to(result_home_core_->loop(), Dispatch::ReturnCanceledEntry);
            return;
        }
        finalize_canceled();
    }

    void return_canceled_entry() noexcept {
        FIBER_ASSERT(status_.load(std::memory_order_acquire) == Status::Canceled);
        FIBER_ASSERT(result_entry_ != nullptr);
        FIBER_ASSERT(result_home_core_ != nullptr);
        FIBER_ASSERT(result_home_core_->loop().in_loop());

        result_home_core_->accept_returned_entry(*result_entry_, key_);
        result_entry_ = nullptr;
        result_home_core_ = nullptr;
        post_to(*caller_loop_, Dispatch::FinalizeCanceled);
    }

    void finalize_canceled() noexcept {
        FIBER_ASSERT(caller_loop_ != nullptr);
        FIBER_ASSERT(caller_loop_->in_loop());
        FIBER_ASSERT(status_.load(std::memory_order_acquire) == Status::Canceled);
        FIBER_ASSERT(result_entry_ == nullptr);
        FIBER_ASSERT(result_home_core_ == nullptr);

        finish_registered();
        delete this;
    }

    void arrive_home() noexcept {
        FIBER_ASSERT(result_entry_ != nullptr);
        FIBER_ASSERT(result_home_core_ != nullptr);
        FIBER_ASSERT(result_home_core_->loop().in_loop());

        Http1ConnectionPoolEntry *entry = std::exchange(result_entry_, nullptr);
        Http1ConnectionPoolCore *home_core = std::exchange(result_home_core_, nullptr);
        const HttpConnectionGroupKey key = key_;
        // The journey ends here; the home pool owns the entry from this point.
        // The active-acquire window ends with it, so a completed shutdown has
        // seen every borrowed lease and in-flight return message land.
        finish_registered();
        delete this;
        home_core->accept_returned_entry(*entry, key);
    }

    void finish_registered() noexcept {
        FIBER_ASSERT(registered_);
        registered_ = false;
        set_->finish_remote_acquire();
    }

    AcquireAwaiter *awaiter_ = nullptr;
    StealableHttp1ConnectionPoolSet *set_ = nullptr;
    event::EventLoop *caller_loop_ = nullptr;
    std::coroutine_handle<> handle_{};
    HttpConnectionGroupKey key_;
    Http1ConnectionPoolCore::Lease local_fallback_{};
    ShardSlot *home_slot_ = nullptr;
    ShardSlot *cursor_ = nullptr;
    Http1ConnectionPoolEntry *result_entry_ = nullptr;
    Http1ConnectionPoolCore *result_home_core_ = nullptr;
    event::EventLoop::NotifyEntry notify_entry_{};
    std::atomic<Status> status_{Status::Stealing};
    Dispatch dispatch_ = Dispatch::TrySteal;
    bool registered_ = false;
};

StealableHttp1ConnectionPoolSet::Lease::Lease(Http1ConnectionPoolCore::Lease &&local) noexcept :
    kind_(Kind::Local), local_(std::move(local)) {}

StealableHttp1ConnectionPoolSet::Lease::Lease(Http1ConnectionPoolCore &home_core, Http1ConnectionPoolEntry &entry,
                                              const HttpConnectionGroupKey &key, State *journey) noexcept :
    kind_(Kind::Remote), entry_(&entry), home_core_(&home_core), key_(key), journey_(journey) {}

StealableHttp1ConnectionPoolSet::Lease::Lease(Lease &&other) noexcept :
    kind_(other.kind_), local_(std::move(other.local_)), entry_(other.entry_), home_core_(other.home_core_),
    key_(std::move(other.key_)), journey_(other.journey_) {
    other.kind_ = Kind::Empty;
    other.entry_ = nullptr;
    other.home_core_ = nullptr;
    other.key_.reset();
    other.journey_ = nullptr;
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
    journey_ = other.journey_;
    other.kind_ = Kind::Empty;
    other.entry_ = nullptr;
    other.home_core_ = nullptr;
    other.key_.reset();
    other.journey_ = nullptr;
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

const HttpConnectionGroupKey &StealableHttp1ConnectionPoolSet::Lease::key() const noexcept {
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

common::IoResult<Http1ClientConnection *> StealableHttp1ConnectionPoolSet::Lease::emplace_connection() noexcept {
    if (kind_ != Kind::Local) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return local_.emplace_connection();
}

void StealableHttp1ConnectionPoolSet::Lease::reset() noexcept {
    if (kind_ == Kind::Local) {
        local_.reset();
        kind_ = Kind::Empty;
        key_.reset();
        return;
    }

    if (kind_ == Kind::Remote) {
        // A remote lease's connection was adopted to the caller loop; the
        // return journey detaches (or closes) it there and publishes the
        // entry to its home pool. Resetting from any other loop violates the
        // handover contract.
        FIBER_ASSERT(journey_ != nullptr);
        FIBER_ASSERT(entry_ != nullptr);
        FIBER_ASSERT(home_core_ != nullptr);
        FIBER_ASSERT(key_.has_value());
        State *journey = std::exchange(journey_, nullptr);
        journey->return_home();
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
#if FIBER_ENABLE_BENCHMARK_TRACE
    trace_report();
#endif
    FIBER_ASSERT(active_acquire_wg_.empty());
    for (std::size_t i = 0; i < group_->size(); ++i) {
        auto *shard = reinterpret_cast<Shard *>(storage_[i].storage);
        std::destroy_at(shard);
    }
}

#if FIBER_ENABLE_BENCHMARK_TRACE
void StealableHttp1ConnectionPoolSet::trace_remote_hit() noexcept {
    if (trace_remote_hit_.fetch_add(1, std::memory_order_relaxed) == 0) {
        trace_report();
    }
}

void StealableHttp1ConnectionPoolSet::trace_report() const noexcept {
    if (!pool_trace_enabled()) {
        return;
    }
    std::fprintf(stderr,
                 "FIBER_HTTP_POOL_TRACE local_hit=%llu remote_attempt=%llu remote_hit=%llu "
                 "remote_attempt_miss=%llu no_candidate=%llu\n",
                 static_cast<unsigned long long>(trace_local_hit_.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(trace_remote_attempt_.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(trace_remote_hit_.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(trace_remote_attempt_miss_.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(trace_no_candidate_.load(std::memory_order_relaxed)));
}
#endif

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

    co_await active_acquire_wg_.join();
    co_await AdminAwaiter(*this, AdminAwaiter::Operation::Shutdown);
    shutdown_wg_.done();
}

StealableHttp1ConnectionPoolSet::AcquireAwaiter::AcquireAwaiter(StealableHttp1ConnectionPoolSet &set,
                                                                const HttpConnectionGroupKey &key) noexcept :
    set_(&set), key_(key) {}

StealableHttp1ConnectionPoolSet::AcquireAwaiter::~AcquireAwaiter() noexcept {
    if (!state_) {
        return;
    }
    FIBER_ASSERT(caller_loop_ != nullptr);
    FIBER_ASSERT(caller_loop_->in_loop());
    State *state = std::exchange(state_, nullptr);
    state->request_cancel();
}

bool StealableHttp1ConnectionPoolSet::AcquireAwaiter::await_ready() noexcept {
    completed_ = prepare();
    return completed_;
}

bool StealableHttp1ConnectionPoolSet::AcquireAwaiter::await_suspend(std::coroutine_handle<> handle) noexcept {
    if (!prepared_ && prepare()) {
        completed_ = true;
        return false;
    }
    FIBER_ASSERT(!completed_);
    FIBER_ASSERT(state_ == nullptr);
    FIBER_ASSERT(cursor_ != nullptr);

    auto *state = new (std::nothrow) State(*this, handle);
    if (!state) {
        result_ = Lease(std::move(local_fallback_));
        cursor_ = nullptr;
        completed_ = true;
        return false;
    }
    if (!set_->begin_remote_acquire()) {
        delete state;
        cursor_ = nullptr;
        completed_ = true;
        return false;
    }

    state->mark_registered();
    state_ = state;
    state_->start();
    return true;
}

StealableHttp1ConnectionPoolSet::Lease StealableHttp1ConnectionPoolSet::AcquireAwaiter::await_resume() noexcept {
    FIBER_ASSERT(completed_);
    if (state_) {
        State *state = std::exchange(state_, nullptr);
        State *journey = nullptr;
        Lease result = state->take_result(journey);
        if (!journey) {
            // Local fallback outcome: no cross-loop return journey to keep.
            delete state;
        }
        return result;
    }
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
#if FIBER_ENABLE_BENCHMARK_TRACE
        set_->trace_local_hit_.fetch_add(1, std::memory_order_relaxed);
#endif
        result_ = Lease(std::move(local_fallback_));
        prepared_ = true;
        cursor_ = nullptr;
        return true;
    }

    cursor_ = home_slot_->next;
    prepared_ = true;
    if (!advance_to_candidate()) {
#if FIBER_ENABLE_BENCHMARK_TRACE
        set_->trace_no_candidate_.fetch_add(1, std::memory_order_relaxed);
#endif
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

StealableHttp1ConnectionPoolSet::AcquireAwaiter
StealableHttp1ConnectionPoolSet::acquire(const HttpConnectionGroupKey &key) noexcept {
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

bool StealableHttp1ConnectionPoolSet::begin_remote_acquire() noexcept {
    std::lock_guard guard(shutdown_mu_);
    if (shutdown_requested_.load(std::memory_order_relaxed)) {
        return false;
    }
    active_acquire_wg_.add();
    return true;
}

void StealableHttp1ConnectionPoolSet::finish_remote_acquire() noexcept { active_acquire_wg_.done(); }

} // namespace fiber::http
