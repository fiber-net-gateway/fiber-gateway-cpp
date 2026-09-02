#ifndef FIBER_NET_TLS_PEM_SOURCE_H
#define FIBER_NET_TLS_PEM_SOURCE_H

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

} // namespace fiber::net

#endif // FIBER_NET_TLS_PEM_SOURCE_H
