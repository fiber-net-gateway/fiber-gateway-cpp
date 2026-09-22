#ifndef FIBER_TLS_TLS_CONNECTED_STATE_H
#define FIBER_TLS_TLS_CONNECTED_STATE_H

// Handshake output DTO (06 §5.3). Movable, not copyable. The two record
// ciphers arrive by MOVE from the engine with sequence numbers already past
// the handshake flight — record-protection continuity across the
// handshake→connection boundary is structural (05 move semantics: the client
// Finished rode the hs-traffic cipher to seq=N; 1.3 app records start from a
// fresh instance at seq=0, 1.2 keeps counting on the same key_block
// instances). Key material (app secrets / 1.2 master) stays here for
// KeyUpdate (09) and session cache / resumption (08); the cipher itself only
// ever receives key material and never hands it back.

#include <array>
#include <cstddef>
#include <cstdint>

#include "TlsVersion.h"
#include "crypto/TlsCertificate.h"
#include "crypto/TlsSecret.h"
#include "handshake/TlsCipherSuites.h"
#include "record/TlsRecordCipher.h"

namespace fiber::tls {

struct TlsConnectedState {
    TlsProtocolVersion version = TlsProtocolVersion::Tls13;
    TlsCipherSuiteId suite = TlsCipherSuiteId::TlsAes128GcmSha256;
    TlsRecordCipher read_cipher; // 1.3: server_app0 / 1.2: key_block server-write keys
    TlsRecordCipher write_cipher; // 1.3: client_app0 / 1.2: key_block client-write keys
    TlsSecret client_app_secret{}; // 1.3 KeyUpdate write-side base; empty on 1.2
    TlsSecret server_app_secret{}; // 1.3 KeyUpdate read-side base; empty on 1.2
    TlsSecret resumption_master{}; // 1.3 NST→PSK derivation base (08); empty on 1.2
    TlsSecret tls12_master{}; // 1.2 session cache / verify source (08); empty on 1.3
    std::array<std::uint8_t, 256> alpn{}; // selected protocol bytes; empty table when alpn_len == 0
    std::uint16_t alpn_len = 0;
    TlsCertificateChain peer_chain; // empty when the session resumed via PSK (moved in, owns its DER)
    bool session_resumed = false;
    bool early_data_accepted = false;
};

} // namespace fiber::tls

#endif // FIBER_TLS_TLS_CONNECTED_STATE_H
