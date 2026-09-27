#include <fiber/tls/record/TlsRecordCipherChain.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace fiber::tls {

namespace {

// True when payload bytes [offset, offset + len) lie inside one chain node's
// readable region; *out then points at them. Zero-length regions never
// straddle: *out ends up null only for a zero-length region on an empty chain
// (never dereferenced downstream).
bool chain_contiguous(const mem::IoBufChain &chain, std::size_t offset, std::size_t len,
                      const std::uint8_t **out) noexcept {
    for (const mem::IoBufNode *node = chain.front_node(); node != nullptr; node = node->next) {
        const std::size_t readable = node->buf.readable();
        if (offset >= readable) {
            offset -= readable;
            continue;
        }
        if (offset + len <= readable) {
            *out = node->buf.readable_data() + offset;
            return true;
        }
        return false; // straddles this node's tail
    }
    // `offset` sits at/past the readable end: only an empty region resolves.
    *out = nullptr;
    return len == 0;
}

// Copies payload bytes [offset, offset + len) to dst (exactly len bytes).
void chain_copy_region(const mem::IoBufChain &chain, std::size_t offset, std::size_t len, std::uint8_t *dst) noexcept {
    for (const mem::IoBufNode *node = chain.front_node(); len > 0 && node != nullptr; node = node->next) {
        const std::size_t readable = node->buf.readable();
        if (offset >= readable) {
            offset -= readable;
            continue;
        }
        const std::size_t take = std::min(len, readable - offset);
        std::memcpy(dst, node->buf.readable_data() + offset, take);
        dst += take;
        len -= take;
        offset = 0;
    }
    FIBER_ASSERT(len == 0);
}

// The largest explicit nonce on the wire: the 1.2 CBC IV (GCM carries 8).
constexpr std::size_t kMaxExplicitNonce = 16;

// Record length bounds, checked BEFORE any body/tag size arithmetic: a
// sealed record shorter than its AEAD overhead would underflow
// `length - tag - explicit_nonce` into a huge span (the cipher rejects the
// length too, but only after the spans are formed). Found by the
// tls_server_engine fuzzer via the handshake context's sealed-record path.
[[nodiscard]] bool length_in_bounds(const TlsRecordCipher &cipher, std::uint16_t length) noexcept {
    return length >= cipher.min_ciphertext_size() && length <= cipher.max_ciphertext_size();
}

[[nodiscard]] TlsRecordCipher::OpenResult malformed() noexcept {
    return {TlsRecordCipher::Status::Malformed, TlsContentType::ApplicationData, 0};
}

} // namespace

std::size_t tls_record_open_dst_size(const TlsRecordCipher &cipher, std::uint16_t length) noexcept {
    // Saturating: a length below the explicit nonce is a malformed record
    // (rejected by the open itself), never a wrapped, enormous size. The
    // explicit nonce is 0 at 1.3 by construction.
    const std::size_t nonce = cipher.explicit_nonce_len();
    return length > nonce ? length - nonce : 0;
}

TlsRecordCipher::OpenResult tls_record_open_transcribe(TlsRecordCipher &cipher, TlsContentType outer_type,
                                                       std::uint16_t legacy_version, std::uint16_t length,
                                                       const mem::IoBufChain &payload,
                                                       std::span<std::uint8_t> dst) noexcept {
    FIBER_ASSERT(payload.readable_bytes() == length);
    FIBER_ASSERT(dst.size() >= tls_record_open_dst_size(cipher, length));
    if (!length_in_bounds(cipher, length)) {
        return malformed();
    }

    // Record = explicit nonce (1.2 GCM 8 / CBC 16; 0 otherwise) || body ||
    // detached tag (16; 0 for CBC, whose MAC + padding sit in the body).
    const std::size_t expl = cipher.explicit_nonce_len();
    const std::size_t tag_len = cipher.detached_tag_len();
    const std::size_t body_off = expl;
    const std::size_t body_len = length - tag_len - expl;

    // The nonce is only read to build the AEAD nonce / CBC IV — it never
    // belongs in dst. At most 16 bytes, so staging it on the stack beats
    // requiring the region to be contiguous.
    std::array<std::uint8_t, kMaxExplicitNonce> nonce_prefix{};
    if (expl > 0) {
        chain_copy_region(payload, 0, expl, nonce_prefix.data());
    }
    const std::span<const std::uint8_t> nonce{nonce_prefix.data(), expl};

    const std::uint8_t *body = nullptr;
    const std::uint8_t *tag = nullptr;
    if (chain_contiguous(payload, body_off, body_len, &body) &&
        chain_contiguous(payload, body_off + body_len, tag_len, &tag)) {
        return cipher.open_scatter(outer_type, legacy_version, length, nonce, {body, body_len}, {tag, tag_len}, dst);
    }
    // Stage body+tag in dst, then decrypt in place — the plaintext keeps its
    // home at dst.data().
    chain_copy_region(payload, body_off, length - body_off, dst.data());
    return cipher.open_scatter(outer_type, legacy_version, length, nonce, {dst.data(), body_len},
                               {dst.data() + body_len, tag_len}, {dst.data(), body_len});
}

TlsRecordOpenChainResult tls_record_open_in_place(TlsRecordCipher &cipher, TlsContentType outer_type,
                                                  std::uint16_t legacy_version, std::uint16_t length,
                                                  mem::IoBufChain &payload, std::span<std::uint8_t> dst) noexcept {
    FIBER_ASSERT(payload.readable_bytes() == length);
    FIBER_ASSERT(dst.size() >= tls_record_open_dst_size(cipher, length));
    if (!length_in_bounds(cipher, length)) {
        return {malformed(), false};
    }

    const std::size_t expl = cipher.explicit_nonce_len();
    const std::size_t tag_len = cipher.detached_tag_len();
    const std::size_t body_off = expl;
    const std::size_t body_len = length - tag_len - expl;

    const std::uint8_t *body = nullptr;
    const std::uint8_t *tag = nullptr;
    if (!chain_contiguous(payload, body_off, body_len, &body) ||
        !chain_contiguous(payload, body_off + body_len, tag_len, &tag)) {
        return {tls_record_open_transcribe(cipher, outer_type, legacy_version, length, payload, dst), false};
    }

    // In place over the record's own bytes — the only mutation of the chain.
    auto *body_mut = const_cast<std::uint8_t *>(body);
    std::array<std::uint8_t, kMaxExplicitNonce> nonce_prefix{};
    if (expl > 0) {
        chain_copy_region(payload, 0, expl, nonce_prefix.data());
    }
    const TlsRecordCipher::OpenResult result =
            cipher.open_scatter(outer_type, legacy_version, length, {nonce_prefix.data(), expl}, {body, body_len},
                                {tag, tag_len}, {body_mut, body_len});
    if (result.status != TlsRecordCipher::Status::Ok) {
        return {result, false}; // touched bytes zeroed by the AEAD; view untouched
    }
    // Shrink the view to the plaintext: drop the explicit nonce (1.2 GCM /
    // CBC) and everything past the plaintext — the tag, 1.3's inner type and
    // padding, or CBC's MAC and padding.
    if (expl > 0) {
        payload.consume(expl);
    }
    payload.trim_end(length - expl - result.plain_len);
    return {result, true};
}

TlsRecordCipher::SealResult tls_record_seal_transcribe(TlsRecordCipher &cipher, TlsContentType inner_type,
                                                       const mem::IoBufChain &plaintext,
                                                       std::span<std::uint8_t> dst) noexcept {
    const std::size_t plain = plaintext.readable_bytes();
    FIBER_ASSERT(plain <= kTlsMaxPlaintextSize);
    FIBER_ASSERT(dst.size() >= cipher.seal_output_size(plain));

    const std::size_t off = cipher.explicit_nonce_len(); // 1.2 GCM nonce / CBC IV prefix; 0 otherwise
    const std::uint8_t *pt = nullptr;
    if (chain_contiguous(plaintext, 0, plain, &pt)) {
        // Disjoint dst: the EVP move is the transcription itself.
        if (pt == nullptr) {
            pt = dst.data(); // zero-length plaintext on an empty chain; never dereferenced
        }
        return cipher.seal_scatter(inner_type, {pt, plain}, dst.subspan(off, plain), dst.first(off),
                                   dst.subspan(off + plain));
    }
    // Gather the straddling plaintext into dst — the gather IS the
    // transcription — then seal in place.
    chain_copy_region(plaintext, 0, plain, dst.data() + off);
    return cipher.seal_scatter(inner_type, {dst.data() + off, plain}, {dst.data() + off, plain}, dst.first(off),
                               dst.subspan(off + plain));
}

TlsRecordSealChainResult tls_record_seal_in_place(TlsRecordCipher &cipher, TlsContentType inner_type,
                                                  mem::IoBufChain &plaintext, std::span<std::uint8_t> dst_header,
                                                  std::span<std::uint8_t> dst_tailer,
                                                  std::span<std::uint8_t> dst) noexcept {
    const std::size_t plain = plaintext.readable_bytes();
    FIBER_ASSERT(plain <= kTlsMaxPlaintextSize);
    FIBER_ASSERT(dst.size() >= cipher.seal_output_size(plain));

    const std::uint8_t *pt = nullptr;
    if (!chain_contiguous(plaintext, 0, plain, &pt)) {
        return {tls_record_seal_transcribe(cipher, inner_type, plaintext, dst), false};
    }
    // In place over the plaintext nodes — the only mutation of the chain; no
    // headroom/tailroom is touched, so shared node storage needs no unique().
    if (pt == nullptr) {
        pt = dst.data(); // zero-length plaintext on an empty chain; never dereferenced
    }
    auto *pt_mut = const_cast<std::uint8_t *>(pt);
    TlsRecordCipher::SealResult result =
            cipher.seal_scatter(inner_type, {pt, plain}, {pt_mut, plain}, dst_header, dst_tailer);
    return {result, result.status == TlsRecordCipher::Status::Ok};
}

} // namespace fiber::tls
