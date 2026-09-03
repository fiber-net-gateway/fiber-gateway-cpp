#ifndef FIBER_NET_DETAIL_TLS_HANDSHAKE_STATE_H
#define FIBER_NET_DETAIL_TLS_HANDSHAKE_STATE_H

#include "../../common/IoError.h"
#include "../TlsParams.h"

namespace fiber::net::detail {

// Bridge between BoringSSL's C callbacks (whose only context channel is the
// SSL's ex_data) and the fiber world: the borrowed caller param plus the error
// surfaced by the configure callback. Owned by whoever drives the handshake —
// a coroutine-frame local for TCP (TlsStreamFd), a session member for QUIC
// (QuicTlsSession, whose handshake spans many drive_handshake calls) — and
// wired to the SSL via TlsRuntime::set_server_handshake_state() for the
// handshake's duration. The param (including the storage its alpn span points
// into and the pointees of configure_ctx / trust_store) must stay valid until
// the handshake completes.
struct TlsServerHandshakeState {
    const TlsServerParam *param = nullptr;
    common::IoErr callback_error = common::IoErr::None;

    // Per-ClientHello bookkeeping. May run more than once (HelloRetryRequest).
    void reset_for_callback() noexcept { callback_error = common::IoErr::None; }
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_TLS_HANDSHAKE_STATE_H
