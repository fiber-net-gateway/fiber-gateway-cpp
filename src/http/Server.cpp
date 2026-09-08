#include <fiber/http/Server.h>

#include <chrono>
#include <utility>

#include <fiber/async/Spawn.h>
#include <fiber/common/Assert.h>

namespace fiber::http {

// Per-worker orchestration: holds one EndpointWorker slot per endpoint and
// drives them through drain -> (deadline) -> abort -> wait -> destroy, all on
// this worker's own event loop.
class Server::Worker : public common::NonCopyable, public common::NonMovable {
public:
    Worker(Server &server, event::EventLoop &loop, std::size_t index) noexcept :
        server_(&server), loop_(loop), index_(index) {}

    // Slots are released either by run_wait_stop() on this loop, or by
    // discard_slots() when start() rolls back.
    ~Worker() { FIBER_ASSERT(slots_.empty()); }

    [[nodiscard]] event::EventLoop &loop() const noexcept { return loop_; }
    [[nodiscard]] std::size_t index() const noexcept { return index_; }

    // Startup only, in endpoint order. Returns false on allocation failure.
    bool install(EndpointWorker *worker, std::chrono::milliseconds drain_timeout) noexcept {
        auto *slot = new (std::nothrow) Slot();
        if (slot == nullptr) {
            return false;
        }
        slot->owner = this;
        slot->worker.reset(worker);
        slot->drain_timeout = drain_timeout;
        slots_.push_back(std::unique_ptr<Slot>(slot));
        return true;
    }

    // Only legal before the worker has been asked to stop: a failed start()
    // tears the slots down on the startup thread, where nothing has run on the
    // worker loop yet.
    void discard_slots() noexcept {
        FIBER_ASSERT(!stop_posted_.load(std::memory_order_acquire));
        slots_.clear();
    }

    // Any thread, idempotent.
    void notify_stop() noexcept {
        if (stop_posted_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        if (loop_.in_loop()) {
            on_stop(this);
            return;
        }
        loop_.post<Worker, &Worker::stop_entry_, &Worker::on_stop>(*this);
    }

private:
    // One endpoint's slot on this worker. The deadline timer lives here rather
    // than on the Worker so that each endpoint gets its own drain budget;
    // TimerEntry is not movable, hence the indirection.
    struct Slot {
        Worker *owner = nullptr;
        std::unique_ptr<EndpointWorker> worker{};
        std::chrono::milliseconds drain_timeout{};
        event::EventLoop::TimerEntry deadline{};
    };

    static void on_stop(Worker *self) noexcept {
        FIBER_ASSERT(self->loop_.in_loop());
        for (const auto &slot: self->slots_) {
            slot->worker->drain();
            if (slot->drain_timeout <= std::chrono::milliseconds::zero()) {
                slot->worker->abort();
                continue;
            }
            if (slot->drain_timeout == std::chrono::milliseconds::max()) {
                continue;
            }
            self->loop_.post_at<Slot, &Slot::deadline, &Worker::on_deadline>(self->loop_.now() + slot->drain_timeout,
                                                                             *slot);
        }
        async::spawn(self->loop_, [self]() -> async::DetachedTask { return run_wait_stop(self); });
    }

    // The graceful budget is spent: force the endpoint's remaining connections
    // down. wait_stopped() then completes on its own.
    static void on_deadline(Slot *slot) noexcept { slot->worker->abort(); }

    static async::DetachedTask run_wait_stop(Worker *self) noexcept {
        for (const auto &slot: self->slots_) {
            co_await slot->worker->wait_stopped();
            if (slot->deadline.is_in_heap()) {
                self->loop_.cancel<Slot, &Slot::deadline>(*slot);
            }
        }
        // Loop-affine resources are released here, on their own loop.
        self->slots_.clear();
        self->server_->on_worker_stopped();
        co_return;
    }

    Server *server_;
    event::EventLoop &loop_;
    std::size_t index_;
    std::vector<std::unique_ptr<Slot>> slots_{};
    event::EventLoop::NotifyEntry stop_entry_{};
    std::atomic<bool> stop_posted_{false};
};

Server::Server(event::EventLoop &loop, HttpHandler default_handler, event::EventLoopGroup *workers) :
    owner_loop_(loop), worker_group_(workers), default_handler_(std::move(default_handler)) {
    // The terminal barrier is armed up front so a serve() that starts before
    // any stop() has a pending count to wait on.
    shutdown_wg_.add();
}

Server::~Server() {
    const State current = state_.load(std::memory_order_acquire);
    FIBER_ASSERT(current == State::Created || current == State::Stopped);
    if (current == State::Created) {
        // Never started, so nothing ever released the barrier.
        shutdown_wg_.done();
    }
}

std::size_t Server::worker_count() const noexcept {
    if (worker_group_ == nullptr || worker_group_->size() == 0) {
        return 1;
    }
    return worker_group_->size();
}

event::EventLoop &Server::worker_loop(std::size_t index) const noexcept {
    if (worker_group_ == nullptr || worker_group_->size() == 0) {
        FIBER_ASSERT(index == 0);
        return owner_loop_;
    }
    return worker_group_->at(index);
}

common::IoResult<void> Server::start() noexcept {
    {
        std::lock_guard guard(lifecycle_mu_);
        if (state_.load(std::memory_order_relaxed) != State::Created) {
            return std::unexpected(common::IoErr::Already);
        }
        if (endpoints_.empty()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        state_.store(State::Started, std::memory_order_release);
    }

    std::size_t started = 0;
    for (; started < endpoints_.size(); ++started) {
        auto bound = endpoints_[started]->on_start(*this);
        if (!bound) {
            rollback_start(started);
            return std::unexpected(bound.error());
        }
    }

    const std::size_t count = worker_count();
    workers_.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        auto *worker = new (std::nothrow) Worker(*this, worker_loop(index), index);
        if (worker == nullptr) {
            rollback_start(started);
            return std::unexpected(common::IoErr::NoMem);
        }
        workers_.push_back(std::unique_ptr<Worker>(worker));

        for (const auto &endpoint: endpoints_) {
            EndpointWorker *slot = endpoint->create_worker(worker->loop(), index);
            if (slot == nullptr || !worker->install(slot, endpoint->drain_timeout())) {
                delete slot;
                rollback_start(started);
                return std::unexpected(common::IoErr::NoMem);
            }
        }
    }
    return {};
}

void Server::rollback_start(std::size_t started) noexcept {
    // Nothing has run on the worker loops yet, so the slots can be dropped
    // right here on the startup thread.
    for (const auto &worker: workers_) {
        worker->discard_slots();
    }
    workers_.clear();

    for (std::size_t i = started; i > 0; --i) {
        endpoints_[i - 1]->on_stop();
    }

    std::lock_guard guard(lifecycle_mu_);
    // A concurrent stop() may already have moved us to Draining and posted the
    // owner-loop teardown; leave that path alone, it drives us to Stopped.
    State expected = State::Started;
    (void) state_.compare_exchange_strong(expected, State::Created, std::memory_order_acq_rel,
                                          std::memory_order_relaxed);
}

async::Task<void> Server::serve() noexcept {
    FIBER_ASSERT(owner_loop_.in_loop());

    bool run_accept_loops = false;
    {
        std::lock_guard guard(lifecycle_mu_);
        const State current = state_.load(std::memory_order_relaxed);
        // serve() before start() would wait on a barrier nothing can release.
        FIBER_ASSERT_MSG(current != State::Created, "Server::serve() called before start()");
        if (current == State::Started) {
            state_.store(State::Serving, std::memory_order_release);
            run_accept_loops = true;
        }
    }

    if (run_accept_loops) {
        for (const auto &endpoint: endpoints_) {
            Endpoint *raw = endpoint.get();
            serve_wg_.add();
            async::spawn(owner_loop_, [this, raw]() -> async::DetachedTask { return run_endpoint_serve(this, raw); });
        }
    }

    co_await shutdown_wg_.join();
    co_return;
}

async::DetachedTask Server::run_endpoint_serve(Server *self, Endpoint *endpoint) noexcept {
    struct ServeGuard {
        Server *server;
        ~ServeGuard() { server->serve_wg_.done(); }
    } guard{self};

    co_await endpoint->on_serve();
    co_return;
}

void Server::stop() noexcept {
    bool release_barrier = false;
    {
        std::lock_guard guard(lifecycle_mu_);
        const State current = state_.load(std::memory_order_relaxed);
        if (current == State::Draining || current == State::Stopped) {
            return;
        }
        if (current == State::Created) {
            // Nothing was ever bound; go straight to the terminal state.
            state_.store(State::Stopped, std::memory_order_release);
            release_barrier = true;
        } else {
            state_.store(State::Draining, std::memory_order_release);
        }
    }

    if (release_barrier) {
        shutdown_wg_.done();
        return;
    }

    if (owner_loop_.in_loop()) {
        on_owner_stop(this);
        return;
    }
    owner_loop_.post<Server, &Server::stop_entry_, &Server::on_owner_stop>(*this);
}

void Server::on_owner_stop(Server *self) noexcept {
    FIBER_ASSERT(self->owner_loop_.in_loop());

    for (const auto &endpoint: self->endpoints_) {
        endpoint->on_stop();
    }
    if (!self->workers_.empty()) {
        self->workers_wg_.add(self->workers_.size());
        for (const auto &worker: self->workers_) {
            worker->notify_stop();
        }
    }
    async::spawn(self->owner_loop_, [self]() -> async::DetachedTask { return finish_shutdown(self); });
}

async::DetachedTask Server::finish_shutdown(Server *self) noexcept {
    co_await self->serve_wg_.join();
    co_await self->workers_wg_.join();

    // Workers first: an EndpointWorker may reach back into its Endpoint.
    self->workers_.clear();
    self->endpoints_.clear();
    self->state_.store(State::Stopped, std::memory_order_release);
    self->shutdown_wg_.done();
    co_return;
}

async::Task<void> Server::stop_and_wait() noexcept {
    stop();
    co_await shutdown_wg_.join();
    co_return;
}

} // namespace fiber::http
