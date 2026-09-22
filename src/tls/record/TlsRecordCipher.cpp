#include <fiber/tls/record/TlsRecordCipher.h>

#include <cstring>

namespace fiber::tls {

namespace {

struct SuiteSpec {
    const EVP_AEAD *aead = nullptr;
    std::size_t key_len = 0;
    std::size_t iv_len = 0; // static iv (1.3, 12) or fixed iv (1.2, 4)
};

// (suite, kind) pairing resolved through the shared registry
// (handshake/TlsCipherSuites.h); a null aead rejects every other pairing at
// init. The nine implemented combinations and their key/iv lengths all live
// in kTlsSuiteRegistry — this only maps the AEAD pick.
[[nodiscard]] SuiteSpec suite_spec(TlsCipherSuiteId suite, TlsRecordProtectionKind kind) noexcept {
    const TlsSuiteInfo *info = tls_suite_info(suite);
    if (info == nullptr || info->is_tls13 != (kind == TlsRecordProtectionKind::Tls13)) {
        return {};
    }
    const EVP_AEAD *aead = nullptr;
    switch (info->aead) {
        case TlsAeadAlgorithm::Aes128Gcm:
            aead = EVP_aead_aes_128_gcm();
            break;
        case TlsAeadAlgorithm::Aes256Gcm:
            aead = EVP_aead_aes_256_gcm();
            break;
        case TlsAeadAlgorithm::Chacha20Poly1305:
            aead = EVP_aead_chacha20_poly1305();
            break;
    }
    return {aead, info->key_len, static_cast<std::size_t>(info->is_tls13 ? 12 : 4)};
}

void store_be64(std::uint8_t *dst, std::uint64_t value) noexcept {
    for (int i = 7; i >= 0; --i) {
        dst[i] = static_cast<std::uint8_t>(value);
        value >>= 8;
    }
}

[[nodiscard]] std::uint64_t load_be64(const std::uint8_t *src) noexcept {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | src[i];
    }
    return value;
}

// Zero-length regions never overlap anything.
[[nodiscard]] bool regions_overlap(const std::uint8_t *a, std::size_t a_len, const std::uint8_t *b,
                                   std::size_t b_len) noexcept {
    return a_len != 0 && b_len != 0 && a < b + b_len && b < a + a_len;
}

// Strips the TLS 1.3 inner plaintext tail (content || type || zero padding):
// the last non-zero byte is the content type, everything before it is the
// plaintext. Shared by open() and open_scatter(); the caller advances the
// sequence number on Ok.
[[nodiscard]] TlsRecordCipher::OpenResult finish_tls13_inner(std::uint8_t *dst, std::size_t written) noexcept {
    std::size_t end = written;
    while (end > 0 && dst[end - 1] == 0) {
        --end;
    }
    if (end == 0) {
        return {TlsRecordCipher::Status::Malformed, TlsContentType::ApplicationData, 0};
    }
    const std::uint8_t type = dst[end - 1];
    if (type < 20 || type > 23) {
        return {TlsRecordCipher::Status::Malformed, TlsContentType::ApplicationData, 0};
    }
    return {TlsRecordCipher::Status::Ok, static_cast<TlsContentType>(type), end - 1};
}

} // namespace

TlsRecordCipher::~TlsRecordCipher() {
    if (initialized_) {
        EVP_AEAD_CTX_cleanup(&aead_ctx_);
    }
}

// Move core: steal the raw bytes, then zero the source. A zeroed EVP_AEAD_CTX
// is uninitialized (the struct's own invariant), and initialized_ = false
// turns the source destructor into a no-op — no double cleanup is possible.
void TlsRecordCipher::move_from(TlsRecordCipher &src) noexcept {
    std::memcpy(static_cast<void *>(&aead_ctx_), &src.aead_ctx_, sizeof aead_ctx_);
    suite_ = src.suite_;
    kind_ = src.kind_;
    iv_ = src.iv_;
    seq_ = src.seq_;
    initialized_ = src.initialized_;
    std::memset(static_cast<void *>(&src.aead_ctx_), 0, sizeof src.aead_ctx_);
    src.seq_ = 0;
    src.initialized_ = false;
}

TlsRecordCipher::TlsRecordCipher(TlsRecordCipher &&other) noexcept { move_from(other); }

TlsRecordCipher &TlsRecordCipher::operator=(TlsRecordCipher &&other) noexcept {
    if (this != &other) {
        this->~TlsRecordCipher();
        move_from(other);
    }
    return *this;
}

common::IoResult<void> TlsRecordCipher::init(TlsCipherSuiteId suite, TlsRecordProtectionKind kind,
                                             std::span<const std::uint8_t> key,
                                             std::span<const std::uint8_t> iv) noexcept {
    FIBER_ASSERT(!initialized_);

    const SuiteSpec spec = suite_spec(suite, kind);
    if (spec.aead == nullptr || key.size() != spec.key_len || iv.size() != spec.iv_len) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (EVP_AEAD_CTX_init(&aead_ctx_, spec.aead, key.data(), spec.key_len, EVP_AEAD_DEFAULT_TAG_LENGTH, nullptr) != 1) {
        EVP_AEAD_CTX_cleanup(&aead_ctx_);
        return std::unexpected(common::IoErr::Invalid);
    }

    suite_ = suite;
    kind_ = kind;
    std::memcpy(iv_.data(), iv.data(), spec.iv_len);
    seq_ = 0;
    initialized_ = true;
    return {};
}

std::size_t TlsRecordCipher::seal_output_size(std::size_t plaintext_len) const noexcept {
    return plaintext_len + (kind_ == TlsRecordProtectionKind::Tls13 ? 17 : 24);
}

std::size_t TlsRecordCipher::open_output_size(std::size_t ciphertext_len) const noexcept {
    return ciphertext_len - (kind_ == TlsRecordProtectionKind::Tls13 ? 16 : 24);
}

std::size_t TlsRecordCipher::min_ciphertext_size() const noexcept {
    return kind_ == TlsRecordProtectionKind::Tls13 ? 17 : 24;
}

std::size_t TlsRecordCipher::max_ciphertext_size() const noexcept {
    return kind_ == TlsRecordProtectionKind::Tls13 ? kTlsMaxPlaintextSize + kTlsMaxCiphertextOverhead13
                                                   : kTlsMaxPlaintextSize + kTlsMaxCiphertextOverhead12;
}

TlsRecordCipher::SealResult TlsRecordCipher::seal(TlsContentType inner_type, std::span<const std::uint8_t> plaintext,
                                                  std::span<std::uint8_t> dst) noexcept {
    FIBER_ASSERT(initialized_);
    const std::size_t out_len = seal_output_size(plaintext.size());
    FIBER_ASSERT(dst.size() >= out_len);

    const std::size_t iv_len = kind_ == TlsRecordProtectionKind::Tls13 ? 12 : 4;
    std::array<std::uint8_t, 12> nonce{};
    std::array<std::uint8_t, 13> aad{};
    std::size_t aad_len = 0;
    std::size_t written = 0;

    if (kind_ == TlsRecordProtectionKind::Tls13) {
        if (regions_overlap(dst.data(), dst.size(), plaintext.data(), plaintext.size())) {
            FIBER_ASSERT(dst.data() == plaintext.data());
        } else if (dst.data() != plaintext.data()) {
            // The EVP takes one input region: stage the plaintext, then seal
            // in place. The only copy this primitive ever makes.
            std::memcpy(dst.data(), plaintext.data(), plaintext.size());
        }
        dst[plaintext.size()] = static_cast<std::uint8_t>(inner_type);

        std::memcpy(nonce.data(), iv_.data(), iv_len);
        store_be64(nonce.data() + 4, load_be64(nonce.data() + 4) ^ seq_);
        tls_encode_record_header(aad.data(), TlsContentType::ApplicationData, kTlsRecordVersionTls12,
                                 static_cast<std::uint16_t>(out_len));
        aad_len = kTlsRecordHeaderSize;

        if (EVP_AEAD_CTX_seal(&aead_ctx_, dst.data(), &written, dst.size(), nonce.data(), iv_len, dst.data(),
                              plaintext.size() + 1, aad.data(), aad_len) != 1) {
            return {Status::AuthFail, 0};
        }
    } else {
        if (regions_overlap(dst.data(), dst.size(), plaintext.data(), plaintext.size())) {
            FIBER_ASSERT(dst.data() + 8 == plaintext.data());
        }

        store_be64(dst.data(), seq_); // explicit nonce = BE64(seq)
        std::memcpy(nonce.data(), iv_.data(), iv_len);
        std::memcpy(nonce.data() + 4, dst.data(), 8);

        store_be64(aad.data(), seq_);
        aad[8] = static_cast<std::uint8_t>(inner_type);
        aad[9] = 0x03;
        aad[10] = 0x03;
        aad[11] = static_cast<std::uint8_t>(plaintext.size() >> 8);
        aad[12] = static_cast<std::uint8_t>(plaintext.size());
        aad_len = 13;

        if (EVP_AEAD_CTX_seal(&aead_ctx_, dst.data() + 8, &written, dst.size() - 8, nonce.data(), 12, plaintext.data(),
                              plaintext.size(), aad.data(), aad_len) != 1) {
            return {Status::AuthFail, 0};
        }
    }

    // 1.3: EVP output is the whole payload. 1.2: EVP output excludes the
    // 8-byte nonce prefix written at dst's head.
    FIBER_ASSERT(kind_ == TlsRecordProtectionKind::Tls13 ? written == out_len : written == out_len - 8);
    ++seq_;
    return {Status::Ok, out_len};
}

TlsRecordCipher::OpenResult TlsRecordCipher::open(TlsContentType outer_type, std::uint16_t legacy_version,
                                                  std::uint16_t length, std::span<const std::uint8_t> ciphertext,
                                                  std::span<std::uint8_t> dst) noexcept {
    FIBER_ASSERT(initialized_);

    // Length bounds are checked first — before any buffer contract and any
    // crypto: no decrypt oracle, and out-of-range lengths are protocol data,
    // not caller bugs.
    if (length < min_ciphertext_size() || length > max_ciphertext_size()) {
        return {Status::Malformed, TlsContentType::ApplicationData, 0};
    }
    FIBER_ASSERT(ciphertext.size() >= length);

    const std::size_t iv_len = kind_ == TlsRecordProtectionKind::Tls13 ? 12 : 4;
    std::array<std::uint8_t, 12> nonce{};
    std::array<std::uint8_t, 13> aad{};
    std::size_t aad_len = 0;
    std::size_t written = 0;

    if (kind_ == TlsRecordProtectionKind::Tls13) {
        if (outer_type != TlsContentType::ApplicationData) {
            return {Status::Malformed, TlsContentType::ApplicationData, 0};
        }
        FIBER_ASSERT(dst.size() >= open_output_size(length));
        if (regions_overlap(dst.data(), dst.size(), ciphertext.data(), ciphertext.size())) {
            FIBER_ASSERT(dst.data() == ciphertext.data());
        }

        std::memcpy(nonce.data(), iv_.data(), iv_len);
        store_be64(nonce.data() + 4, load_be64(nonce.data() + 4) ^ seq_);
        tls_encode_record_header(aad.data(), outer_type, legacy_version, length); // received header verbatim
        aad_len = kTlsRecordHeaderSize;

        if (EVP_AEAD_CTX_open(&aead_ctx_, dst.data(), &written, dst.size(), nonce.data(), iv_len, ciphertext.data(),
                              length, aad.data(), aad_len) != 1) {
            return {Status::AuthFail, TlsContentType::ApplicationData, 0};
        }
        FIBER_ASSERT(written == length - 16);

        // Strip zero padding; the last non-zero byte is the content type.
        OpenResult result = finish_tls13_inner(dst.data(), written);
        if (result.status == Status::Ok) {
            ++seq_;
        }
        return result;
    }

    FIBER_ASSERT(dst.size() >= open_output_size(length));
    if (regions_overlap(dst.data(), dst.size(), ciphertext.data(), ciphertext.size())) {
        FIBER_ASSERT(dst.data() == ciphertext.data() + 8);
    }

    std::memcpy(nonce.data(), iv_.data(), iv_len);
    std::memcpy(nonce.data() + 4, ciphertext.data(), 8);

    store_be64(aad.data(), seq_);
    aad[8] = static_cast<std::uint8_t>(outer_type);
    aad[9] = static_cast<std::uint8_t>(legacy_version >> 8);
    aad[10] = static_cast<std::uint8_t>(legacy_version);
    const std::uint16_t plain_len = static_cast<std::uint16_t>(length - 24);
    aad[11] = static_cast<std::uint8_t>(plain_len >> 8);
    aad[12] = static_cast<std::uint8_t>(plain_len);
    aad_len = 13;

    // The EVP input spans ciphertext body + tag (everything past the nonce
    // prefix); the AAD still carries the plaintext-only length.
    if (EVP_AEAD_CTX_open(&aead_ctx_, dst.data(), &written, dst.size(), nonce.data(), 12, ciphertext.data() + 8,
                          length - 8, aad.data(), aad_len) != 1) {
        return {Status::AuthFail, TlsContentType::ApplicationData, 0};
    }
    FIBER_ASSERT(written == plain_len);

    ++seq_;
    return {Status::Ok, outer_type, written};
}

TlsRecordCipher::SealResult TlsRecordCipher::seal_scatter(TlsContentType inner_type,
                                                          std::span<const std::uint8_t> plaintext,
                                                          std::span<std::uint8_t> dst_ct,
                                                          std::span<std::uint8_t> dst_prefix,
                                                          std::span<std::uint8_t> dst_tag) noexcept {
    FIBER_ASSERT(initialized_);
    const std::size_t out_len = seal_output_size(plaintext.size());
    FIBER_ASSERT(dst_ct.size() >= plaintext.size());
    // The EVP requires the tag output to not alias any other argument.
    FIBER_ASSERT(!regions_overlap(dst_tag.data(), dst_tag.size(), dst_ct.data(), dst_ct.size()));
    FIBER_ASSERT(!regions_overlap(dst_tag.data(), dst_tag.size(), plaintext.data(), plaintext.size()));

    const std::size_t iv_len = kind_ == TlsRecordProtectionKind::Tls13 ? 12 : 4;
    std::array<std::uint8_t, 12> nonce{};
    std::array<std::uint8_t, 13> aad{};
    std::size_t aad_len = 0;
    std::size_t tag_written = 0;
    const std::uint8_t *extra_in = nullptr;
    std::size_t extra_in_len = 0;
    std::uint8_t type_byte = 0;

    if (kind_ == TlsRecordProtectionKind::Tls13) {
        FIBER_ASSERT(dst_prefix.empty());
        FIBER_ASSERT(dst_tag.size() >= 17);
        if (regions_overlap(dst_ct.data(), dst_ct.size(), plaintext.data(), plaintext.size())) {
            FIBER_ASSERT(dst_ct.data() == plaintext.data());
        }

        // The inner-type byte rides along as extra_in: its ciphertext lands
        // in front of the tag in dst_tag, so it never needs to be physically
        // adjacent to the plaintext.
        type_byte = static_cast<std::uint8_t>(inner_type);
        extra_in = &type_byte;
        extra_in_len = 1;

        std::memcpy(nonce.data(), iv_.data(), iv_len);
        store_be64(nonce.data() + 4, load_be64(nonce.data() + 4) ^ seq_);
        tls_encode_record_header(aad.data(), TlsContentType::ApplicationData, kTlsRecordVersionTls12,
                                 static_cast<std::uint16_t>(out_len));
        aad_len = kTlsRecordHeaderSize;
    } else {
        FIBER_ASSERT(dst_prefix.size() >= 8);
        FIBER_ASSERT(dst_tag.size() >= 16);
        FIBER_ASSERT(!regions_overlap(dst_prefix.data(), 8, dst_ct.data(), dst_ct.size()));
        if (regions_overlap(dst_ct.data(), dst_ct.size(), plaintext.data(), plaintext.size())) {
            FIBER_ASSERT(dst_ct.data() == plaintext.data());
        }

        store_be64(dst_prefix.data(), seq_); // explicit nonce = BE64(seq)
        std::memcpy(nonce.data(), iv_.data(), iv_len);
        std::memcpy(nonce.data() + 4, dst_prefix.data(), 8);

        store_be64(aad.data(), seq_);
        aad[8] = static_cast<std::uint8_t>(inner_type);
        aad[9] = 0x03;
        aad[10] = 0x03;
        aad[11] = static_cast<std::uint8_t>(plaintext.size() >> 8);
        aad[12] = static_cast<std::uint8_t>(plaintext.size());
        aad_len = 13;
    }

    // The AEAD nonce is always 12 bytes here: 1.3 static iv, 1.2 fixed iv
    // (4) || explicit nonce (8) — not iv_len, which is the 1.2 fixed-iv size.
    if (EVP_AEAD_CTX_seal_scatter(&aead_ctx_, dst_ct.data(), dst_tag.data(), &tag_written, dst_tag.size(), nonce.data(),
                                  12, plaintext.data(), plaintext.size(), extra_in, extra_in_len, aad.data(),
                                  aad_len) != 1) {
        return {Status::AuthFail, 0};
    }
    FIBER_ASSERT(tag_written == (kind_ == TlsRecordProtectionKind::Tls13 ? 17 : 16));

    ++seq_;
    return {Status::Ok, out_len};
}

TlsRecordCipher::OpenResult
TlsRecordCipher::open_scatter(TlsContentType outer_type, std::uint16_t legacy_version, std::uint16_t length,
                              std::span<const std::uint8_t> explicit_nonce, std::span<const std::uint8_t> body,
                              std::span<const std::uint8_t> tag, std::span<std::uint8_t> dst) noexcept {
    FIBER_ASSERT(initialized_);
    // Protocol bounds first — before any buffer contract and any crypto (no
    // decrypt oracle; out-of-range lengths are peer data, not caller bugs).
    if (length < min_ciphertext_size() || length > max_ciphertext_size()) {
        return {Status::Malformed, TlsContentType::ApplicationData, 0};
    }

    const std::size_t iv_len = kind_ == TlsRecordProtectionKind::Tls13 ? 12 : 4;
    std::array<std::uint8_t, 12> nonce{};
    std::array<std::uint8_t, 13> aad{};
    std::size_t aad_len = 0;

    if (kind_ == TlsRecordProtectionKind::Tls13) {
        if (outer_type != TlsContentType::ApplicationData) {
            return {Status::Malformed, TlsContentType::ApplicationData, 0};
        }
        FIBER_ASSERT(explicit_nonce.empty());
        FIBER_ASSERT(body.size() >= length - 16);
        FIBER_ASSERT(tag.size() >= 16);
        FIBER_ASSERT(dst.size() >= open_output_size(length));
        if (regions_overlap(dst.data(), dst.size(), body.data(), body.size())) {
            FIBER_ASSERT(dst.data() == body.data());
        }

        std::memcpy(nonce.data(), iv_.data(), iv_len);
        store_be64(nonce.data() + 4, load_be64(nonce.data() + 4) ^ seq_);
        tls_encode_record_header(aad.data(), outer_type, legacy_version, length); // received header verbatim
        aad_len = kTlsRecordHeaderSize;

        // open_gather has no out_len: it writes exactly in_len bytes to dst.
        if (EVP_AEAD_CTX_open_gather(&aead_ctx_, dst.data(), nonce.data(), iv_len, body.data(), length - 16, tag.data(),
                                     16, aad.data(), aad_len) != 1) {
            return {Status::AuthFail, TlsContentType::ApplicationData, 0};
        }

        OpenResult result = finish_tls13_inner(dst.data(), length - 16);
        if (result.status == Status::Ok) {
            ++seq_;
        }
        return result;
    }

    FIBER_ASSERT(explicit_nonce.size() >= 8);
    FIBER_ASSERT(body.size() >= length - 24);
    FIBER_ASSERT(tag.size() >= 16);
    FIBER_ASSERT(dst.size() >= open_output_size(length));
    if (regions_overlap(dst.data(), dst.size(), body.data(), body.size())) {
        FIBER_ASSERT(dst.data() == body.data());
    }

    const std::uint16_t plain_len = static_cast<std::uint16_t>(length - 24);
    std::memcpy(nonce.data(), iv_.data(), iv_len);
    std::memcpy(nonce.data() + 4, explicit_nonce.data(), 8);

    store_be64(aad.data(), seq_);
    aad[8] = static_cast<std::uint8_t>(outer_type);
    aad[9] = static_cast<std::uint8_t>(legacy_version >> 8);
    aad[10] = static_cast<std::uint8_t>(legacy_version);
    aad[11] = static_cast<std::uint8_t>(plain_len >> 8);
    aad[12] = static_cast<std::uint8_t>(plain_len);
    aad_len = 13;

    if (EVP_AEAD_CTX_open_gather(&aead_ctx_, dst.data(), nonce.data(), 12, body.data(), plain_len, tag.data(), 16,
                                 aad.data(), aad_len) != 1) {
        return {Status::AuthFail, TlsContentType::ApplicationData, 0};
    }

    ++seq_;
    return {Status::Ok, outer_type, plain_len};
}

} // namespace fiber::tls
