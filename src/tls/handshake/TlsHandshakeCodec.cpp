#include <fiber/tls/handshake/TlsHandshakeCodec.h>

#include <array>

#include <fiber/tls/handshake/TlsExtensionCodec.h>

namespace fiber::tls {

common::IoResult<TlsHandshakeHeader> tls_decode_handshake_header(const std::uint8_t *src, std::size_t len) noexcept {
    if (len < kTlsHandshakeHeaderSize) {
        return std::unexpected(common::IoErr::Invalid);
    }
    TlsHandshakeHeader header{};
    header.type = static_cast<TlsHandshakeType>(src[0]);
    header.length = (static_cast<std::uint32_t>(src[1]) << 16U) | (static_cast<std::uint32_t>(src[2]) << 8U) |
                    static_cast<std::uint32_t>(src[3]);
    return header;
}

namespace {

inline constexpr std::size_t kMaxClientHelloExtensions = 64;

[[nodiscard]] bool extension_seen(const std::array<std::uint16_t, kMaxClientHelloExtensions> &seen, std::size_t count,
                                  std::uint16_t type) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
        if (seen[i] == type) {
            return true;
        }
    }
    return false;
}

// RFC 6066 §3 ServerNameList: { type(1), len(2), name } entries, exact walk.
[[nodiscard]] common::IoResult<void> parse_server_name(std::span<const std::uint8_t> data,
                                                       TlsClientHello &hello) noexcept {
    TlsReadCursor cursor(data.data(), data.size());
    const auto list_len = cursor.read_be16();
    if (!list_len.has_value() || list_len.value() != data.size() - 2) {
        return std::unexpected(common::IoErr::Invalid);
    }
    while (!cursor.empty()) {
        const auto name_type = cursor.read_u8();
        if (!name_type.has_value()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        const auto name_len = cursor.read_be16();
        if (!name_len.has_value()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        auto name = cursor.read_slice(name_len.value());
        if (!name.has_value()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        if (name_type.value() == 0 && !hello.has_server_name) {
            hello.has_server_name = true;
            hello.server_name = {reinterpret_cast<const char *>(name.value().data()), name.value().size()};
        }
    }
    return {};
}

// u16-counted raw u16 list (supported_groups / signature_algorithms[_cert]).
[[nodiscard]] common::IoResult<std::span<const std::uint8_t>>
parse_u16_list(std::span<const std::uint8_t> data) noexcept {
    TlsReadCursor cursor(data.data(), data.size());
    const auto list_len = cursor.read_be16();
    if (!list_len.has_value() || list_len.value() < 2 || (list_len.value() & 1U) != 0 ||
        list_len.value() != data.size() - 2) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return std::span<const std::uint8_t>{data.data() + 2, list_len.value()};
}

// RFC 8446 §4.2.8 client_shares body after the 2-byte vector length.
[[nodiscard]] common::IoResult<std::span<const std::uint8_t>>
parse_key_share_entries(std::span<const std::uint8_t> data) noexcept {
    TlsReadCursor cursor(data.data(), data.size());
    const auto list_len = cursor.read_be16();
    if (!list_len.has_value() || list_len.value() != data.size() - 2) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const std::span<const std::uint8_t> entries{data.data() + 2, list_len.value()};
    // Structural walk: every entry must have a non-empty key_exchange.
    TlsReadCursor walk(entries.data(), entries.size());
    while (!walk.empty()) {
        if (!walk.read_be16().has_value()) { // group
            return std::unexpected(common::IoErr::Invalid);
        }
        const auto key_len = walk.read_be16();
        if (!key_len.has_value() || key_len.value() < 1 || !walk.skip(key_len.value()).has_value()) {
            return std::unexpected(common::IoErr::Invalid);
        }
    }
    return entries;
}

[[nodiscard]] common::IoResult<std::span<const std::uint8_t>>
parse_alpn_list(std::span<const std::uint8_t> data) noexcept {
    TlsReadCursor cursor(data.data(), data.size());
    const auto list_len = cursor.read_be16();
    if (!list_len.has_value() || list_len.value() != data.size() - 2) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const std::span<const std::uint8_t> list{data.data() + 2, list_len.value()};
    std::string_view name;
    TlsAlpnCursor walk(list);
    while (true) {
        const auto has = walk.next(name);
        if (!has.has_value()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        if (!has.value()) {
            break;
        }
    }
    return list;
}

// RFC 8446 §4.2.11 pre_shared_key: identities and binders. `body_start` anchors
// psk_binder_block_offset (the transcript truncation point).
[[nodiscard]] common::IoResult<void> parse_pre_shared_key(std::span<const std::uint8_t> data,
                                                          const std::uint8_t *body_start,
                                                          TlsClientHello &hello) noexcept {
    TlsReadCursor cursor(data.data(), data.size());
    const auto identities_len = cursor.read_be16();
    if (!identities_len.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    auto identities = cursor.read_slice(identities_len.value());
    if (!identities.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }

    // Where the binders vector's length prefix starts, relative to body start.
    hello.psk_binder_block_offset = static_cast<std::size_t>((data.data() + 2 + identities_len.value()) - body_start);

    const auto binders_len = cursor.read_be16();
    if (!binders_len.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    auto binders = cursor.read_slice(binders_len.value());
    if (!binders.has_value() || !cursor.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::uint16_t identity_count = 0;
    TlsReadCursor identity_walk(identities.value().data(), identities.value().size());
    while (!identity_walk.empty()) {
        const auto identity_len = identity_walk.read_be16();
        if (!identity_len.has_value() || !identity_walk.skip(identity_len.value()).has_value() ||
            !identity_walk.read_be32().has_value()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        ++identity_count;
    }

    std::uint16_t binder_count = 0;
    TlsReadCursor binder_walk(binders.value().data(), binders.value().size());
    while (!binder_walk.empty()) {
        const auto binder_len = binder_walk.read_u8();
        if (!binder_len.has_value() || !binder_walk.skip(binder_len.value()).has_value()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        ++binder_count;
    }

    if (identity_count != binder_count) {
        return std::unexpected(common::IoErr::Invalid);
    }

    hello.psk_identities = identities.value();
    hello.psk_binders = binders.value();
    hello.psk_identity_count = identity_count;
    hello.psk_binder_count = binder_count;
    return {};
}

[[nodiscard]] common::IoResult<void> parse_client_hello_extensions(std::span<const std::uint8_t> block,
                                                                   const std::uint8_t *body_start,
                                                                   TlsClientHello &hello) noexcept {
    std::array<std::uint16_t, kMaxClientHelloExtensions> seen{};
    std::size_t seen_count = 0;
    std::uint16_t last_type = 0;

    TlsExtensionCursor cursor(block);
    TlsExtensionView view;
    while (true) {
        const auto has = cursor.next(view);
        if (!has.has_value()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        if (!has.value()) {
            break;
        }
        if (extension_seen(seen, seen_count, view.type) || seen_count == seen.size()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        seen[seen_count++] = view.type;
        last_type = view.type;

        switch (static_cast<TlsExtensionType>(view.type)) {
            case TlsExtensionType::ServerName:
                if (!parse_server_name(view.data, hello).has_value()) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                break;
            case TlsExtensionType::SupportedVersions: {
                if (view.data.size() < 1) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                const std::uint8_t list_len = view.data[0];
                const std::size_t rest = view.data.size() - 1;
                if (list_len < 2 || (list_len & 1U) != 0 || list_len != rest) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                hello.has_supported_versions = true;
                hello.supported_versions = std::span<const std::uint8_t>{view.data.data() + 1, list_len};
                break;
            }
            case TlsExtensionType::SupportedGroups: {
                const auto groups = parse_u16_list(view.data);
                if (!groups.has_value()) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                hello.has_supported_groups = true;
                hello.supported_groups = groups.value();
                break;
            }
            case TlsExtensionType::SignatureAlgorithms: {
                const auto schemes = parse_u16_list(view.data);
                if (!schemes.has_value()) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                hello.has_signature_algorithms = true;
                hello.signature_algorithms = schemes.value();
                break;
            }
            case TlsExtensionType::SignatureAlgorithmsCert: {
                const auto schemes = parse_u16_list(view.data);
                if (!schemes.has_value()) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                hello.has_signature_algorithms_cert = true;
                hello.signature_algorithms_cert = schemes.value();
                break;
            }
            case TlsExtensionType::KeyShare: {
                const auto entries = parse_key_share_entries(view.data);
                if (!entries.has_value()) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                hello.has_key_share = true;
                hello.key_share_entries = entries.value();
                break;
            }
            case TlsExtensionType::Alpn: {
                const auto list = parse_alpn_list(view.data);
                if (!list.has_value()) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                hello.has_alpn = true;
                hello.alpn_list = list.value();
                break;
            }
            case TlsExtensionType::PskKeyExchangeModes: {
                if (view.data.empty() || view.data[0] != view.data.size() - 1 || view.data.size() < 2) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                hello.has_psk_key_exchange_modes = true;
                hello.psk_key_exchange_modes =
                        std::span<const std::uint8_t>{view.data.data() + 1, view.data.size() - 1};
                break;
            }
            case TlsExtensionType::EarlyData:
                if (!view.data.empty()) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                hello.has_early_data = true;
                break;
            case TlsExtensionType::SessionTicket:
                hello.has_session_ticket = true;
                hello.session_ticket = view.data;
                break;
            case TlsExtensionType::RecordSizeLimit:
                if (view.data.size() != 2) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                hello.has_record_size_limit = true;
                hello.record_size_limit =
                        static_cast<std::uint16_t>((static_cast<std::uint16_t>(view.data[0]) << 8U) | view.data[1]);
                break;
            case TlsExtensionType::EncryptThenMac:
                if (!view.data.empty()) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                hello.has_encrypt_then_mac = true;
                break;
            case TlsExtensionType::ExtendedMasterSecret:
                if (!view.data.empty()) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                hello.has_extended_master_secret = true;
                break;
            case TlsExtensionType::RenegotiationInfo:
                hello.has_renegotiation_info = true;
                break;
            case TlsExtensionType::PreSharedKey:
                if (!parse_pre_shared_key(view.data, body_start, hello).has_value()) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                hello.has_pre_shared_key = true;
                break;
            default:
                // Unknown or not-extracted extension: structurally covered by the
                // block walk, never negotiated.
                break;
        }
    }

    if (hello.has_pre_shared_key && last_type != static_cast<std::uint16_t>(TlsExtensionType::PreSharedKey)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return {};
}

} // namespace

common::IoResult<void> tls_decode_client_hello(const std::uint8_t *body, std::size_t len,
                                               TlsClientHello &out) noexcept {
    TlsReadCursor cursor(body, len);
    TlsClientHello hello{};

    const auto legacy_version = cursor.read_be16();
    if (!legacy_version.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    hello.legacy_version = legacy_version.value();

    const auto random = cursor.read_slice(32);
    if (!random.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    hello.random = random.value();

    const auto session_id_len = cursor.read_u8();
    if (!session_id_len.has_value() || session_id_len.value() > 32) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const auto session_id = cursor.read_slice(session_id_len.value());
    if (!session_id.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    hello.session_id = session_id.value();

    const auto cipher_suites_len = cursor.read_be16();
    if (!cipher_suites_len.has_value() || cipher_suites_len.value() < 2 || (cipher_suites_len.value() & 1U) != 0) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const auto cipher_suites = cursor.read_slice(cipher_suites_len.value());
    if (!cipher_suites.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    hello.cipher_suites = cipher_suites.value();

    const auto compression_len = cursor.read_u8();
    if (!compression_len.has_value() || compression_len.value() < 1) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const auto compression = cursor.read_slice(compression_len.value());
    if (!compression.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    hello.compression_methods = compression.value();

    // Extensions are optional in TLS 1.2; present block must consume the body.
    if (!cursor.empty()) {
        const auto block_len = cursor.read_be16();
        if (!block_len.has_value()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        const auto block = cursor.read_slice(block_len.value());
        if (!block.has_value() || !cursor.empty()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        hello.extensions_block = block.value();
        if (!parse_client_hello_extensions(block.value(), body, hello).has_value()) {
            return std::unexpected(common::IoErr::Invalid);
        }
    }

    out = hello;
    return {};
}

} // namespace fiber::tls
