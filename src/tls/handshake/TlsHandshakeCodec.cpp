#include <fiber/tls/handshake/TlsHandshakeCodec.h>

#include <array>
#include <cstring>

#include <fiber/tls/TlsVersion.h>
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

// ====================================================================
// 06 codec补齐：server flight decode + client flight encode
// ====================================================================

namespace {

// ---- encode side: bounds-checked big-endian writer over caller scratch ----

class TlsWriteCursor {
public:
    explicit TlsWriteCursor(std::span<std::uint8_t> dst) noexcept :
        begin_(dst.data()), pos_(dst.data()), end_(dst.data() + dst.size()) {}

    [[nodiscard]] std::size_t offset() const noexcept { return static_cast<std::size_t>(pos_ - begin_); }
    [[nodiscard]] std::size_t remaining() const noexcept { return static_cast<std::size_t>(end_ - pos_); }

    [[nodiscard]] bool u8(std::uint8_t value) noexcept {
        if (remaining() < 1) {
            return false;
        }
        *pos_++ = value;
        return true;
    }

    [[nodiscard]] bool be16(std::uint16_t value) noexcept {
        if (remaining() < 2) {
            return false;
        }
        *pos_++ = static_cast<std::uint8_t>(value >> 8U);
        *pos_++ = static_cast<std::uint8_t>(value);
        return true;
    }

    [[nodiscard]] bool be24(std::uint32_t value) noexcept {
        if (remaining() < 3) {
            return false;
        }
        *pos_++ = static_cast<std::uint8_t>(value >> 16U);
        *pos_++ = static_cast<std::uint8_t>(value >> 8U);
        *pos_++ = static_cast<std::uint8_t>(value);
        return true;
    }

    [[nodiscard]] bool be32(std::uint32_t value) noexcept {
        if (remaining() < 4) {
            return false;
        }
        *pos_++ = static_cast<std::uint8_t>(value >> 24U);
        *pos_++ = static_cast<std::uint8_t>(value >> 16U);
        *pos_++ = static_cast<std::uint8_t>(value >> 8U);
        *pos_++ = static_cast<std::uint8_t>(value);
        return true;
    }

    [[nodiscard]] bool bytes(std::span<const std::uint8_t> src) noexcept {
        if (remaining() < src.size()) {
            return false;
        }
        if (!src.empty()) {
            std::memcpy(pos_, src.data(), src.size());
            pos_ += src.size();
        }
        return true;
    }

    [[nodiscard]] bool zero(std::size_t len) noexcept {
        if (remaining() < len) {
            return false;
        }
        if (len > 0) {
            std::memset(pos_, 0, len);
            pos_ += len;
        }
        return true;
    }

    // Extension head: type + payload length.
    [[nodiscard]] bool ext(TlsExtensionType type, std::size_t payload_len) noexcept {
        return be16(static_cast<std::uint16_t>(type)) && be16(static_cast<std::uint16_t>(payload_len));
    }

private:
    std::uint8_t *begin_ = nullptr;
    std::uint8_t *pos_ = nullptr;
    std::uint8_t *end_ = nullptr;
};

template<std::size_t kCap>
[[nodiscard]] bool ext_seen(const std::array<std::uint16_t, kCap> &seen, std::size_t count,
                            std::uint16_t type) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
        if (seen[i] == type) {
            return true;
        }
    }
    return false;
}

// SH/EE ALPN payload: ProtocolNameList with EXACTLY one non-empty name (the
// server's selection; RFC 7301 §3.1 — servers must not send >1).
[[nodiscard]] common::IoResult<std::string_view> parse_alpn_single(std::span<const std::uint8_t> data) noexcept {
    TlsReadCursor cursor(data.data(), data.size());
    const auto list_len = cursor.read_be16();
    if (!list_len.has_value() || list_len.value() != data.size() - 2) {
        return std::unexpected(common::IoErr::Invalid);
    }
    std::string_view picked{};
    std::size_t count = 0;
    TlsAlpnCursor walk({data.data() + 2, list_len.value()});
    while (true) {
        std::string_view name;
        const auto has = walk.next(name);
        if (!has.has_value()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        if (!has.value()) {
            break;
        }
        picked = name;
        ++count;
    }
    if (count != 1 || picked.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return picked;
}

// SH/HRR key_share payload: ServerHello carries one bare KeyShareEntry —
// group then length-prefixed key_exchange; HelloRetryRequest carries only
// the 2-byte selected_group. The length-prefixed list wrapper is
// ClientHello-only (RFC 8446 §4.2.8 server_share / selected_group).
[[nodiscard]] common::IoResult<void> parse_server_key_share(std::span<const std::uint8_t> data,
                                                            TlsServerHello &out) noexcept {
    TlsReadCursor cursor(data.data(), data.size());
    const auto group = cursor.read_be16();
    if (!group.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    out.has_key_share = true;
    out.key_share_group = group.value();
    if (cursor.empty()) {
        out.key_share = {}; // the HRR selected_group form has no key portion
        return {};
    }
    const auto key_len = cursor.read_be16();
    if (!key_len.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const auto key = cursor.read_slice(key_len.value());
    if (!key.has_value() || !cursor.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    out.key_share = key.value();
    return {};
}

inline constexpr std::size_t kMaxServerHelloExtensions = 16;
inline constexpr std::size_t kMaxEncryptedExtensionsEntries = 32;

[[nodiscard]] common::IoResult<void> parse_server_hello_extensions(std::span<const std::uint8_t> block,
                                                                   TlsServerHello &hello) noexcept {
    std::array<std::uint16_t, kMaxServerHelloExtensions> seen{};
    std::size_t seen_count = 0;

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
        if (ext_seen(seen, seen_count, view.type) || seen_count == seen.size()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        seen[seen_count++] = view.type;

        switch (static_cast<TlsExtensionType>(view.type)) {
            case TlsExtensionType::SupportedVersions:
                if (view.data.size() != 2) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                hello.has_supported_version = true;
                hello.supported_version =
                        static_cast<std::uint16_t>((static_cast<std::uint16_t>(view.data[0]) << 8U) | view.data[1]);
                break;
            case TlsExtensionType::KeyShare:
                if (!parse_server_key_share(view.data, hello).has_value()) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                break;
            case TlsExtensionType::Cookie: {
                // RFC 8446 §4.2.2: opaque cookie<1..2^16-1> — 2-byte prefix, non-empty.
                TlsReadCursor cookie(view.data.data(), view.data.size());
                const auto len = cookie.read_be16();
                if (!len.has_value() || len.value() < 1 || len.value() != view.data.size() - 2) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                hello.has_cookie = true;
                hello.cookie = std::span<const std::uint8_t>{view.data.data() + 2, len.value()};
                break;
            }
            case TlsExtensionType::PreSharedKey:
                if (view.data.size() != 2) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                hello.has_selected_identity = true;
                hello.selected_identity =
                        static_cast<std::uint16_t>((static_cast<std::uint16_t>(view.data[0]) << 8U) | view.data[1]);
                break;
            case TlsExtensionType::Alpn: {
                const auto picked = parse_alpn_single(view.data);
                if (!picked.has_value()) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                hello.has_alpn = true;
                hello.alpn = picked.value();
                break;
            }
            case TlsExtensionType::ExtendedMasterSecret:
                if (!view.data.empty()) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                hello.has_extended_master_secret = true;
                break;
            case TlsExtensionType::RenegotiationInfo:
                hello.has_renegotiation_info = true;
                hello.renegotiation_info = view.data;
                break;
            default:
                // Unknown or not-extracted extension: structurally covered by
                // the block walk. Whether it is legal HERE (SH-exclusive in EE,
                // post-1.2 extension in a 1.3 SH) is the engine's check.
                break;
        }
    }
    return {};
}

// Certificate list walk shared by both Certificate forms: 3-byte-length
// entries from a 3-byte-length list, exact consumption, chain-count cap.
// `with_entry_extensions` covers the 1.3 per-entry extension vector.
template<typename Certs>
[[nodiscard]] common::IoResult<void> parse_certificate_list(std::span<const std::uint8_t> list,
                                                            bool with_entry_extensions, Certs &out) noexcept {
    TlsReadCursor cursor(list.data(), list.size());
    while (!cursor.empty()) {
        if (out.cert_count == Certs::kMaxEntries) {
            return std::unexpected(common::IoErr::Invalid);
        }
        const auto cert_len = cursor.read_be24();
        if (!cert_len.has_value() || cert_len.value() < 1) {
            return std::unexpected(common::IoErr::Invalid);
        }
        const auto cert = cursor.read_slice(cert_len.value());
        if (!cert.has_value()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        if (with_entry_extensions) {
            const auto ext_len = cursor.read_be16();
            if (!ext_len.has_value() || !cursor.skip(ext_len.value()).has_value()) {
                return std::unexpected(common::IoErr::Invalid);
            }
        }
        out.certs[out.cert_count++] = cert.value();
    }
    return {};
}

// The CH encoder's fixed offers (06: engine registry constants, not config).
// 09 §4.2 narrows the supported_versions list to the config bounds; an empty
// offered_versions span keeps the engine-fixed default.
inline constexpr std::uint16_t kClientOfferedVersions[] = {0x0304, 0x0303};

inline constexpr std::span<const std::uint16_t> effective_offered_versions(const TlsClientHelloInput &in) noexcept {
    return in.offered_versions.empty() ? std::span<const std::uint16_t>{kClientOfferedVersions} : in.offered_versions;
}

} // namespace

common::IoResult<void> tls_decode_server_hello(const std::uint8_t *body, std::size_t len,
                                               TlsServerHello &out) noexcept {
    TlsReadCursor cursor(body, len);
    TlsServerHello hello{};

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

    const auto suite = cursor.read_be16();
    if (!suite.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    hello.cipher_suite = suite.value();

    const auto compression = cursor.read_u8();
    if (!compression.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    hello.compression_method = compression.value();

    // Extensions optional (1.2 SH); a present block must consume the body.
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
        if (!parse_server_hello_extensions(block.value(), hello).has_value()) {
            return std::unexpected(common::IoErr::Invalid);
        }
    }

    out = hello;
    return {};
}

common::IoResult<void> tls_decode_encrypted_extensions(const std::uint8_t *body, std::size_t len,
                                                       TlsEncryptedExtensions &out) noexcept {
    TlsReadCursor cursor(body, len);
    TlsEncryptedExtensions ee{};

    const auto block_len = cursor.read_be16();
    if (!block_len.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const auto block = cursor.read_slice(block_len.value());
    if (!block.has_value() || !cursor.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    ee.extensions_block = block.value();

    std::array<std::uint16_t, kMaxEncryptedExtensionsEntries> seen{};
    std::size_t seen_count = 0;
    TlsExtensionCursor walk(block.value());
    TlsExtensionView view;
    while (true) {
        const auto has = walk.next(view);
        if (!has.has_value()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        if (!has.value()) {
            break;
        }
        if (ext_seen(seen, seen_count, view.type) || seen_count == seen.size()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        seen[seen_count++] = view.type;

        switch (static_cast<TlsExtensionType>(view.type)) {
            case TlsExtensionType::ServerName:
                // RFC 8446 §4.2.1: the EE server_name ack carries no
                // ServerNameList — any payload (e.g. the RFC 6066 1.2 form)
                // is malformed.
                if (!view.data.empty()) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                break;
            case TlsExtensionType::Alpn: {
                const auto picked = parse_alpn_single(view.data);
                if (!picked.has_value()) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                ee.has_alpn = true;
                ee.alpn = picked.value();
                break;
            }
            case TlsExtensionType::EarlyData:
                if (!view.data.empty()) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                ee.has_early_data = true;
                break;
            default:
                break;
        }
    }

    out = ee;
    return {};
}

common::IoResult<void> tls_decode_certificate_13(const std::uint8_t *body, std::size_t len,
                                                 TlsCertificate13 &out) noexcept {
    TlsReadCursor cursor(body, len);
    TlsCertificate13 cert{};

    const auto ctx_len = cursor.read_u8();
    if (!ctx_len.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const auto ctx = cursor.read_slice(ctx_len.value());
    if (!ctx.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    cert.certificate_request_context = {reinterpret_cast<const char *>(ctx.value().data()), ctx.value().size()};

    const auto list_len = cursor.read_be24();
    if (!list_len.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const auto list = cursor.read_slice(list_len.value());
    if (!list.has_value() || !cursor.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (!parse_certificate_list(list.value(), true, cert).has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }

    out = cert;
    return {};
}

common::IoResult<void> tls_decode_certificate_12(const std::uint8_t *body, std::size_t len,
                                                 TlsCertificate12 &out) noexcept {
    TlsReadCursor cursor(body, len);
    TlsCertificate12 cert{};

    const auto list_len = cursor.read_be24();
    if (!list_len.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const auto list = cursor.read_slice(list_len.value());
    if (!list.has_value() || !cursor.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (!parse_certificate_list(list.value(), false, cert).has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }

    out = cert;
    return {};
}

common::IoResult<void> tls_decode_certificate_request_13(const std::uint8_t *body, std::size_t len,
                                                         TlsCertificateRequest13 &out) noexcept {
    TlsReadCursor cursor(body, len);
    TlsCertificateRequest13 request{};

    const auto ctx_len = cursor.read_u8();
    if (!ctx_len.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const auto ctx = cursor.read_slice(ctx_len.value());
    if (!ctx.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    request.certificate_request_context = {reinterpret_cast<const char *>(ctx.value().data()), ctx.value().size()};

    const auto block_len = cursor.read_be16();
    if (!block_len.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const auto block = cursor.read_slice(block_len.value());
    if (!block.has_value() || !cursor.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    request.extensions_block = block.value();

    std::array<std::uint16_t, kMaxEncryptedExtensionsEntries> seen{};
    std::size_t seen_count = 0;
    TlsExtensionCursor walk(block.value());
    TlsExtensionView view;
    while (true) {
        const auto has = walk.next(view);
        if (!has.has_value()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        if (!has.value()) {
            break;
        }
        if (ext_seen(seen, seen_count, view.type) || seen_count == seen.size()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        seen[seen_count++] = view.type;

        if (static_cast<TlsExtensionType>(view.type) == TlsExtensionType::SignatureAlgorithms) {
            const auto schemes = parse_u16_list(view.data);
            if (!schemes.has_value()) {
                return std::unexpected(common::IoErr::Invalid);
            }
            request.has_signature_algorithms = true;
            request.signature_algorithms = schemes.value();
        }
    }
    if (!request.has_signature_algorithms) {
        return std::unexpected(common::IoErr::Invalid);
    }

    out = request;
    return {};
}

common::IoResult<void> tls_decode_certificate_request_12(const std::uint8_t *body, std::size_t len,
                                                         TlsCertificateRequest12 &out) noexcept {
    TlsReadCursor cursor(body, len);
    TlsCertificateRequest12 request{};

    const auto types_len = cursor.read_u8();
    if (!types_len.has_value() || types_len.value() < 1) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const auto types = cursor.read_slice(types_len.value());
    if (!types.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    request.certificate_types = types.value();

    // Positional optional vectors (RFC 5246 §7.4.4 order: types, [sigalgs],
    // [authorities]) — the wire is not self-describing here; every
    // implementation parses positionally. Real 1.2 servers always send sigalgs.
    // Unlike the extension form, the 1.2 sigalgs VECTOR is a bare u16 list
    // with no inner length prefix.
    if (!cursor.empty()) {
        const auto sig_len = cursor.read_be16();
        if (!sig_len.has_value()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        const auto sig = cursor.read_slice(sig_len.value());
        if (!sig.has_value() || sig_len.value() < 2 || (sig_len.value() & 1U) != 0) {
            return std::unexpected(common::IoErr::Invalid);
        }
        request.has_signature_algorithms = true;
        request.signature_algorithms = sig.value();
    }
    if (!cursor.empty()) {
        // certificate_authorities: u16-length vector (RFC 5246 §7.4.4 — the
        // u24 form died with RFC 2246; 4346/5246 both say <0..2^16-1>).
        const auto auth_len = cursor.read_be16();
        if (!auth_len.has_value()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        const auto auth = cursor.read_slice(auth_len.value());
        if (!auth.has_value()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        request.certificate_authorities = auth.value();
    }
    if (!cursor.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }

    out = request;
    return {};
}

common::IoResult<void> tls_decode_certificate_verify(const std::uint8_t *body, std::size_t len,
                                                     TlsCertificateVerify &out) noexcept {
    TlsReadCursor cursor(body, len);
    TlsCertificateVerify verify{};

    const auto scheme = cursor.read_be16();
    if (!scheme.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    verify.algorithm = scheme.value();

    const auto sig_len = cursor.read_be16();
    if (!sig_len.has_value() || sig_len.value() < 1) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const auto sig = cursor.read_slice(sig_len.value());
    if (!sig.has_value() || !cursor.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    verify.signature = sig.value();

    out = verify;
    return {};
}

common::IoResult<void> tls_decode_finished(const std::uint8_t *body, std::size_t len, TlsFinished &out) noexcept {
    if (len < 1) {
        return std::unexpected(common::IoErr::Invalid);
    }
    out.verify_data = std::span<const std::uint8_t>{body, len};
    return {};
}

common::IoResult<void> tls_decode_server_key_exchange(const std::uint8_t *body, std::size_t len,
                                                      TlsServerKeyExchange &out) noexcept {
    TlsReadCursor cursor(body, len);
    TlsServerKeyExchange ske{};

    const auto curve_type = cursor.read_u8();
    if (!curve_type.has_value() || curve_type.value() != 3) { // named_curve only
        return std::unexpected(common::IoErr::Invalid);
    }
    ske.curve_type = curve_type.value();

    const auto group = cursor.read_be16();
    if (!group.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    ske.named_group = group.value();

    const auto point_len = cursor.read_u8();
    if (!point_len.has_value() || point_len.value() < 1) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const auto point = cursor.read_slice(point_len.value());
    if (!point.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    ske.public_key = point.value();

    const auto scheme = cursor.read_be16();
    if (!scheme.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    ske.algorithm = scheme.value();

    const auto sig_len = cursor.read_be16();
    if (!sig_len.has_value() || sig_len.value() < 1) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const auto sig = cursor.read_slice(sig_len.value());
    if (!sig.has_value() || !cursor.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    ske.signature = sig.value();

    out = ske;
    return {};
}

// ---- encode ----

common::IoResult<std::size_t> tls_client_hello_size(const TlsClientHelloInput &in) noexcept {
    // Contract checks (mirror the encoder's shape requirements).
    if (in.random.size() != 32 || in.session_id.size() > 32) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (in.cipher_suites.empty() || in.supported_groups.empty() || in.signature_algorithms.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (in.early_data && !in.has_psk) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (in.has_psk && in.psk_binder_len != 32 && in.psk_binder_len != 48) {
        return std::unexpected(common::IoErr::Invalid);
    }
    // Prefix-bound checks: every lengthened field must fit its prefix.
    if (2 + 2 * in.cipher_suites.size() > 0xFFFF || 2 + 2 * in.supported_groups.size() > 0xFFFF ||
        2 + 2 * in.signature_algorithms.size() > 0xFFFF) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (in.sni_host.size() > 0xFFFF - 7 || in.session_ticket.size() > 0xFFFF - 4 || in.key_share.size() > 0xFFFF - 6 ||
        (!in.cookie.empty() && in.cookie.size() > 0xFFFF - 2) || (in.has_psk && in.psk_identity.size() > 0xFFFF - 6)) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::size_t size = kTlsHandshakeHeaderSize + 2 + 32 + 1 + in.session_id.size() + 2 + 2 * in.cipher_suites.size() +
                       1 + 1 + 2; // header, fixed fields, compression [null], ext-block len
    if (!in.sni_host.empty()) {
        size += 4 + 2 + 1 + 2 + in.sni_host.size();
    }
    if (in.offer_extended_master_secret) {
        size += 4;
    }
    if (in.offer_renegotiation_info) {
        size += 4 + 1;
    }
    if (!in.session_ticket.empty()) {
        size += 4 + in.session_ticket.size();
    }
    if (!in.alpn.empty()) {
        std::size_t list = 0;
        for (const std::string_view name: in.alpn) {
            list += 1 + name.size();
        }
        if (2 + list > 0xFFFF) {
            return std::unexpected(common::IoErr::Invalid);
        }
        size += 4 + 2 + list;
    }
    if (!in.cookie.empty()) {
        size += 4 + 2 + in.cookie.size();
    }
    size += 4 + 2 + 2 * in.supported_groups.size();
    size += 4 + 2 + 2 * in.signature_algorithms.size();
    const std::span<const std::uint16_t> versions = effective_offered_versions(in);
    if (versions.empty() || versions.size() > 127) { // vector length is a 1-byte prefix
        return std::unexpected(common::IoErr::Invalid);
    }
    size += 4 + 1 + 2 * versions.size();
    // psk_key_exchange_modes rides every CH (engine-fixed constant): a peer
    // that never sees it marks the connection unresumable — BoringSSL's
    // server skips NewSessionTicket issuance entirely (!accept_psk_mode).
    size += 4 + 2;
    if (!in.key_share.empty()) {
        size += 4 + 2 + 2 + 2 + in.key_share.size();
    }
    if (in.early_data) {
        size += 4;
    }
    if (in.has_psk) {
        size += 4 + (2 + 2 + in.psk_identity.size() + 4) + (2 + 1 + in.psk_binder_len);
    }
    if (size - kTlsHandshakeHeaderSize > kTlsMaxHandshakeMessageSize) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return size;
}

common::IoResult<TlsClientHelloEncoded> tls_encode_client_hello(const TlsClientHelloInput &in,
                                                                std::span<std::uint8_t> scratch) noexcept {
    const auto size = tls_client_hello_size(in);
    if (!size.has_value()) {
        return std::unexpected(size.error());
    }
    if (scratch.size() < size.value()) {
        return std::unexpected(common::IoErr::Invalid);
    }

    TlsWriteCursor w(scratch);
    TlsClientHelloEncoded encoded{};

    // Handshake header + fixed ClientHello fields.
    if (!w.u8(static_cast<std::uint8_t>(TlsHandshakeType::ClientHello)) ||
        !w.be24(static_cast<std::uint32_t>(size.value() - kTlsHandshakeHeaderSize)) ||
        !w.be16(static_cast<std::uint16_t>(TlsProtocolVersion::Tls12)) || !w.bytes(in.random) ||
        !w.u8(static_cast<std::uint8_t>(in.session_id.size())) || !w.bytes(in.session_id)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (!w.be16(static_cast<std::uint16_t>(2 * in.cipher_suites.size()))) {
        return std::unexpected(common::IoErr::Invalid);
    }
    for (const std::uint16_t suite: in.cipher_suites) {
        if (!w.be16(suite)) {
            return std::unexpected(common::IoErr::Invalid);
        }
    }
    if (!w.u8(1) || !w.u8(0)) { // compression: exactly null
        return std::unexpected(common::IoErr::Invalid);
    }

    // Extension block: everything after this 2-byte length; the exact-size
    // precheck makes the prefix computable without backpatching.
    const std::size_t ext_block_len = size.value() - w.offset() - 2;
    if (!w.be16(static_cast<std::uint16_t>(ext_block_len))) {
        return std::unexpected(common::IoErr::Invalid);
    }

    // Deterministic order (pre_shared_key LAST — RFC 8446 §4.2.11).
    if (!in.sni_host.empty()) {
        const std::size_t name_len = in.sni_host.size();
        if (!w.ext(TlsExtensionType::ServerName, 2 + 1 + 2 + name_len) ||
            !w.be16(static_cast<std::uint16_t>(1 + 2 + name_len)) || !w.u8(0) ||
            !w.be16(static_cast<std::uint16_t>(name_len)) ||
            !w.bytes({reinterpret_cast<const std::uint8_t *>(in.sni_host.data()), name_len})) {
            return std::unexpected(common::IoErr::Invalid);
        }
    }
    if (in.offer_extended_master_secret && !w.ext(TlsExtensionType::ExtendedMasterSecret, 0)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (in.offer_renegotiation_info) {
        if (!w.ext(TlsExtensionType::RenegotiationInfo, 1) || !w.u8(0)) {
            return std::unexpected(common::IoErr::Invalid);
        }
    }
    if (!in.session_ticket.empty()) {
        if (!w.ext(TlsExtensionType::SessionTicket, in.session_ticket.size()) || !w.bytes(in.session_ticket)) {
            return std::unexpected(common::IoErr::Invalid);
        }
    }
    if (!in.alpn.empty()) {
        std::size_t list_len = 0;
        for (const std::string_view name: in.alpn) {
            list_len += 1 + name.size();
        }
        if (!w.ext(TlsExtensionType::Alpn, 2 + list_len) || !w.be16(static_cast<std::uint16_t>(list_len))) {
            return std::unexpected(common::IoErr::Invalid);
        }
        for (const std::string_view name: in.alpn) {
            if (!w.u8(static_cast<std::uint8_t>(name.size())) ||
                !w.bytes({reinterpret_cast<const std::uint8_t *>(name.data()), name.size()})) {
                return std::unexpected(common::IoErr::Invalid);
            }
        }
    }
    if (!in.cookie.empty()) {
        if (!w.ext(TlsExtensionType::Cookie, 2 + in.cookie.size()) ||
            !w.be16(static_cast<std::uint16_t>(in.cookie.size())) || !w.bytes(in.cookie)) {
            return std::unexpected(common::IoErr::Invalid);
        }
    }
    if (!w.ext(TlsExtensionType::SupportedGroups, 2 + 2 * in.supported_groups.size()) ||
        !w.be16(static_cast<std::uint16_t>(2 * in.supported_groups.size()))) {
        return std::unexpected(common::IoErr::Invalid);
    }
    for (const std::uint16_t group: in.supported_groups) {
        if (!w.be16(group)) {
            return std::unexpected(common::IoErr::Invalid);
        }
    }
    if (!w.ext(TlsExtensionType::SignatureAlgorithms, 2 + 2 * in.signature_algorithms.size()) ||
        !w.be16(static_cast<std::uint16_t>(2 * in.signature_algorithms.size()))) {
        return std::unexpected(common::IoErr::Invalid);
    }
    for (const std::uint16_t scheme: in.signature_algorithms) {
        if (!w.be16(scheme)) {
            return std::unexpected(common::IoErr::Invalid);
        }
    }
    const std::span<const std::uint16_t> versions = effective_offered_versions(in);
    if (!w.ext(TlsExtensionType::SupportedVersions, 1 + 2 * versions.size()) ||
        !w.u8(static_cast<std::uint8_t>(2 * versions.size()))) {
        return std::unexpected(common::IoErr::Invalid);
    }
    for (const std::uint16_t version: versions) {
        if (!w.be16(version)) {
            return std::unexpected(common::IoErr::Invalid);
        }
    }
    // Always offered — see the sizing comment above.
    if (!w.ext(TlsExtensionType::PskKeyExchangeModes, 2) || !w.u8(1) || !w.u8(kTlsPskModePskDheKe)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (!in.key_share.empty()) {
        if (!w.ext(TlsExtensionType::KeyShare, 2 + 2 + 2 + in.key_share.size()) ||
            !w.be16(static_cast<std::uint16_t>(2 + 2 + in.key_share.size())) || !w.be16(in.key_share_group) ||
            !w.be16(static_cast<std::uint16_t>(in.key_share.size())) || !w.bytes(in.key_share)) {
            return std::unexpected(common::IoErr::Invalid);
        }
    }
    if (in.early_data && !w.ext(TlsExtensionType::EarlyData, 0)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (in.has_psk) {
        if (!w.ext(TlsExtensionType::PreSharedKey,
                   (2 + 2 + in.psk_identity.size() + 4) + (2 + 1 + in.psk_binder_len)) ||
            !w.be16(static_cast<std::uint16_t>(2 + in.psk_identity.size() + 4)) ||
            !w.be16(static_cast<std::uint16_t>(in.psk_identity.size())) || !w.bytes(in.psk_identity) ||
            !w.be32(in.psk_obfuscated_ticket_age)) {
            return std::unexpected(common::IoErr::Invalid);
        }
        encoded.binder_block_offset = w.offset();
        if (!w.be16(static_cast<std::uint16_t>(1 + in.psk_binder_len)) ||
            !w.u8(static_cast<std::uint8_t>(in.psk_binder_len)) || !w.zero(in.psk_binder_len)) {
            return std::unexpected(common::IoErr::Invalid);
        }
    }

    encoded.len = w.offset();
    return encoded;
}

common::IoResult<std::size_t> tls_encode_handshake_message(TlsHandshakeType type, std::span<const std::uint8_t> body,
                                                           std::span<std::uint8_t> scratch) noexcept {
    if (body.size() > kTlsMaxHandshakeMessageSize || scratch.size() < kTlsHandshakeHeaderSize + body.size()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    TlsWriteCursor w(scratch);
    if (!w.u8(static_cast<std::uint8_t>(type)) || !w.be24(static_cast<std::uint32_t>(body.size())) || !w.bytes(body)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return w.offset();
}

common::IoResult<std::size_t> tls_encode_certificate_13(std::span<const std::uint8_t> request_context,
                                                        std::span<const std::span<const std::uint8_t>> certs,
                                                        std::span<std::uint8_t> scratch) noexcept {
    if (certs.size() > TlsCertificate13::kMaxEntries || request_context.size() > 0xFF) {
        return std::unexpected(common::IoErr::Invalid);
    }
    std::size_t list_len = 0;
    for (const auto cert: certs) {
        if (cert.size() < 1) {
            return std::unexpected(common::IoErr::Invalid);
        }
        list_len += 3 + cert.size() + 2;
    }
    const std::size_t body_len = 1 + request_context.size() + 3 + list_len;
    if (body_len > kTlsMaxHandshakeMessageSize || scratch.size() < kTlsHandshakeHeaderSize + body_len) {
        return std::unexpected(common::IoErr::Invalid);
    }

    TlsWriteCursor w(scratch);
    if (!w.u8(static_cast<std::uint8_t>(TlsHandshakeType::Certificate)) ||
        !w.be24(static_cast<std::uint32_t>(body_len)) || !w.u8(static_cast<std::uint8_t>(request_context.size())) ||
        !w.bytes(request_context) || !w.be24(static_cast<std::uint32_t>(list_len))) {
        return std::unexpected(common::IoErr::Invalid);
    }
    for (const auto cert: certs) {
        if (!w.be24(static_cast<std::uint32_t>(cert.size())) || !w.bytes(cert) || !w.be16(0)) {
            return std::unexpected(common::IoErr::Invalid);
        }
    }
    return w.offset();
}

common::IoResult<std::size_t> tls_encode_certificate_12(std::span<const std::span<const std::uint8_t>> certs,
                                                        std::span<std::uint8_t> scratch) noexcept {
    if (certs.size() > TlsCertificate12::kMaxEntries) {
        return std::unexpected(common::IoErr::Invalid);
    }
    std::size_t list_len = 0;
    for (const auto cert: certs) {
        if (cert.size() < 1) {
            return std::unexpected(common::IoErr::Invalid);
        }
        list_len += 3 + cert.size();
    }
    const std::size_t body_len = 3 + list_len;
    if (body_len > kTlsMaxHandshakeMessageSize || scratch.size() < kTlsHandshakeHeaderSize + body_len) {
        return std::unexpected(common::IoErr::Invalid);
    }

    TlsWriteCursor w(scratch);
    if (!w.u8(static_cast<std::uint8_t>(TlsHandshakeType::Certificate)) ||
        !w.be24(static_cast<std::uint32_t>(body_len)) || !w.be24(static_cast<std::uint32_t>(list_len))) {
        return std::unexpected(common::IoErr::Invalid);
    }
    for (const auto cert: certs) {
        if (!w.be24(static_cast<std::uint32_t>(cert.size())) || !w.bytes(cert)) {
            return std::unexpected(common::IoErr::Invalid);
        }
    }
    return w.offset();
}

common::IoResult<std::size_t> tls_encode_certificate_verify(std::uint16_t scheme,
                                                            std::span<const std::uint8_t> signature,
                                                            std::span<std::uint8_t> scratch) noexcept {
    if (signature.size() < 1 || signature.size() > 0xFFFF) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const std::size_t body_len = 2 + 2 + signature.size();
    if (scratch.size() < kTlsHandshakeHeaderSize + body_len) {
        return std::unexpected(common::IoErr::Invalid);
    }
    TlsWriteCursor w(scratch);
    if (!w.u8(static_cast<std::uint8_t>(TlsHandshakeType::CertificateVerify)) ||
        !w.be24(static_cast<std::uint32_t>(body_len)) || !w.be16(scheme) ||
        !w.be16(static_cast<std::uint16_t>(signature.size())) || !w.bytes(signature)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return w.offset();
}

common::IoResult<std::size_t> tls_encode_finished(std::span<const std::uint8_t> verify_data,
                                                  std::span<std::uint8_t> scratch) noexcept {
    if (verify_data.size() < 1 || verify_data.size() > kTlsMaxHandshakeMessageSize ||
        scratch.size() < kTlsHandshakeHeaderSize + verify_data.size()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    TlsWriteCursor w(scratch);
    if (!w.u8(static_cast<std::uint8_t>(TlsHandshakeType::Finished)) ||
        !w.be24(static_cast<std::uint32_t>(verify_data.size())) || !w.bytes(verify_data)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return w.offset();
}

common::IoResult<std::size_t> tls_encode_client_key_exchange(std::span<const std::uint8_t> point,
                                                             std::span<std::uint8_t> scratch) noexcept {
    if (point.size() < 1 || point.size() > 255 || scratch.size() < kTlsHandshakeHeaderSize + 1 + point.size()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    TlsWriteCursor w(scratch);
    if (!w.u8(static_cast<std::uint8_t>(TlsHandshakeType::ClientKeyExchange)) ||
        !w.be24(static_cast<std::uint32_t>(1 + point.size())) || !w.u8(static_cast<std::uint8_t>(point.size())) ||
        !w.bytes(point)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return w.offset();
}

common::IoResult<void> tls_decode_client_key_exchange(const std::uint8_t *body, std::size_t len,
                                                      TlsClientKeyExchange &out) noexcept {
    TlsReadCursor cursor(body, len);
    TlsClientKeyExchange cke{};

    const auto point_len = cursor.read_u8();
    if (!point_len.has_value() || point_len.value() < 1) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const auto point = cursor.read_slice(point_len.value());
    if (!point.has_value() || !cursor.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    cke.public_key = point.value();

    out = cke;
    return {};
}

// ====================================================================
// 07 codec补齐：server flight encode + client flight decode
// ====================================================================

common::IoResult<std::size_t> tls_encode_server_hello(const TlsServerHelloInput &in,
                                                      std::span<std::uint8_t> scratch) noexcept {
    // Contract checks (mirror the encoder's shape requirements).
    if (in.random.size() != 32 || in.session_id.size() > 32) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (!in.alpn.empty() && in.alpn.size() > 0xFF) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (in.key_share.size() > 0xFFFF) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::size_t ext_len = 0;
    if (in.tls13) {
        // key_share (server share, or the bare selected_group when empty — HRR)
        // -> supported_versions -> pre_shared_key LAST (RFC 8446 §4.2.11).
        ext_len += 4 + 2 + (in.key_share.empty() ? 0 : 2 + in.key_share.size());
        ext_len += 4 + 2;
        if (in.selected_identity) {
            ext_len += 4 + 2;
        }
    } else {
        if (in.extended_master_secret) {
            ext_len += 4;
        }
        if (in.renegotiation_info) {
            ext_len += 4 + 1;
        }
        if (!in.alpn.empty()) {
            ext_len += 4 + 2 + 1 + in.alpn.size();
        }
        if (in.session_ticket) {
            ext_len += 4; // RFC 5077 §3.2: the SH echo is empty-payload
        }
    }

    const std::size_t body_len = 2 + 32 + 1 + in.session_id.size() + 2 + 1 + 2 + ext_len;
    if (body_len > kTlsMaxHandshakeMessageSize || scratch.size() < kTlsHandshakeHeaderSize + body_len) {
        return std::unexpected(common::IoErr::Invalid);
    }

    TlsWriteCursor w(scratch);
    if (!w.u8(static_cast<std::uint8_t>(TlsHandshakeType::ServerHello)) ||
        !w.be24(static_cast<std::uint32_t>(body_len)) ||
        !w.be16(static_cast<std::uint16_t>(TlsProtocolVersion::Tls12)) || !w.bytes(in.random) ||
        !w.u8(static_cast<std::uint8_t>(in.session_id.size())) || !w.bytes(in.session_id) || !w.be16(in.cipher_suite) ||
        !w.u8(0) || !w.be16(static_cast<std::uint16_t>(ext_len))) {
        return std::unexpected(common::IoErr::Invalid);
    }

    if (in.tls13) {
        if (!w.ext(TlsExtensionType::KeyShare, 2 + (in.key_share.empty() ? 0 : 2 + in.key_share.size())) ||
            !w.be16(in.key_share_group)) {
            return std::unexpected(common::IoErr::Invalid);
        }
        if (!in.key_share.empty() &&
            (!w.be16(static_cast<std::uint16_t>(in.key_share.size())) || !w.bytes(in.key_share))) {
            return std::unexpected(common::IoErr::Invalid);
        }
        if (!w.ext(TlsExtensionType::SupportedVersions, 2) || !w.be16(0x0304)) {
            return std::unexpected(common::IoErr::Invalid);
        }
        if (in.selected_identity && (!w.ext(TlsExtensionType::PreSharedKey, 2) || !w.be16(in.identity))) {
            return std::unexpected(common::IoErr::Invalid);
        }
    } else {
        if (in.extended_master_secret && !w.ext(TlsExtensionType::ExtendedMasterSecret, 0)) {
            return std::unexpected(common::IoErr::Invalid);
        }
        if (in.renegotiation_info) {
            if (!w.ext(TlsExtensionType::RenegotiationInfo, 1) || !w.u8(0)) {
                return std::unexpected(common::IoErr::Invalid);
            }
        }
        if (!in.alpn.empty()) {
            if (!w.ext(TlsExtensionType::Alpn, 2 + 1 + in.alpn.size()) ||
                !w.be16(static_cast<std::uint16_t>(1 + in.alpn.size())) ||
                !w.u8(static_cast<std::uint8_t>(in.alpn.size())) ||
                !w.bytes({reinterpret_cast<const std::uint8_t *>(in.alpn.data()), in.alpn.size()})) {
                return std::unexpected(common::IoErr::Invalid);
            }
        }
        if (in.session_ticket && !w.ext(TlsExtensionType::SessionTicket, 0)) {
            return std::unexpected(common::IoErr::Invalid);
        }
    }
    return w.offset();
}

common::IoResult<std::size_t> tls_encode_encrypted_extensions(const TlsEncryptedExtensionsInput &in,
                                                              std::span<std::uint8_t> scratch) noexcept {
    if (!in.alpn.empty() && in.alpn.size() > 0xFF) {
        return std::unexpected(common::IoErr::Invalid);
    }
    std::size_t ext_len = 0;
    if (in.acknowledge_server_name) {
        ext_len += 4; // the ack is an EMPTY extension (RFC 8446 §4.2.1)
    }
    if (!in.alpn.empty()) {
        ext_len += 4 + 2 + 1 + in.alpn.size();
    }
    if (in.early_data) {
        ext_len += 4;
    }

    const std::size_t body_len = 2 + ext_len;
    if (scratch.size() < kTlsHandshakeHeaderSize + body_len) {
        return std::unexpected(common::IoErr::Invalid);
    }

    TlsWriteCursor w(scratch);
    if (!w.u8(static_cast<std::uint8_t>(TlsHandshakeType::EncryptedExtensions)) ||
        !w.be24(static_cast<std::uint32_t>(body_len)) || !w.be16(static_cast<std::uint16_t>(ext_len))) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (in.acknowledge_server_name && !w.ext(TlsExtensionType::ServerName, 0)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (!in.alpn.empty()) {
        if (!w.ext(TlsExtensionType::Alpn, 2 + 1 + in.alpn.size()) ||
            !w.be16(static_cast<std::uint16_t>(1 + in.alpn.size())) ||
            !w.u8(static_cast<std::uint8_t>(in.alpn.size())) ||
            !w.bytes({reinterpret_cast<const std::uint8_t *>(in.alpn.data()), in.alpn.size()})) {
            return std::unexpected(common::IoErr::Invalid);
        }
    }
    if (in.early_data && !w.ext(TlsExtensionType::EarlyData, 0)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return w.offset();
}

common::IoResult<std::size_t> tls_encode_certificate_request_13(std::span<const std::uint16_t> signature_algorithms,
                                                                std::span<std::uint8_t> scratch) noexcept {
    if (signature_algorithms.empty() || 2 + 2 * signature_algorithms.size() > 0xFFFF) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const std::size_t ext_len = 4 + 2 + 2 * signature_algorithms.size();
    const std::size_t body_len = 1 + 2 + ext_len;
    if (scratch.size() < kTlsHandshakeHeaderSize + body_len) {
        return std::unexpected(common::IoErr::Invalid);
    }

    TlsWriteCursor w(scratch);
    if (!w.u8(static_cast<std::uint8_t>(TlsHandshakeType::CertificateRequest)) ||
        !w.be24(static_cast<std::uint32_t>(body_len)) || !w.u8(0) || !w.be16(static_cast<std::uint16_t>(ext_len)) ||
        !w.ext(TlsExtensionType::SignatureAlgorithms, 2 + 2 * signature_algorithms.size()) ||
        !w.be16(static_cast<std::uint16_t>(2 * signature_algorithms.size()))) {
        return std::unexpected(common::IoErr::Invalid);
    }
    for (const std::uint16_t scheme: signature_algorithms) {
        if (!w.be16(scheme)) {
            return std::unexpected(common::IoErr::Invalid);
        }
    }
    return w.offset();
}

common::IoResult<std::size_t> tls_encode_certificate_request_12(std::span<const std::uint16_t> signature_algorithms,
                                                                std::span<std::uint8_t> scratch) noexcept {
    if (signature_algorithms.empty() || 2 + 2 * signature_algorithms.size() > 0xFFFF) {
        return std::unexpected(common::IoErr::Invalid);
    }
    // types{rsa_sign(1), ecdsa_sign(64)} + sigalgs vector + EMPTY authorities.
    const std::size_t body_len = 1 + 2 + 2 + 2 * signature_algorithms.size() + 2;
    if (scratch.size() < kTlsHandshakeHeaderSize + body_len) {
        return std::unexpected(common::IoErr::Invalid);
    }

    TlsWriteCursor w(scratch);
    if (!w.u8(static_cast<std::uint8_t>(TlsHandshakeType::CertificateRequest)) ||
        !w.be24(static_cast<std::uint32_t>(body_len)) || !w.u8(2) || !w.u8(1) || !w.u8(64) ||
        !w.be16(static_cast<std::uint16_t>(2 * signature_algorithms.size()))) {
        return std::unexpected(common::IoErr::Invalid);
    }
    for (const std::uint16_t scheme: signature_algorithms) {
        if (!w.be16(scheme)) {
            return std::unexpected(common::IoErr::Invalid);
        }
    }
    if (!w.be16(0)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return w.offset();
}

common::IoResult<std::size_t> tls_encode_server_key_exchange(const TlsServerKeyExchangeInput &in,
                                                             std::span<std::uint8_t> scratch) noexcept {
    if (in.public_key.size() < 1 || in.public_key.size() > 255 || in.signature.empty() ||
        in.signature.size() > 0xFFFF) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const std::size_t body_len = 1 + 2 + 1 + in.public_key.size() + 2 + 2 + in.signature.size();
    if (body_len > kTlsMaxHandshakeMessageSize || scratch.size() < kTlsHandshakeHeaderSize + body_len) {
        return std::unexpected(common::IoErr::Invalid);
    }

    TlsWriteCursor w(scratch);
    if (!w.u8(static_cast<std::uint8_t>(TlsHandshakeType::ServerKeyExchange)) ||
        !w.be24(static_cast<std::uint32_t>(body_len)) || !w.u8(3) || !w.be16(in.named_group) ||
        !w.u8(static_cast<std::uint8_t>(in.public_key.size())) || !w.bytes(in.public_key) || !w.be16(in.scheme) ||
        !w.be16(static_cast<std::uint16_t>(in.signature.size())) || !w.bytes(in.signature)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return w.offset();
}

common::IoResult<std::size_t> tls_encode_new_session_ticket_13(const TlsNewSessionTicket13Input &in,
                                                               std::span<std::uint8_t> scratch) noexcept {
    if (in.ticket.empty() || in.ticket.size() > 0xFFFF) {
        return std::unexpected(common::IoErr::Invalid);
    }
    std::size_t ext_len = 0;
    if (in.max_early_data != 0) {
        ext_len += 4 + 4;
    }
    const std::size_t body_len = 4 + 4 + 1 + 1 + 2 + in.ticket.size() + 2 + ext_len;
    if (scratch.size() < kTlsHandshakeHeaderSize + body_len) {
        return std::unexpected(common::IoErr::Invalid);
    }

    TlsWriteCursor w(scratch);
    if (!w.u8(static_cast<std::uint8_t>(TlsHandshakeType::NewSessionTicket)) ||
        !w.be24(static_cast<std::uint32_t>(body_len)) || !w.be32(in.lifetime_s) || !w.be32(in.ticket_age_add) ||
        !w.u8(1) || !w.u8(in.ticket_nonce) || !w.be16(static_cast<std::uint16_t>(in.ticket.size())) ||
        !w.bytes(in.ticket) || !w.be16(static_cast<std::uint16_t>(ext_len))) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (in.max_early_data != 0 && (!w.ext(TlsExtensionType::EarlyData, 4) || !w.be32(in.max_early_data))) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return w.offset();
}

common::IoResult<std::size_t> tls_encode_new_session_ticket_12(std::uint32_t lifetime_s,
                                                               std::span<const std::uint8_t> ticket,
                                                               std::span<std::uint8_t> scratch) noexcept {
    if (ticket.empty() || ticket.size() > 0xFFFF) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const std::size_t body_len = 4 + 2 + ticket.size();
    if (scratch.size() < kTlsHandshakeHeaderSize + body_len) {
        return std::unexpected(common::IoErr::Invalid);
    }

    TlsWriteCursor w(scratch);
    if (!w.u8(static_cast<std::uint8_t>(TlsHandshakeType::NewSessionTicket)) ||
        !w.be24(static_cast<std::uint32_t>(body_len)) || !w.be32(lifetime_s) ||
        !w.be16(static_cast<std::uint16_t>(ticket.size())) || !w.bytes(ticket)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return w.offset();
}

} // namespace fiber::tls
