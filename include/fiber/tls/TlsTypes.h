#ifndef FIBER_TLS_TLS_TYPES_H
#define FIBER_TLS_TLS_TYPES_H

#include <cstdint>

namespace fiber::tls {

// Alert descriptions, RFC 8446 §6 (superset shared with RFC 5246 §7.2).
// Reserved values (decryption_failed 21, no_certificate 41, export_restriction
// 60) are omitted. Senders always pair these with a severity; the fatal /
// warning split is decided by the layer that raises the alert.
enum class TlsAlertDesc : std::uint8_t {
    CloseNotify = 0,
    UnexpectedMessage = 10,
    BadRecordMac = 20,
    RecordOverflow = 22,
    DecompressionFailure = 30,
    HandshakeFailure = 40,
    BadCertificate = 42,
    UnsupportedCertificate = 43,
    CertificateRevoked = 44,
    CertificateExpired = 45,
    CertificateUnknown = 46,
    IllegalParameter = 47,
    UnknownCa = 48,
    AccessDenied = 49,
    DecodeError = 50,
    DecryptError = 51,
    ProtocolVersion = 70,
    InternalError = 80,
    InappropriateFallback = 86,
    UserCanceled = 90,
    NoRenegotiation = 100,
    MissingExtension = 109,
    UnsupportedExtension = 110,
    CertificateUnobtainable = 111,
    UnrecognizedName = 112,
    BadCertificateStatusResponse = 113,
    BadCertificateHashValue = 114,
    UnknownPskIdentity = 115,
    CertificateRequired = 116,
    NoApplicationProtocol = 120,
};

enum class TlsAlertLevel : std::uint8_t {
    Warning = 1,
    Fatal = 2,
};

} // namespace fiber::tls

#endif // FIBER_TLS_TLS_TYPES_H
