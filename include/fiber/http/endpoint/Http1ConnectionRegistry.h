#ifndef FIBER_HTTP_ENDPOINT_HTTP1_CONNECTION_REGISTRY_H
#define FIBER_HTTP_ENDPOINT_HTTP1_CONNECTION_REGISTRY_H

#include <cstddef>

#include "../../common/IntrusiveList.h"
#include "../Http1Connection.h"

namespace fiber::http {

// Per-loop registry of live HTTP/1 sessions. Connections link themselves in
// from their serve coroutine frames through their intrusive hooks, so
// registration allocates nothing and the list is only ever touched on its own
// loop -- no lock, unlike the mutex-guarded shared_ptr vector this replaces.
//
// Shared by Http1Endpoint and by Http2Endpoint, which also serves HTTP/1
// sessions when it negotiates down.
class Http1ConnectionRegistry {
public:
    void link(Http1Connection &connection) noexcept { connections_.push_back(connection); }
    void unlink(Http1Connection &connection) noexcept { connections_.erase(connection); }
    [[nodiscard]] bool empty() const noexcept { return connections_.empty(); }

    // Idle connections close immediately; busy ones finish the request they are
    // serving, answer with Connection: close, and then close. Nothing here cuts
    // a request short: a session ends when HTTP/1 says it is done, bounded by
    // the connection's own header/keep-alive/write timeouts.
    void drain_all() noexcept {
        for (Http1Connection *connection = connections_.front(); connection != nullptr;
             connection = connections_.next_of(*connection)) {
            connection->request_drain();
        }
    }

private:
    // Http1Connection is not standard-layout (it holds unique_ptr members), but
    // it is non-polymorphic, which is what container_of actually needs.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
    using ConnectionList = common::IntrusiveList<Http1Connection, offsetof(Http1Connection, worker_hook_)>;
#pragma GCC diagnostic pop

    ConnectionList connections_{};
};

} // namespace fiber::http

#endif // FIBER_HTTP_ENDPOINT_HTTP1_CONNECTION_REGISTRY_H
