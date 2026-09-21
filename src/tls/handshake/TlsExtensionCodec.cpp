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

} // namespace fiber::tls
