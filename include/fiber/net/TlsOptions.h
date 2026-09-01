#ifndef FIBER_NET_TLS_OPTIONS_H
#define FIBER_NET_TLS_OPTIONS_H

#include <cstdint>
#include <string>
#include <utility>

namespace fiber::net {

enum class TlsPemSourceKind : std::uint8_t {
    None,
    File,
    Content,
};

struct TlsPemSource {
    TlsPemSourceKind kind = TlsPemSourceKind::None;
    std::string value{};

    [[nodiscard]] static TlsPemSource from_file(std::string path) {
        return {.kind = TlsPemSourceKind::File, .value = std::move(path)};
    }

    [[nodiscard]] static TlsPemSource from_content(std::string pem) {
        return {.kind = TlsPemSourceKind::Content, .value = std::move(pem)};
    }

    [[nodiscard]] bool empty() const noexcept { return kind == TlsPemSourceKind::None; }
};

enum class TlsTrustStoreKind : std::uint8_t {
    None,
    System,
    File,
    Content,
};

struct TlsTrustStoreSource {
    TlsTrustStoreKind kind = TlsTrustStoreKind::None;
    std::string value{};

    [[nodiscard]] static TlsTrustStoreSource system() noexcept { return {.kind = TlsTrustStoreKind::System}; }

    [[nodiscard]] static TlsTrustStoreSource from_file(std::string path) {
        return {.kind = TlsTrustStoreKind::File, .value = std::move(path)};
    }

    [[nodiscard]] static TlsTrustStoreSource from_content(std::string pem) {
        return {.kind = TlsTrustStoreKind::Content, .value = std::move(pem)};
    }

    [[nodiscard]] bool empty() const noexcept { return kind == TlsTrustStoreKind::None; }
};

// Configuration-time certificate material. TlsContext::create() consumes this
// description synchronously and does not retain paths or PEM content.
struct TlsOptions {
    // PEM leaf certificate followed by optional intermediate certificates.
    TlsPemSource certificate_chain{};
    // PEM private key matching the leaf certificate.
    TlsPemSource private_key{};
    // Root certificates used to authenticate a peer. Whether authentication is
    // enabled is a per-connection policy, not a property of this material.
    TlsTrustStoreSource trust_store{};
};

} // namespace fiber::net

#endif // FIBER_NET_TLS_OPTIONS_H
