#include "TlsSuitePreference.h"

#include <algorithm>

#include <openssl/aead.h>

namespace fiber::tls {

bool tls_has_aes_hardware() noexcept { return EVP_has_aes_hardware() != 0; }

const std::array<std::uint16_t, 9> &tls_effective_suite_order() noexcept {
    static const std::array<std::uint16_t, 9> kEffective =
            tls_has_aes_hardware() ? kTlsSuitePreference : tls_suites_chacha_first(kTlsSuitePreference);
    return kEffective;
}

std::span<const std::uint16_t> tls_client_offer_suites(bool offer_tls12) noexcept {
    static const auto kOffer = [] {
        std::array<std::uint16_t, kTlsSuitePreference.size() + kTlsClientLegacySuites.size()> out{};
        const auto &effective = tls_effective_suite_order();
        const auto tail = std::copy(effective.begin(), effective.end(), out.begin());
        std::copy(kTlsClientLegacySuites.begin(), kTlsClientLegacySuites.end(), tail);
        return out;
    }();
    return {kOffer.data(), offer_tls12 ? kOffer.size() : kTlsSuitePreference.size()};
}

} // namespace fiber::tls
