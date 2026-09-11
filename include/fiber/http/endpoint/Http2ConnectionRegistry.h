#ifndef FIBER_HTTP_ENDPOINT_HTTP2_CONNECTION_REGISTRY_H
#define FIBER_HTTP_ENDPOINT_HTTP2_CONNECTION_REGISTRY_H

#include <cstddef>

#include "../../common/IntrusiveList.h"
#include "../Http2ServerConnection.h"

namespace fiber::http {

// Per-loop registry of live HTTP/2 server sessions, linked in from their serve
// coroutine frames. Only ever walked or mutated on its own loop, so no lock.
class Http2ConnectionRegistry {
public:
    void link(Http2ServerConnection &connection) noexcept { connections_.push_back(connection); }
    void unlink(Http2ServerConnection &connection) noexcept { connections_.erase(connection); }
    [[nodiscard]] bool empty() const noexcept { return connections_.empty(); }

    // GOAWAY every session: the peer opens no new streams and the ones already
    // running finish, after which each connection closes itself.
    void drain_all() noexcept {
        // The callback must not assume the node survives: read the next
        // pointer first so a connection that unlinks and destroys itself
        // synchronously cannot leave this traversal reading freed memory.
        Http2ServerConnection *connection = connections_.front();
        while (connection != nullptr) {
            Http2ServerConnection *next = connections_.next_of(*connection);
            connection->request_drain();
            connection = next;
        }
    }

private:
    // Http2ServerConnection is not standard-layout (it holds an Http2Connection
    // by value), but it is non-polymorphic, which is what container_of needs.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
    using ConnectionList = common::IntrusiveList<Http2ServerConnection, offsetof(Http2ServerConnection, worker_hook_)>;
#pragma GCC diagnostic pop

    ConnectionList connections_{};
};

} // namespace fiber::http

#endif // FIBER_HTTP_ENDPOINT_HTTP2_CONNECTION_REGISTRY_H
