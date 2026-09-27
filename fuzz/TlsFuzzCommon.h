#ifndef FIBER_FUZZ_TLS_FUZZ_COMMON_H
#define FIBER_FUZZ_TLS_FUZZ_COMMON_H

// Shared material for the TLS fuzz harnesses and the seed generator: a fixed
// clock, one server credential and trust store from the test fixtures, and a
// ticket service with a fixed key — the seed generator mints real tickets
// against the same service, so resumption seeds open in the harness and the
// fuzzer starts from the PSK/binder/0-RTT paths instead of rediscovering them.

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "../tests/TlsCertFixtures.h"

#include <fiber/async/Spawn.h>
#include <fiber/common/Assert.h>
#include <fiber/common/mem/IoBuf.h>
#include <fiber/event/EventLoop.h>
#include <fiber/tls/TlsConfig.h>
#include <fiber/tls/TlsTicketService.h>
#include <fiber/tls/crypto/TlsCertificate.h>
#include <fiber/tls/crypto/TlsSignature.h>

namespace fiber::fuzz {

namespace certfix = fiber::tls::certfix;

// The engines' clock input (certificate validity + ticket age), fixed so a
// seed replays identically.
inline constexpr std::int64_t kNowMs = certfix::kRefNowMs;

// IoBufChain nodes resolve the CURRENT thread's loop, which only exists
// inside EventLoop::run(). One loop serves every input: each call spawns the
// body and runs the loop until the body stops it.
template<typename F>
void run_in_loop(F &&body) {
    static fiber::event::EventLoop loop;
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        body();
        loop.stop();
        co_return;
    });
    loop.run();
}

// A copy of `bytes` as one IoBuf (the engines take ownership).
inline fiber::mem::IoBuf to_iobuf(std::span<const std::uint8_t> bytes) {
    fiber::mem::IoBuf buf = fiber::mem::IoBuf::allocate(bytes.empty() ? 1 : bytes.size());
    FIBER_ASSERT(buf.valid());
    if (!bytes.empty()) {
        std::memcpy(buf.writable_data(), bytes.data(), bytes.size());
    }
    buf.commit(bytes.size());
    return buf;
}

// Server: RSA leaf + intermediate, tickets on, 0-RTT on. Client-certificate
// trust is opt-in (with_client_auth): a client_trust config disables 1.2
// resumption by design, so the default leaves it unset.
struct ServerMaterial {
    std::string chain_pem;
    std::optional<fiber::tls::TlsCertificateChain> chain;
    std::optional<fiber::tls::TlsPrivateKey> key;
    std::optional<fiber::tls::TlsTrustStore> trust;
    std::optional<fiber::tls::TlsTicketService> tickets;
    fiber::tls::TlsTicketMinter minter{};
    fiber::tls::TlsResumptionLookup lookup{};
    std::string_view alpn[2] = {"h2", "http/1.1"};

    ServerMaterial() {
        chain_pem.assign(certfix::kLeafRsaPem);
        chain_pem.append(certfix::kIntermediateRsaPem);
        auto parsed = fiber::tls::TlsCertificateChain::parse_pem_bundle({chain_pem.data(), chain_pem.size()});
        FIBER_ASSERT(parsed.has_value());
        chain = std::move(parsed).value();
        auto parsed_key =
                fiber::tls::TlsPrivateKey::parse_pem({certfix::kRsa2048KeyPem, std::strlen(certfix::kRsa2048KeyPem)});
        FIBER_ASSERT(parsed_key.has_value());
        key = std::move(parsed_key).value();
        auto store =
                fiber::tls::TlsTrustStore::from_pem_bundle({certfix::kRootRsaPem, std::strlen(certfix::kRootRsaPem)});
        FIBER_ASSERT(store.has_value());
        trust = std::move(store).value();

        fiber::tls::TlsTicketKeyMaterial material;
        material.id = 7;
        for (std::size_t i = 0; i < material.bytes.size(); ++i) {
            material.bytes[i] = static_cast<std::uint8_t>(0xA0 + i);
        }
        material.created_ms = kNowMs - 1000;
        tickets.emplace(std::span<const fiber::tls::TlsTicketKeyMaterial>(&material, 1),
                        fiber::tls::TlsTicketKeyPolicy{});
        FIBER_ASSERT(tickets->valid());
        minter = tickets->minter();
        lookup = tickets->lookup();
    }

    [[nodiscard]] fiber::tls::TlsServerConfig config() const {
        fiber::tls::TlsServerConfig cfg;
        cfg.chain = &*chain;
        cfg.key = &*key;
        cfg.alpn = alpn;
        cfg.now_unix_ms = kNowMs;
        cfg.enable_early_data = true;
        return cfg;
    }

    // mTLS: verify client chains against the test root and require one.
    void with_client_auth(fiber::tls::TlsServerConfig &cfg) const {
        cfg.client_trust = &*trust;
        cfg.require_client_cert = true;
    }
};

// Client: verifies against the test root; SNI "example.com" (the leaf SAN).
struct ClientMaterial {
    std::optional<fiber::tls::TlsTrustStore> trust;
    std::string_view alpn[2] = {"h2", "http/1.1"};

    ClientMaterial() {
        auto store =
                fiber::tls::TlsTrustStore::from_pem_bundle({certfix::kRootRsaPem, std::strlen(certfix::kRootRsaPem)});
        FIBER_ASSERT(store.has_value());
        trust = std::move(store).value();
    }

    [[nodiscard]] fiber::tls::TlsClientConfig config() const {
        fiber::tls::TlsClientConfig cfg;
        cfg.sni_host = "example.com";
        cfg.alpn = alpn;
        cfg.trust = &*trust;
        cfg.now_unix_ms = kNowMs;
        return cfg;
    }
};

} // namespace fiber::fuzz

#endif // FIBER_FUZZ_TLS_FUZZ_COMMON_H
