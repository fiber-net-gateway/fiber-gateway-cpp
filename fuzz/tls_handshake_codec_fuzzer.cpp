// Fuzzes every TLS handshake message decoder (the pre-authentication parsing
// surface). Input: [selector][message body]. After a successful decode the
// follow-up parsers the engines apply to the decoded views run too, and every
// returned span is read in full so an out-of-bounds view trips ASan here
// rather than deep inside an engine.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <fiber/tls/handshake/TlsCipherSuites.h>
#include <fiber/tls/handshake/TlsExtensionCodec.h>
#include <fiber/tls/handshake/TlsHandshakeCodec.h>
#include <fiber/tls/handshake/TlsHandshakeMessage.h>
#include <fiber/tls/record/TlsRecord.h>

namespace {

using namespace fiber::tls;

volatile std::uint8_t g_sink = 0;

void touch(std::span<const std::uint8_t> bytes) {
    std::uint8_t acc = 0;
    for (const std::uint8_t b: bytes) {
        acc ^= b;
    }
    g_sink = g_sink ^ acc;
}

void touch(std::string_view s) { touch({reinterpret_cast<const std::uint8_t *>(s.data()), s.size()}); }

void client_hello(const std::uint8_t *data, std::size_t size) {
    TlsClientHello ch;
    if (!tls_decode_client_hello(data, size, ch).has_value()) {
        return;
    }
    touch(ch.random);
    touch(ch.session_id);
    touch(ch.cipher_suites);
    touch(ch.compression_methods);
    touch(ch.extensions_block);
    touch(ch.server_name);
    touch(ch.supported_versions);
    touch(ch.supported_groups);
    touch(ch.signature_algorithms);
    touch(ch.signature_algorithms_cert);
    touch(ch.key_share_entries);
    touch(ch.alpn_list);
    for (const TlsNamedGroup group: {TlsNamedGroup::X25519, TlsNamedGroup::Secp256r1, TlsNamedGroup::Secp384r1}) {
        TlsKeyShareView share;
        if (tls_find_client_key_share(ch.key_share_entries, group, share).value_or(false)) {
            touch(share.key_exchange);
        }
    }
    if (ch.has_pre_shared_key) {
        touch(ch.psk_identities);
        touch(ch.psk_binders);
        for (std::size_t i = 0; i < 4; ++i) {
            TlsPskIdentityView identity;
            if (tls_psk_identity_at(ch.psk_identities, i, identity).value_or(false)) {
                touch(identity.identity);
            }
            std::span<const std::uint8_t> binder;
            if (tls_psk_binder_at(ch.psk_binders, i, binder).value_or(false)) {
                touch(binder);
            }
        }
    }
    (void) tls_psk_modes_contains(ch.psk_key_exchange_modes, kTlsPskModePskDheKe);
    std::span<const std::uint8_t> payload;
    if (tls_find_extension_payload(ch.extensions_block, TlsExtensionType::QuicTransportParameters, payload)) {
        touch(payload);
    }
    const TlsClientHelloView view = ch.view();
    touch(view.server_name);
}

void server_hello(const std::uint8_t *data, std::size_t size) {
    TlsServerHello sh;
    if (!tls_decode_server_hello(data, size, sh).has_value()) {
        return;
    }
    touch(sh.random);
    touch(sh.session_id);
    touch(sh.key_share);
    touch(sh.alpn);
    touch(sh.renegotiation_info);
    touch(sh.cookie);
    touch(sh.extensions_block);
    (void) tls_is_hello_retry_request(sh.random);
}

void encrypted_extensions(const std::uint8_t *data, std::size_t size) {
    TlsEncryptedExtensions ee;
    if (tls_decode_encrypted_extensions(data, size, ee).has_value()) {
        touch(ee.alpn);
        touch(ee.extensions_block);
    }
}

void certificate_13(const std::uint8_t *data, std::size_t size) {
    TlsCertificate13 cert;
    if (tls_decode_certificate_13(data, size, cert).has_value()) {
        touch(cert.certificate_request_context);
        for (std::size_t i = 0; i < cert.cert_count; ++i) {
            touch(cert.certs[i]);
        }
    }
}

void certificate_12(const std::uint8_t *data, std::size_t size) {
    TlsCertificate12 cert;
    if (tls_decode_certificate_12(data, size, cert).has_value()) {
        for (std::size_t i = 0; i < cert.cert_count; ++i) {
            touch(cert.certs[i]);
        }
    }
}

void certificate_request_13(const std::uint8_t *data, std::size_t size) {
    TlsCertificateRequest13 cr;
    if (tls_decode_certificate_request_13(data, size, cr).has_value()) {
        touch(cr.certificate_request_context);
        touch(cr.signature_algorithms);
    }
}

void certificate_request_12(const std::uint8_t *data, std::size_t size) {
    TlsCertificateRequest12 cr;
    if (tls_decode_certificate_request_12(data, size, cr).has_value()) {
        touch(cr.certificate_types);
        touch(cr.signature_algorithms);
        touch(cr.certificate_authorities);
    }
}

void certificate_verify(const std::uint8_t *data, std::size_t size) {
    TlsCertificateVerify cv;
    if (tls_decode_certificate_verify(data, size, cv).has_value()) {
        touch(cv.signature);
    }
}

void finished(const std::uint8_t *data, std::size_t size) {
    TlsFinished fin;
    if (tls_decode_finished(data, size, fin).has_value()) {
        touch(fin.verify_data);
    }
}

void server_key_exchange(const std::uint8_t *data, std::size_t size) {
    TlsServerKeyExchange ske;
    if (tls_decode_server_key_exchange(data, size, ske).has_value()) {
        touch(ske.public_key);
        touch(ske.signature);
    }
}

void client_key_exchange(const std::uint8_t *data, std::size_t size) {
    TlsClientKeyExchange cke;
    if (tls_decode_client_key_exchange(data, size, cke).has_value()) {
        touch(cke.public_key);
    }
}

void headers(const std::uint8_t *data, std::size_t size) {
    if (size >= kTlsHandshakeHeaderSize) {
        (void) tls_decode_handshake_header(data, size);
    }
    if (size >= kTlsRecordHeaderSize) {
        (void) tls_decode_record_header(data);
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size < 1) {
        return 0;
    }
    const std::uint8_t selector = data[0];
    const std::uint8_t *body = data + 1;
    const std::size_t len = size - 1;
    switch (selector % 13) {
        case 0:
            client_hello(body, len);
            break;
        case 1:
            server_hello(body, len);
            break;
        case 2:
            encrypted_extensions(body, len);
            break;
        case 3:
            certificate_13(body, len);
            break;
        case 4:
            certificate_12(body, len);
            break;
        case 5:
            certificate_request_13(body, len);
            break;
        case 6:
            certificate_request_12(body, len);
            break;
        case 7:
            certificate_verify(body, len);
            break;
        case 8:
            finished(body, len);
            break;
        case 9:
            server_key_exchange(body, len);
            break;
        case 10:
            client_key_exchange(body, len);
            break;
        case 11:
            headers(body, len);
            break;
        default:
            client_hello(body, len);
            break; // the widest surface gets two slots
    }
    return 0;
}
