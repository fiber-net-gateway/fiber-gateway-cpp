// Fuzzes the post-handshake TLS connection (TlsConnection) with records the
// harness SEALS under the peer's real keys, so the fuzzer reaches what random
// ciphertext never could: inner content types and 1.3 padding, alerts
// (warning budget, close_notify), post-handshake handshake reassembly
// (NewSessionTicket, KeyUpdate, refused messages), plaintext-length bounds,
// and the write path's owed-KeyUpdate response. Raw records exercise framing.
//
// Input: [config][record...]
//   config bit 0: TLS 1.2 (else 1.3); bit 1: our side is the server.
//   record: [ctl][len_hi][len_lo][payload (len bytes, truncated at the end)]
//     ctl & 0xC0 == 0xC0 -> payload fed raw (unsealed framing bytes)
//     otherwise sealed; inner content type = ctl & 0x07:
//       0 CCS, 1 alert, 2 handshake, 3 application data, 4-7 -> (ctl >> 3) & 0x1F
//     ctl & 0x40 -> after the record, our side writes app data
//     ctl & 0x80 -> after the record, drain our side's plaintext

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include "TlsFuzzCommon.h"

#include <fiber/tls/TlsConnectedState.h>
#include <fiber/tls/TlsConnection.h>
#include <fiber/tls/crypto/Tls12KeySchedule.h>
#include <fiber/tls/crypto/Tls13KeySchedule.h>
#include <fiber/tls/crypto/TlsSecret.h>
#include <fiber/tls/record/TlsRecord.h>
#include <fiber/tls/record/TlsRecordCipher.h>
#include <fiber/tls/record/TlsRecordReader.h>

namespace {

using namespace fiber::tls;

bool init_cipher(TlsRecordCipher &cipher, TlsCipherSuiteId suite, TlsRecordProtectionKind kind,
                 TlsRecordDirection direction, const TlsTrafficKeys &keys) {
    return cipher.init(suite, kind, direction, {keys.key.data(), keys.key_len}, {keys.iv.data(), keys.iv_len})
            .has_value();
}

TlsSecret fixed_secret(std::uint8_t seed, std::size_t len) {
    std::array<std::uint8_t, 48> bytes{};
    for (std::size_t i = 0; i < len; ++i) {
        bytes[i] = static_cast<std::uint8_t>(seed + i * 7);
    }
    return TlsSecret::from_bytes({bytes.data(), len});
}

// Our connected state plus the peer's write side (cipher + 1.3 secret for
// mirroring the KeyUpdates it sends).
struct Setup {
    TlsConnectedState state;
    TlsRecordCipher peer_write;
    TlsSecret peer_secret{};
    TlsCipherSuiteId suite{};
    bool tls13 = true;
};

bool make_setup(bool tls12, bool server, Setup &out) {
    if (!tls12) {
        out.tls13 = true;
        out.suite = TlsCipherSuiteId::TlsAes128GcmSha256;
        const TlsSecret client_app = fixed_secret(0x11, 32);
        const TlsSecret server_app = fixed_secret(0x77, 32);
        auto client_keys = tls13_traffic_keys(client_app, out.suite);
        auto server_keys = tls13_traffic_keys(server_app, out.suite);
        if (!client_keys || !server_keys) {
            return false;
        }
        const TlsTrafficKeys &ours = server ? *server_keys : *client_keys;
        const TlsTrafficKeys &peers = server ? *client_keys : *server_keys;
        out.state.version = TlsProtocolVersion::Tls13;
        out.state.suite = out.suite;
        // TlsSecret is move-only: copies go through the bytes.
        out.state.client_app_secret = TlsSecret::from_bytes(client_app.bytes());
        out.state.server_app_secret = TlsSecret::from_bytes(server_app.bytes());
        out.peer_secret = TlsSecret::from_bytes((server ? client_app : server_app).bytes());
        return init_cipher(out.state.write_cipher, out.suite, TlsRecordProtectionKind::Tls13, TlsRecordDirection::Seal,
                           ours) &&
               init_cipher(out.state.read_cipher, out.suite, TlsRecordProtectionKind::Tls13, TlsRecordDirection::Open,
                           peers) &&
               init_cipher(out.peer_write, out.suite, TlsRecordProtectionKind::Tls13, TlsRecordDirection::Seal, peers);
    }
    out.tls13 = false;
    out.suite = TlsCipherSuiteId::EcdheRsaAes128GcmSha256;
    const TlsSecret master = fixed_secret(0x33, 48);
    std::array<std::uint8_t, 32> client_random{};
    std::array<std::uint8_t, 32> server_random{};
    client_random.fill(0x01);
    server_random.fill(0x02);
    auto block = tls12_key_block(out.suite, master, client_random, server_random);
    if (!block) {
        return false;
    }
    const TlsTrafficKeys &ours = server ? block->server : block->client;
    const TlsTrafficKeys &peers = server ? block->client : block->server;
    out.state.version = TlsProtocolVersion::Tls12;
    out.state.suite = out.suite;
    return init_cipher(out.state.write_cipher, out.suite, TlsRecordProtectionKind::Tls12, TlsRecordDirection::Seal,
                       ours) &&
           init_cipher(out.state.read_cipher, out.suite, TlsRecordProtectionKind::Tls12, TlsRecordDirection::Open,
                       peers) &&
           init_cipher(out.peer_write, out.suite, TlsRecordProtectionKind::Tls12, TlsRecordDirection::Seal, peers);
}

std::vector<std::uint8_t> seal(Setup &setup, std::uint8_t inner, std::span<const std::uint8_t> plain) {
    // The u16 record length bounds the sealable plaintext; oversized
    // plaintexts (up to that bound) are kept to reach record_overflow.
    const std::size_t cap = 0xFFFF - 64;
    if (plain.size() > cap) {
        plain = plain.first(cap);
    }
    const std::size_t sealed = setup.peer_write.seal_output_size(plain.size());
    std::vector<std::uint8_t> out(kTlsRecordHeaderSize + sealed);
    const std::uint8_t outer = setup.tls13 ? static_cast<std::uint8_t>(TlsContentType::ApplicationData) : inner;
    tls_encode_record_header(out.data(), static_cast<TlsContentType>(outer), 0x0303,
                             static_cast<std::uint16_t>(sealed));
    const auto result = setup.peer_write.seal(static_cast<TlsContentType>(inner), plain,
                                              {out.data() + kTlsRecordHeaderSize, sealed});
    if (result.status != TlsRecordCipher::Status::Ok) {
        return {};
    }
    out.resize(kTlsRecordHeaderSize + result.out_len);
    return out;
}

// A 1.3 peer sending a whole KeyUpdate message rotates its write keys right
// after it; mirror that so later records still open.
void mirror_key_update(Setup &setup, std::uint8_t inner, std::span<const std::uint8_t> plain) {
    if (!setup.tls13 || inner != static_cast<std::uint8_t>(TlsContentType::Handshake) || plain.size() != 5 ||
        plain[0] != static_cast<std::uint8_t>(TlsHandshakeType::KeyUpdate) || plain[1] != 0 || plain[2] != 0 ||
        plain[3] != 1 || plain[4] > 1) {
        return;
    }
    auto next = tls13_key_update(setup.peer_secret);
    if (!next) {
        return;
    }
    auto keys = tls13_traffic_keys(*next, setup.suite);
    TlsRecordCipher fresh;
    if (keys && init_cipher(fresh, setup.suite, TlsRecordProtectionKind::Tls13, TlsRecordDirection::Seal, *keys)) {
        setup.peer_secret = std::move(*next);
        setup.peer_write = std::move(fresh);
    }
}

void drain_plaintext(TlsConnection &conn) {
    std::array<std::uint8_t, 4096> buf{};
    for (int i = 0; i < 64; ++i) {
        std::size_t n = 0;
        if (conn.read(buf.data(), buf.size(), n) != TlsConnection::ReadStatus::Ok || n == 0) {
            return;
        }
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size < 1) {
        return 0;
    }
    const std::uint8_t config = data[0];
    fiber::fuzz::run_in_loop([&] {
        Setup setup;
        const bool server = (config & 0x02) != 0;
        if (!make_setup((config & 0x01) != 0, server, setup)) {
            return;
        }
        TlsConnection conn(server ? TlsConnectionRole::Server : TlsConnectionRole::Client, std::move(setup.state));
        TlsRecordReader reader;
        std::size_t off = 1;
        while (off + 3 <= size && !conn.failed() && !conn.peer_closed()) {
            const std::uint8_t ctl = data[off];
            const std::size_t want = (static_cast<std::size_t>(data[off + 1]) << 8) | data[off + 2];
            off += 3;
            const std::size_t len = std::min(want, size - off);
            const std::span<const std::uint8_t> payload(data + off, len);
            off += len;

            std::vector<std::uint8_t> wire;
            if ((ctl & 0xC0) == 0xC0) {
                wire.assign(payload.begin(), payload.end());
            } else {
                const std::uint8_t kind = ctl & 0x07;
                const std::uint8_t inner = kind < 4 ? static_cast<std::uint8_t>(20 + kind) : ((ctl >> 3) & 0x1F);
                wire = seal(setup, inner, payload);
                mirror_key_update(setup, inner, payload);
            }
            if (!wire.empty() && !reader.feed(fiber::fuzz::to_iobuf(wire))) {
                break;
            }
            for (;;) {
                TlsRecordReader::Result next = reader.next();
                if (next.status == TlsRecordReader::Result::Status::Fatal) {
                    conn.on_framing_fatal(next.alert);
                    break;
                }
                if (next.status == TlsRecordReader::Result::Status::NeedMore) {
                    break;
                }
                conn.on_record(std::move(next.record));
                if (conn.failed() || conn.peer_closed()) {
                    break;
                }
            }
            if ((ctl & 0xC0) != 0xC0) {
                if ((ctl & 0x40) != 0) {
                    const std::uint8_t reply[] = {'o', 'k'};
                    (void) conn.write(reply);
                }
                if ((ctl & 0x80) != 0) {
                    drain_plaintext(conn);
                }
            }
            (void) conn.take_output();
        }
        drain_plaintext(conn);
        (void) conn.close_notify();
        (void) conn.take_output();
    });
    return 0;
}
