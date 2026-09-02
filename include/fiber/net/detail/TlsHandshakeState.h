#ifndef FIBER_NET_DETAIL_TLS_HANDSHAKE_STATE_H
#define FIBER_NET_DETAIL_TLS_HANDSHAKE_STATE_H

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../common/IoError.h"
#include "../TlsParams.h"

namespace fiber::net::detail {

struct TlsServerHandshakeState {
    const TlsServerParam *param = nullptr;
    const SocketAddress *remote_addr = nullptr;
    const SocketAddress *local_addr = nullptr;
    TlsTransportKind transport = TlsTransportKind::Tcp;
    common::IoErr callback_error = common::IoErr::None;
    std::size_t credential_count = 0;
    bool trust_store_set = false;
    bool session_id_context_set = false;
    TlsClientCertificateMode client_certificate_mode = TlsClientCertificateMode::None;
    std::array<std::uint8_t, 255> selected_alpn{};
    std::uint8_t selected_alpn_size = 0;

    void reset_for_callback() noexcept {
        callback_error = common::IoErr::None;
        credential_count = 0;
        trust_store_set = false;
        session_id_context_set = false;
        client_certificate_mode = TlsClientCertificateMode::None;
        selected_alpn_size = 0;
    }
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_TLS_HANDSHAKE_STATE_H
