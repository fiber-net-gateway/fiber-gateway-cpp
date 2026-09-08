#ifndef FIBER_HTTP_HTTP1_SERVER_OPTIONS_H
#define FIBER_HTTP_HTTP1_SERVER_OPTIONS_H

#include <chrono>
#include <cstddef>

namespace fiber::http {

// HTTP/1 session policy. Timeouts and header sizing are independent of the
// corresponding HTTP/2 and HTTP/3 endpoint settings.
struct Http1ServerOptions {
    // Idle wait between requests on a kept-alive connection.
    std::chrono::seconds keep_alive_timeout{70};
    // Budget for reading one request head.
    std::chrono::seconds header_timeout{10};
    std::chrono::seconds write_timeout{30};
    // Request-header arena sizing (see Http1HeaderParseBufferOptions).
    std::size_t header_init_size = 8 * 1024;
    std::size_t header_large_size = 32 * 1024;
    std::size_t header_large_num = 4;
    // Read and discard an unread request body so the connection can be reused
    // instead of closed.
    bool drain_unread_body = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_SERVER_OPTIONS_H
