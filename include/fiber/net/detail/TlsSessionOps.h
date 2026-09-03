#ifndef FIBER_NET_DETAIL_TLS_SESSION_OPS_H
#define FIBER_NET_DETAIL_TLS_SESSION_OPS_H

struct ssl_session_st;
typedef struct ssl_session_st SSL_SESSION;

namespace fiber::net::detail {

// Internal transport hook used to hand newly-issued TLS sessions to the
// transport-specific cache. It is intentionally not part of TlsClientParam.
struct TlsNewSessionOps {
    void *ctx = nullptr;
    bool (*store)(void *ctx, SSL_SESSION *session) noexcept = nullptr;
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_TLS_SESSION_OPS_H
