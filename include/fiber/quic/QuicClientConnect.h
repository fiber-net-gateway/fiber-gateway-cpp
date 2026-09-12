#ifndef FIBER_QUIC_QUIC_CLIENT_CONNECT_H
#define FIBER_QUIC_QUIC_CLIENT_CONNECT_H

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../net/SocketAddress.h"
#include "QuicConnection.h"

struct ssl_session_st;
typedef struct ssl_session_st SSL_SESSION;

namespace fiber::net {
class TlsCredential;
class TrustStore;
} // namespace fiber::net

namespace fiber::quic {

// Session-cache contract for a QUIC client owner. The transport hands newly
// issued sessions and tokens to the owner through QuicConnection::Ops; the
// owner rebuilds the key from what it knows about the connection -- the
// transport never stores it.
//
// Identity of a resumable server: the TLS material and names a session was
// issued against, plus the address it was reached at.
struct QuicClientCacheKey {
    std::string_view server_name{};
    std::string_view verify_name{};
    net::SocketAddress remote_addr{};
    const net::TlsCredential *credential = nullptr;
    const net::TrustStore *trust_store = nullptr;
};

struct QuicClientCachedState {
    SSL_SESSION *session = nullptr;
    const std::uint8_t *token = nullptr;
    std::size_t token_len = 0;
    QuicTransportSettings remembered_transport{};
    bool has_remembered_transport = false;
};

struct QuicClientCacheOps {
    void *owner = nullptr;
    bool (*load)(void *owner, const QuicClientCacheKey &key, QuicClientCachedState &out) noexcept = nullptr;
    // Returning true transfers the callback's SSL_SESSION reference to the cache.
    bool (*store_session)(void *owner, const QuicClientCacheKey &key, SSL_SESSION *session,
                          const QuicTransportSettings &remembered_transport) noexcept = nullptr;
    void (*store_token)(void *owner, const QuicClientCacheKey &key, const std::uint8_t *token,
                        std::size_t token_len) noexcept = nullptr;
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_CLIENT_CONNECT_H
