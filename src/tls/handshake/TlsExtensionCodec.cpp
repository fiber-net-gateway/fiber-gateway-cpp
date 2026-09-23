#include <fiber/tls/handshake/TlsExtensionCodec.h>

namespace fiber::tls {

common::IoResult<bool> TlsExtensionCursor::next(TlsExtensionView &out) noexcept {
    if (cursor_.empty()) {
        return false;
    }
    const auto type = cursor_.read_be16();
    if (!type.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const auto length = cursor_.read_be16();
    if (!length.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    auto data = cursor_.read_slice(length.value());
    if (!data.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    out.type = type.value();
    out.data = data.value();
    return true;
}

common::IoResult<bool> TlsAlpnCursor::next(std::string_view &out) noexcept {
    if (cursor_.empty()) {
        return false;
    }
    const auto length = cursor_.read_u8();
    if (!length.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    auto name = cursor_.read_slice(length.value());
    if (!name.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    out = std::string_view{reinterpret_cast<const char *>(name.value().data()), name.value().size()};
    return true;
}

common::IoResult<bool> tls_find_client_key_share(std::span<const std::uint8_t> client_shares, TlsNamedGroup group,
                                                 TlsKeyShareView &out) noexcept {
    TlsReadCursor cursor(client_shares.data(), client_shares.size());
    while (!cursor.empty()) {
        const auto entry_group = cursor.read_be16();
        if (!entry_group.has_value()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        const auto length = cursor.read_be16();
        if (!length.has_value()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        auto key_exchange = cursor.read_slice(length.value());
        if (!key_exchange.has_value()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        if (entry_group.value() == static_cast<std::uint16_t>(group)) {
            out.group = group;
            out.key_exchange = key_exchange.value();
            return true;
        }
    }
    return false;
}

bool tls_psk_modes_contains(std::span<const std::uint8_t> modes, std::uint8_t mode) noexcept {
    for (const std::uint8_t candidate: modes) {
        if (candidate == mode) {
            return true;
        }
    }
    return false;
}

common::IoResult<bool> tls_psk_identity_at(std::span<const std::uint8_t> psk_identities, std::size_t index,
                                           TlsPskIdentityView &out) noexcept {
    TlsReadCursor cursor(psk_identities.data(), psk_identities.size());
    for (std::size_t i = 0;; ++i) {
        if (cursor.empty()) {
            return false;
        }
        const auto identity_len = cursor.read_be16();
        if (!identity_len.has_value()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        auto identity = cursor.read_slice(identity_len.value());
        if (!identity.has_value()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        const auto age = cursor.read_be32();
        if (!age.has_value()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        if (i == index) {
            out.identity = identity.value();
            out.obfuscated_ticket_age = age.value();
            return true;
        }
    }
}

common::IoResult<bool> tls_psk_binder_at(std::span<const std::uint8_t> psk_binders, std::size_t index,
                                         std::span<const std::uint8_t> &out) noexcept {
    TlsReadCursor cursor(psk_binders.data(), psk_binders.size());
    for (std::size_t i = 0;; ++i) {
        if (cursor.empty()) {
            return false;
        }
        const auto binder_len = cursor.read_u8();
        if (!binder_len.has_value()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        auto binder = cursor.read_slice(binder_len.value());
        if (!binder.has_value()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        if (i == index) {
            out = binder.value();
            return true;
        }
    }
}

} // namespace fiber::tls
