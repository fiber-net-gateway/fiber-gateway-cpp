// Fuzzes the TLS server handshake engine with an arbitrary client byte stream:
// record framing and reassembly, ClientHello decode, version/suite/group/ALPN
// negotiation, HRR, PSK ticket lookup and binder verification (tickets open —
// the seed corpus carries tickets from the harness's own ticket service),
// 0-RTT accept/skip windows, mTLS, and the 1.2 flight.
//
// Input: [control][client bytes...]. control bits 0-5 = feed chunk size
// (0 = one feed), bit 6 = require a client certificate, bit 7 = no ALPN.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include "TlsFuzzCommon.h"

#include "tls/handshake/TlsServerHandshakeEngine.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size < 1) {
        return 0;
    }
    static const fiber::fuzz::ServerMaterial material;
    const std::uint8_t control = data[0];
    const std::span<const std::uint8_t> stream(data + 1, size - 1);

    fiber::fuzz::run_in_loop([&] {
        fiber::tls::TlsServerConfig cfg = material.config();
        if ((control & 0x40) != 0) {
            material.with_client_auth(cfg);
        }
        if ((control & 0x80) != 0) {
            cfg.alpn = {};
        }
        fiber::tls::TlsServerHandshakeEngine engine(cfg, &material.lookup, &material.minter);
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
            // Unreachable without the peer's keys, but keep the terminal
            // contract honest if a seed ever completes.
            (void) engine.take_state();
            (void) engine.take_inbound_leftover();
        }
    });
    return 0;
}
