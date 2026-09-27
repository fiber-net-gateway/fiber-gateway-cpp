// Fuzzes the TLS client handshake engine with an arbitrary server byte
// stream: ServerHello/HRR processing (version, suite, group, session-id echo,
// downgrade sentinel, PSK acceptance), alerts and CCS in the plaintext window,
// and the whole plaintext TLS 1.2 server flight — Certificate chain parsing
// and verification, ServerKeyExchange, CertificateRequest, ServerHelloDone.
//
// Input: [control][server bytes...]. control bits 0-5 = feed chunk size
// (0 = one feed), bit 6 = TLS 1.2-only client, bit 7 = offer a resumption
// session with 0-RTT.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "TlsFuzzCommon.h"

#include <fiber/tls/handshake/TlsClientHandshakeEngine.h>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size < 1) {
        return 0;
    }
    static const fiber::fuzz::ClientMaterial material;
    static constexpr std::array<std::uint8_t, 16> kTicket{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    static constexpr std::array<std::uint8_t, 32> kPsk{0x42};
    const std::uint8_t control = data[0];
    const std::span<const std::uint8_t> stream(data + 1, size - 1);

    fiber::fuzz::run_in_loop([&] {
        fiber::tls::TlsClientConfig cfg = material.config();
        if ((control & 0x40) != 0) {
            cfg.max_version = fiber::tls::kTlsVersionTls12;
        }
        fiber::tls::TlsSessionOffer offer;
        offer.identity = kTicket;
        offer.psk = kPsk;
        offer.max_early_data = 1024;
        const bool resume = (control & 0x80) != 0 && (control & 0x40) == 0;
        fiber::tls::TlsClientHandshakeEngine engine(cfg, resume ? &offer : nullptr);
        if (!engine.done() && resume) {
            const std::uint8_t early[] = {'G', 'E', 'T', ' ', '/'};
            (void) engine.write_early_data(early);
        }
        (void) engine.take_output(); // the ClientHello
        const std::size_t chunk = (control & 0x3F) == 0 ? stream.size() : (control & 0x3F);
        std::size_t off = 0;
        while (off < stream.size() && !engine.done()) {
            const std::size_t n = std::min(chunk, stream.size() - off);
            if (!engine.feed(fiber::fuzz::to_iobuf(stream.subspan(off, n))).has_value()) {
                break; // NoMem
            }
            (void) engine.take_output();
            off += n;
        }
        (void) engine.take_output();
        if (engine.done() && !engine.failed()) {
            (void) engine.take_state();
            (void) engine.take_inbound_leftover();
        }
    });
    return 0;
}
