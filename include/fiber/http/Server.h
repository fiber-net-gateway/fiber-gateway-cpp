#ifndef FIBER_HTTP_SERVER_H
#define FIBER_HTTP_SERVER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include "../async/Spawn.h"
#include "../async/Task.h"
#include "../async/WaitGroup.h"
#include "../common/Assert.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "../event/EventLoopGroup.h"
#include "../net/SocketAddress.h"
#include "HttpExchange.h"

namespace fiber::http {

class Server;

// One endpoint's presence on a single worker loop: it owns every connection
// that endpoint has on that loop and knows how to wind them down.
//
// Constructed on the startup thread (see Endpoint::create_worker). Every other
// member function, and the destructor, run on the worker's own event loop.
class EndpointWorker : public common::NonCopyable, public common::NonMovable {
public:
    virtual ~EndpointWorker() = default;

    // Stop admitting new work and ask live connections to wind down:
    //   HTTP/1 -> close idle connections now; mark busy ones so they close
    //             after the current exchange (response carries Connection: close)
    //   HTTP/2 -> GOAWAY, already-open streams run to completion
    //   HTTP/3 -> refuse new connections, GOAWAY the live ones
    // Idempotent.
    //
    // There is no shutdown budget and no forced teardown: when a request must
    // run to completion, cutting it off is worse than shutting down slowly, so
    // the protocol decides when a connection is finished. Each protocol layer
    // is therefore responsible for making sure its connections do terminate --
    // that is what its own read/write/idle timeouts are for.
    virtual void drain() noexcept = 0;

    // Completes once every resource this worker holds for the endpoint is
    // gone. Awaited exactly once by Server::Worker, but implementations should
    // not rely on that.
    [[nodiscard]] virtual async::Task<void> wait_stopped() noexcept = 0;
};

// One listening address plus the protocol policy, options and handler that go
// with it. Lives on the server's owner loop.
class Endpoint : public common::NonCopyable, public common::NonMovable {
public:
    virtual ~Endpoint() = default;

    // ---- Called once each on the owner loop ----

    // Acquires the listening resources. A failure fails Server::start() as a
    // whole; endpoints already started are rolled back with on_stop().
    [[nodiscard]] virtual common::IoResult<void> on_start(Server &server) noexcept = 0;

    // The accept / receive loop. Returns once the listener is closed.
    [[nodiscard]] virtual async::Task<void> on_serve() noexcept = 0;

    // Closes the listener: no new connections after this point. Idempotent.
    virtual void on_stop() noexcept = 0;

    // ---- Called once per worker during start(), on the startup thread ----

    // Returns nullptr on allocation failure, which fails start(). Ownership
    // passes to the server; the object is destroyed on `loop`.
    [[nodiscard]] virtual EndpointWorker *create_worker(event::EventLoop &loop, std::size_t index) noexcept = 0;

    // Valid after a successful on_start(); reflects the kernel-assigned port
    // when the configured port was 0.
    [[nodiscard]] virtual const net::SocketAddress &local_addr() const noexcept = 0;
};

// Owns a set of endpoints and the shutdown choreography around them.
//
// Threading: add_endpoint() and start() must not run concurrently with the
// owner loop's callbacks — call them either on the owner loop or on the
// startup thread before that loop is running. serve() must be awaited on the
// owner loop. stop(), stop_and_wait() and the observers are safe from any
// thread.
//
// The server neither owns nor stops the EventLoopGroup it was handed. Once
// serve() (or stop_and_wait()) has completed, every listener, connection and
// per-worker context is gone and the caller may stop and join the loops.
//
// Shutdown has no deadline: stop() closes the listeners and asks each endpoint
// to drain, then waits for the protocols to finish on their own terms.
class Server : public common::NonCopyable, public common::NonMovable {
public:
    enum class State : std::uint8_t {
        Created, // accepting add_endpoint()
        Started, // endpoints bound, not serving yet
        Serving,
        Draining, // listeners closed, waiting for connections to wind down
        Stopped, // every resource released
    };

    Server(event::EventLoop &loop, HttpHandler default_handler, event::EventLoopGroup *workers = nullptr);
    ~Server();

    // Constructs an endpoint in place and takes ownership of it. Only valid in
    // the Created state; returns nullptr on allocation failure or if the
    // server has already been started. The returned pointer stays valid until
    // the server reaches Stopped.
    template<class E, class... Args>
    [[nodiscard]] E *add_endpoint(Args &&...args) noexcept {
        static_assert(std::is_base_of_v<Endpoint, E>, "endpoint must derive from fiber::http::Endpoint");
        if (state_.load(std::memory_order_acquire) != State::Created) {
            return nullptr;
        }
        auto *endpoint = new (std::nothrow) E(std::forward<Args>(args)...);
        if (endpoint == nullptr) {
            return nullptr;
        }
        endpoints_.push_back(std::unique_ptr<Endpoint>(endpoint));
        return endpoint;
    }

    // Binds every endpoint and builds the per-worker contexts. On failure
    // nothing is left bound and the server returns to Created (unless a
    // concurrent stop() already moved it to Draining).
    [[nodiscard]] common::IoResult<void> start() noexcept;

    // Runs the endpoints' accept loops and completes only after shutdown has
    // fully finished. Await this on the owner loop.
    [[nodiscard]] async::Task<void> serve() noexcept;

    // Requests shutdown. Non-blocking, idempotent, callable from any thread.
    void stop() noexcept;

    // stop() plus the same completion barrier serve() waits on. Several
    // waiters (including serve()) may await concurrently.
    [[nodiscard]] async::Task<void> stop_and_wait() noexcept;

    [[nodiscard]] State state() const noexcept { return state_.load(std::memory_order_acquire); }
    // True once shutdown has begun: accept loops use it to drop connections
    // they are still holding.
    [[nodiscard]] bool draining() const noexcept {
        const State current = state();
        return current == State::Draining || current == State::Stopped;
    }

    [[nodiscard]] event::EventLoop &owner_loop() const noexcept { return owner_loop_; }
    [[nodiscard]] std::size_t worker_count() const noexcept;
    [[nodiscard]] event::EventLoop &worker_loop(std::size_t index) const noexcept;
    [[nodiscard]] const HttpHandler &default_handler() const noexcept { return default_handler_; }
    [[nodiscard]] std::size_t endpoint_count() const noexcept { return endpoints_.size(); }

private:
    class Worker;

    static void on_owner_stop(Server *self) noexcept;
    static async::DetachedTask finish_shutdown(Server *self) noexcept;
    static async::DetachedTask run_endpoint_serve(Server *self, Endpoint *endpoint) noexcept;

    // Undoes a partial start(): destroys the worker contexts built so far and
    // stops the first `started` endpoints in reverse order.
    void rollback_start(std::size_t started) noexcept;
    void on_worker_stopped() noexcept { workers_wg_.done(); }

    event::EventLoop &owner_loop_;
    event::EventLoopGroup *worker_group_ = nullptr;
    HttpHandler default_handler_;
    std::vector<std::unique_ptr<Endpoint>> endpoints_{};
    std::vector<std::unique_ptr<Worker>> workers_{};

    mutable std::mutex lifecycle_mu_{};
    std::atomic<State> state_{State::Created};

    async::WaitGroup serve_wg_{}; // one per running endpoint accept loop
    async::WaitGroup workers_wg_{}; // one per worker that has yet to stop
    async::WaitGroup shutdown_wg_{}; // terminal barrier, armed in the constructor
    event::EventLoop::NotifyEntry stop_entry_{};
};

} // namespace fiber::http

#endif // FIBER_HTTP_SERVER_H
