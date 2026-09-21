#ifndef FIBER_TLS_RECORD_TLS_RECORD_CIPHER_CHAIN_H
#define FIBER_TLS_RECORD_TLS_RECORD_CIPHER_CHAIN_H

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../common/mem/IoBufChain.h"
#include "TlsRecord.h"
#include "TlsRecordCipher.h"

namespace fiber::tls {

// Chain-facing record transforms: the four IoBufChain shapes on top of the
// span primitives in TlsRecordCipher. The AEAD takes one contiguous input
// region, so these functions adapt each record's chain topology:
//
//   open  transcribe   chain in (any topology), plaintext always at dst.data()
//   open  in place     decrypt into the record's own bytes, shrink the view
//   seal  transcribe   chain in (any topology), wire payload at dst.data()
//   seal  in place     ciphertext over the plaintext nodes, nonce/tag in
//                      caller-provided prefix/suffix spans
//
// Contract shared by every entry point (asserted here, at the boundary):
//   - open:  payload.readable_bytes() == length — the chain holds exactly one
//     complete record (TlsRecordReader::next() output). Out-of-range lengths
//     remain PROTOCOL failures (cipher returns Malformed, no assert).
//   - seal:  plaintext.readable_bytes() <= kTlsMaxPlaintextSize — the Writer
//     chunking contract; more is a caller bug.
//
// All four are allocation-free: straddling topologies degrade to one gather
// into the caller-provided dst (the unavoidable copy under the EVP's
// single-input-region contract).

// Worst-case dst capacity for the open entry points below: the record minus a
// 1.2 explicit nonce. The straddling path stages body+tag in dst and decrypts
// in place; a contiguous record uses less (the capacity still must be sized
// for the worst case — the path choice is data-dependent).
[[nodiscard]] std::size_t tls_record_open_dst_size(const TlsRecordCipher &cipher, std::uint16_t length) noexcept;

// Open, transcribed: plaintext always lands at dst.data() (1.2 included — no
// nonce offset), the chain is only read. Contiguous record: zero copies (the
// EVP moves the plaintext natively). Straddling: one gather into dst.
[[nodiscard]] TlsRecordCipher::OpenResult tls_record_open_transcribe(TlsRecordCipher &cipher, TlsContentType outer_type,
                                                                     std::uint16_t legacy_version, std::uint16_t length,
                                                                     const mem::IoBufChain &payload,
                                                                     std::span<std::uint8_t> dst) noexcept;

struct TlsRecordOpenChainResult {
    TlsRecordCipher::OpenResult open;
    // true: the plaintext IS the chain's readable prefix (view already
    // shrunk — tag, 1.2 nonce, 1.3 type+padding are gone). false: the record
    // straddled nodes and the plaintext is at dst.data() instead; the chain
    // is left untouched (its bytes still ciphertext). On a failing status the
    // AEAD has zeroed the touched bytes and the flag carries no meaning —
    // the connection is fatal either way.
    bool in_chain = false;
};

// Open, in place: decrypts into the record's own readable bytes — only the
// record's own region is ever written, so NO unique() requirement on the node
// storage — then shrinks the view (1.3: trim the tag+type+padding; 1.2: the
// explicit nonce is consumed and the tag trimmed). Eligible whenever the
// ciphertext body and the tag are each contiguous (the body may end at a node
// boundary with the tag in the next node); otherwise degrades to transcribe
// into dst.
[[nodiscard]] TlsRecordOpenChainResult tls_record_open_in_place(TlsRecordCipher &cipher, TlsContentType outer_type,
                                                                std::uint16_t legacy_version, std::uint16_t length,
                                                                mem::IoBufChain &payload,
                                                                std::span<std::uint8_t> dst) noexcept;

// Seal, transcribed: the full protected payload (1.2 nonce prefix + ct + tag)
// lands at dst.data(), sized by cipher.seal_output_size(). Contiguous
// plaintext: the EVP move is the transcription itself. Straddling: one gather
// first (the gather is the transcription). The chain is only read.
[[nodiscard]] TlsRecordCipher::SealResult tls_record_seal_transcribe(TlsRecordCipher &cipher, TlsContentType inner_type,
                                                                     const mem::IoBufChain &plaintext,
                                                                     std::span<std::uint8_t> dst) noexcept;

struct TlsRecordSealChainResult {
    TlsRecordCipher::SealResult seal;
    // true: the wire payload is dst_header || chain || dst_tailer[0..tag_len]
    // — the chain's readable region (same length as the plaintext) is now
    // ciphertext, written in place with no headroom/tailroom touch (hence no
    // unique() requirement). false: the plaintext straddled nodes and the
    // whole payload is at dst.data() instead; dst_header/dst_tailer are
    // untouched. On a failing status the AEAD zeroed what it touched.
    bool in_chain = false;
};

// Seal, in place with external prefix/suffix: the ciphertext overwrites the
// plaintext nodes (length preserved), 1.2's explicit nonce goes to dst_header
// (8 bytes; must be EMPTY for 1.3 — it has no wire prefix) and the tag to
// dst_tailer (1.3: 17 bytes, encrypted inner type || tag; 1.2: 16 bytes).
// Eligible whenever the plaintext is contiguous in one node; otherwise
// degrades to transcribe into dst (sized by seal_output_size()).
[[nodiscard]] TlsRecordSealChainResult tls_record_seal_in_place(TlsRecordCipher &cipher, TlsContentType inner_type,
                                                                mem::IoBufChain &plaintext,
                                                                std::span<std::uint8_t> dst_header,
                                                                std::span<std::uint8_t> dst_tailer,
                                                                std::span<std::uint8_t> dst) noexcept;

} // namespace fiber::tls

#endif // FIBER_TLS_RECORD_TLS_RECORD_CIPHER_CHAIN_H
