#include <fiber/tls/TlsVersion.h>

namespace fiber::tls {

bool tls_version_list_contains(std::span<const std::uint8_t> versions, std::uint16_t version) noexcept {
    for (std::size_t i = 0; i + 1 < versions.size(); i += 2) {
        const std::uint16_t candidate = (static_cast<std::uint16_t>(versions[i]) << 8U) | versions[i + 1];
        if (candidate == version) {
            return true;
        }
    }
    return false;
}

} // namespace fiber::tls
