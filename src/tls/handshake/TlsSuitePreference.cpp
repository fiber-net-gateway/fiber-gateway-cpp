#include "TlsSuitePreference.h"

#include <openssl/aead.h>

namespace fiber::tls {

bool tls_has_aes_hardware() noexcept { return EVP_has_aes_hardware() != 0; }

const std::array<std::uint16_t, 9> &tls_effective_suite_order() noexcept {
    static const std::array<std::uint16_t, 9> kEffective =
            tls_has_aes_hardware() ? kTlsSuitePreference : tls_suites_chacha_first(kTlsSuitePreference);
    return kEffective;
}

} // namespace fiber::tls
