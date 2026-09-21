#ifndef FIBER_TLS_HANDSHAKE_TLS_HANDSHAKE_CODEC_H
#define FIBER_TLS_HANDSHAKE_TLS_HANDSHAKE_CODEC_H

#include <cstddef>
#include <cstdint>

#include "../../common/IoError.h"
#include "TlsHandshakeMessage.h"

namespace fiber::tls {

// Decodes a ClientHello message body (the bytes AFTER the 4-byte handshake
// header; length == header.length). Structural validation only — version
// negotiation and policy checks belong to the server FSM. Fails with
// IoErr::Invalid on any RFC 8446 §4.1.2 / §4.2 malformation: field lengths,
// inexact list walks, trailing bytes, duplicate extensions, empty-payload
// violations, pre_shared_key not placed last, identity/binder count mismatch.
// `out` is only mutated on success.
[[nodiscard]] common::IoResult<void> tls_decode_client_hello(const std::uint8_t *body, std::size_t len,
                                                             TlsClientHello &out) noexcept;

} // namespace fiber::tls

#endif // FIBER_TLS_HANDSHAKE_TLS_HANDSHAKE_CODEC_H
