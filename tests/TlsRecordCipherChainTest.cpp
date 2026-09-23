#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sys/uio.h>
#include <vector>

#include <fiber/common/mem/IoBuf.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/tls/record/TlsRecordCipher.h>
#include <fiber/tls/record/TlsRecordCipherChain.h>
#include <fiber/tls/record/TlsRecordReader.h>

// The four chain shapes (open/seal x transcribe/in-place) over the span
// primitives: topology routing (contiguous fast path vs one-gather degrade),
// the no-unique() in-place contracts, and view surgery after in-place open.

namespace {

using fiber::mem::IoBuf;
using fiber::mem::IoBufChain;
using fiber::mem::IoBufNodePool;
using fiber::tls::TlsCipherSuiteId;
using fiber::tls::TlsContentType;
using fiber::tls::TlsRecordCipher;
using fiber::tls::TlsRecordProtectionKind;

struct KindVec {
    TlsCipherSuiteId suite;
    TlsRecordProtectionKind kind;
    std::vector<std::uint8_t> key;
    std::vector<std::uint8_t> iv;
};

std::vector<std::uint8_t> key_iv(const char *hex) {
    std::vector<std::uint8_t> out;
    for (; hex[0] && hex[1]; hex += 2) {
        out.push_back(static_cast<std::uint8_t>(std::strtoul(hex, nullptr, 16)));
    }
    return out;
}

const KindVec &tls13_vec() {
    static const KindVec v{TlsCipherSuiteId::TlsAes128GcmSha256, TlsRecordProtectionKind::Tls13,
                           key_iv("3fce516009c21727d0f2e4e86ee403bc"), key_iv("5d313eb2671276ee13000b30")};
    return v;
}

const KindVec &tls12_vec() {
    static const KindVec v{TlsCipherSuiteId::EcdheRsaAes128GcmSha256, TlsRecordProtectionKind::Tls12,
                           key_iv("0102030405060708090a0b0c0d0e0f10"), key_iv("11121314")};
    return v;
}

// The 1.2 ChaCha20 form (RFC 7905 §2): 12-byte implicit IV, no wire nonce —
// exercises the same chain shapes with the nonce prefix length at 0.
const KindVec &tls12_chacha_vec() {
    static const KindVec v{TlsCipherSuiteId::EcdheRsaChacha20Poly1305, TlsRecordProtectionKind::Tls12,
                           key_iv("21022dda596ed5d9acd890e3c63f5051a1b2c3d4e5f60718293a4b5c6d7e8f90"),
                           key_iv("1112131415161718191a1b1c")};
    return v;
}

std::vector<std::uint8_t> ramp(std::size_t len, std::uint8_t seed) {
    std::vector<std::uint8_t> out(len);
    for (std::size_t i = 0; i < len; ++i) {
        out[i] = static_cast<std::uint8_t>(seed + i);
    }
    return out;
}

void init_cipher(TlsRecordCipher &cipher, const KindVec &v) {
    EXPECT_TRUE(cipher.init(v.suite, v.kind, v.key, v.iv).has_value());
}

bool is_tls12(const TlsRecordCipher &cipher) { return cipher.kind() == TlsRecordProtectionKind::Tls12; }

// 1.3 wraps everything as application_data on the wire; 1.2 keeps the type.
TlsContentType outer_for(const TlsRecordCipher &cipher, TlsContentType inner) {
    return is_tls12(cipher) ? inner : TlsContentType::ApplicationData;
}

std::vector<std::uint8_t> seal_wire(TlsRecordCipher &cipher, TlsContentType type,
                                    const std::vector<std::uint8_t> &plain) {
    std::vector<std::uint8_t> wire(cipher.seal_output_size(plain.size()));
    EXPECT_EQ(cipher.seal(type, plain, wire).status, TlsRecordCipher::Status::Ok);
    return wire;
}

IoBufChain make_chain(IoBufNodePool &pool, const std::vector<std::vector<std::uint8_t>> &pieces) {
    IoBufChain chain(pool);
    for (const auto &piece: pieces) {
        IoBuf buf = IoBuf::allocate(piece.size());
        EXPECT_TRUE(buf.valid());
        std::memcpy(buf.writable_data(), piece.data(), piece.size());
        buf.commit(piece.size());
        EXPECT_TRUE(chain.append(std::move(buf)));
    }
    return chain;
}

std::vector<std::vector<std::uint8_t>> split_at(const std::vector<std::uint8_t> &wire,
                                                std::vector<std::size_t> points) {
    std::vector<std::vector<std::uint8_t>> pieces;
    std::size_t prev = 0;
    for (std::size_t point: points) {
        pieces.emplace_back(wire.begin() + static_cast<std::ptrdiff_t>(prev),
                            wire.begin() + static_cast<std::ptrdiff_t>(point));
        prev = point;
    }
    pieces.emplace_back(wire.begin() + static_cast<std::ptrdiff_t>(prev), wire.end());
    return pieces;
}

std::vector<std::uint8_t> chain_bytes(const IoBufChain &chain) {
    std::vector<std::uint8_t> out;
    out.reserve(chain.readable_bytes());
    struct iovec spans[8];
    const int count = chain.fill_write_iov(spans, 8);
    for (int i = 0; i < count; ++i) {
        const auto *begin = static_cast<const std::uint8_t *>(spans[i].iov_base);
        out.insert(out.end(), begin, begin + spans[i].iov_len);
    }
    return out;
}

// ---------------------------------------------------------------- sizes

TEST(TlsRecordChainSizes, OpenDstSizePerKind) {
    TlsRecordCipher c13;
    init_cipher(c13, tls13_vec());
    TlsRecordCipher c12;
    init_cipher(c12, tls12_vec());

    EXPECT_EQ(fiber::tls::tls_record_open_dst_size(c13, 117), 117u); // stages body+tag
    EXPECT_EQ(fiber::tls::tls_record_open_dst_size(c12, 124), 116u); // nonce stays on the chain
}

// ---------------------------------------------------------------- open: transcribe

TEST(TlsRecordChainOpen, TranscribeLeavesTheChainUntouched) {
    for (const KindVec *v: {&tls13_vec(), &tls12_vec(), &tls12_chacha_vec()}) {
        TlsRecordCipher seal_side;
        init_cipher(seal_side, *v);
        TlsRecordCipher opener;
        init_cipher(opener, *v);

        const auto plain = ramp(100, 0x10);
        const TlsContentType type = TlsContentType::Handshake;
        const auto wire = seal_wire(seal_side, type, plain);

        IoBufNodePool pool;
        IoBufChain chain = make_chain(pool, {wire});
        std::vector<std::uint8_t> dst(
                fiber::tls::tls_record_open_dst_size(opener, static_cast<std::uint16_t>(wire.size())));

        const auto r = fiber::tls::tls_record_open_transcribe(opener, outer_for(opener, type), 0x0303,
                                                              static_cast<std::uint16_t>(wire.size()), chain, dst);
        ASSERT_EQ(r.status, TlsRecordCipher::Status::Ok);
        EXPECT_EQ(r.inner_type, type);
        EXPECT_EQ(r.plain_len, plain.size());
        EXPECT_EQ(std::vector<std::uint8_t>(dst.begin(), dst.begin() + r.plain_len), plain);

        EXPECT_EQ(chain.readable_bytes(), wire.size());
        EXPECT_EQ(chain_bytes(chain), wire); // read-only: the ciphertext survives
    }
}

TEST(TlsRecordChainOpen, TranscribeAcrossStraddlingTopologies) {
    const std::size_t plain_len = 100;
    const auto plain = ramp(plain_len, 0x20);

    for (const KindVec *v: {&tls13_vec(), &tls12_vec(), &tls12_chacha_vec()}) {
        TlsRecordCipher seal_side;
        init_cipher(seal_side, *v);

        const TlsContentType type = TlsContentType::ApplicationData;
        const auto wire = seal_wire(seal_side, type, plain);

        const std::size_t off = seal_side.explicit_nonce_len();
        const std::size_t body_end = wire.size() - 16; // tag starts here

        // Mid-body split (staged), tag straddling split (staged), three nodes
        // (staged), and the body/tag node boundary (zero-copy scatter path).
        std::vector<std::vector<std::size_t>> splits{
                {off + 30}, {body_end - 8}, {off + 25, off + 70, body_end}, {body_end}};
        for (const auto &points: splits) {
            TlsRecordCipher opener; // fresh per topology: wire was sealed at seq 0
            init_cipher(opener, *v);
            IoBufNodePool pool;
            IoBufChain chain = make_chain(pool, split_at(wire, points));
            std::vector<std::uint8_t> dst(
                    fiber::tls::tls_record_open_dst_size(opener, static_cast<std::uint16_t>(wire.size())));

            const auto r = fiber::tls::tls_record_open_transcribe(opener, outer_for(opener, type), 0x0303,
                                                                  static_cast<std::uint16_t>(wire.size()), chain, dst);
            ASSERT_EQ(r.status, TlsRecordCipher::Status::Ok) << "split size " << points.size();
            EXPECT_EQ(r.inner_type, type);
            EXPECT_EQ(r.plain_len, plain_len);
            EXPECT_EQ(std::vector<std::uint8_t>(dst.begin(), dst.begin() + plain_len), plain);
        }
    }
}

// ---------------------------------------------------------------- open: in place

TEST(TlsRecordChainOpen, InPlaceShrinksTheChainView) {
    for (const KindVec *v: {&tls13_vec(), &tls12_vec(), &tls12_chacha_vec()}) {
        TlsRecordCipher seal_side;
        init_cipher(seal_side, *v);
        TlsRecordCipher opener;
        init_cipher(opener, *v);

        const auto plain = ramp(90, 0x30);
        const TlsContentType type = TlsContentType::ApplicationData;
        const auto wire = seal_wire(seal_side, type, plain);

        IoBufNodePool pool;
        IoBufChain chain = make_chain(pool, {wire});
        std::vector<std::uint8_t> dst(
                fiber::tls::tls_record_open_dst_size(opener, static_cast<std::uint16_t>(wire.size())));

        const auto r = fiber::tls::tls_record_open_in_place(opener, outer_for(opener, type), 0x0303,
                                                            static_cast<std::uint16_t>(wire.size()), chain, dst);
        ASSERT_EQ(r.open.status, TlsRecordCipher::Status::Ok);
        EXPECT_TRUE(r.in_chain);
        EXPECT_EQ(r.open.inner_type, type);
        EXPECT_EQ(r.open.plain_len, plain.size());
        EXPECT_EQ(chain.readable_bytes(), plain.size()); // tag + nonce + padding are gone
        EXPECT_EQ(chain_bytes(chain), plain);
    }
}

// The scatter win: the body ends exactly at a node boundary and the tag lives
// in the NEXT node — still zero-copy in place.
TEST(TlsRecordChainOpen, InPlaceWorksWhenTagIsInNextNode) {
    for (const KindVec *v: {&tls13_vec(), &tls12_vec(), &tls12_chacha_vec()}) {
        TlsRecordCipher seal_side;
        init_cipher(seal_side, *v);
        TlsRecordCipher opener;
        init_cipher(opener, *v);

        const auto plain = ramp(70, 0x40);
        const TlsContentType type = TlsContentType::Handshake;
        const auto wire = seal_wire(seal_side, type, plain);
        const std::size_t body_end = wire.size() - 16;

        IoBufNodePool pool;
        IoBufChain chain = make_chain(pool, split_at(wire, {body_end}));
        std::vector<std::uint8_t> dst(
                fiber::tls::tls_record_open_dst_size(opener, static_cast<std::uint16_t>(wire.size())));

        const auto r = fiber::tls::tls_record_open_in_place(opener, outer_for(opener, type), 0x0303,
                                                            static_cast<std::uint16_t>(wire.size()), chain, dst);
        ASSERT_EQ(r.open.status, TlsRecordCipher::Status::Ok);
        EXPECT_TRUE(r.in_chain);
        EXPECT_EQ(chain.readable_bytes(), plain.size());
        EXPECT_EQ(chain_bytes(chain), plain);
    }
}

TEST(TlsRecordChainOpen, StraddlingBodyDegradesToTranscribe) {
    const KindVec &v = tls13_vec();
    TlsRecordCipher seal_side;
    init_cipher(seal_side, v);
    TlsRecordCipher opener;
    init_cipher(opener, v);

    const auto plain = ramp(100, 0x50);
    const auto wire = seal_wire(seal_side, TlsContentType::ApplicationData, plain);

    IoBufNodePool pool;
    IoBufChain chain = make_chain(pool, split_at(wire, {60})); // mid-body
    std::vector<std::uint8_t> dst(
            fiber::tls::tls_record_open_dst_size(opener, static_cast<std::uint16_t>(wire.size())));

    const auto r = fiber::tls::tls_record_open_in_place(opener, TlsContentType::ApplicationData, 0x0303,
                                                        static_cast<std::uint16_t>(wire.size()), chain, dst);
    ASSERT_EQ(r.open.status, TlsRecordCipher::Status::Ok);
    EXPECT_FALSE(r.in_chain); // the plaintext went to dst instead
    EXPECT_EQ(r.open.plain_len, plain.size());
    EXPECT_EQ(std::vector<std::uint8_t>(dst.begin(), dst.begin() + plain.size()), plain);
    EXPECT_EQ(chain.readable_bytes(), wire.size());
    EXPECT_EQ(chain_bytes(chain), wire); // untouched
}

TEST(TlsRecordChainOpen, AuthFailDoesNotAdvanceSequence) {
    const KindVec &v = tls13_vec();
    TlsRecordCipher seal_side;
    init_cipher(seal_side, v);
    TlsRecordCipher opener;
    init_cipher(opener, v);

    const auto plain = ramp(50, 0x60);
    auto wire = seal_wire(seal_side, TlsContentType::ApplicationData, plain);
    wire.back() ^= 0x01; // break the tag

    IoBufNodePool pool;
    IoBufChain chain = make_chain(pool, {wire});
    std::vector<std::uint8_t> dst(
            fiber::tls::tls_record_open_dst_size(opener, static_cast<std::uint16_t>(wire.size())));

    const auto r = fiber::tls::tls_record_open_in_place(opener, TlsContentType::ApplicationData, 0x0303,
                                                        static_cast<std::uint16_t>(wire.size()), chain, dst);
    EXPECT_EQ(r.open.status, TlsRecordCipher::Status::AuthFail);
    EXPECT_EQ(opener.sequence(), 0u);
}

// ---------------------------------------------------------------- seal

TEST(TlsRecordChainSeal, TranscribeMatchesSeal) {
    for (const KindVec *v: {&tls13_vec(), &tls12_vec(), &tls12_chacha_vec()}) {
        const auto plain = ramp(120, 0x70);
        const TlsContentType type = TlsContentType::Alert;

        TlsRecordCipher reference;
        init_cipher(reference, *v);
        const auto wire = seal_wire(reference, type, plain);

        IoBufNodePool pool;
        TlsRecordCipher chain_cipher;
        init_cipher(chain_cipher, *v);

        // Single node and three straddling nodes must both reproduce the
        // span seal byte for byte (each against its own sequence number).
        IoBufChain one = make_chain(pool, {plain});
        std::vector<std::uint8_t> dst(chain_cipher.seal_output_size(plain.size()));
        auto r = fiber::tls::tls_record_seal_transcribe(chain_cipher, type, one, dst);
        ASSERT_EQ(r.status, TlsRecordCipher::Status::Ok);
        EXPECT_EQ(dst, wire);

        const auto wire_seq1 = seal_wire(reference, type, plain); // reference is at seq 1 now
        // The straddling nodes must hold exactly `plain`'s bytes.
        const auto p1 = std::vector<std::uint8_t>(plain.begin(), plain.begin() + 30);
        const auto p2 = std::vector<std::uint8_t>(plain.begin() + 30, plain.begin() + 90);
        const auto p3 = std::vector<std::uint8_t>(plain.begin() + 90, plain.end());
        IoBufChain many = make_chain(pool, {p1, p2, p3});
        std::vector<std::uint8_t> dst2(chain_cipher.seal_output_size(plain.size()));
        r = fiber::tls::tls_record_seal_transcribe(chain_cipher, type, many, dst2);
        ASSERT_EQ(r.status, TlsRecordCipher::Status::Ok);
        EXPECT_EQ(dst2, wire_seq1);
    }
}

TEST(TlsRecordChainSeal, InPlaceSplitsWireIntoHeaderChainTailer) {
    for (const KindVec *v: {&tls13_vec(), &tls12_vec(), &tls12_chacha_vec()}) {
        const auto plain = ramp(80, 0x80);
        const TlsContentType type = TlsContentType::ApplicationData;

        TlsRecordCipher reference;
        init_cipher(reference, *v);
        const auto wire = seal_wire(reference, type, plain);

        TlsRecordCipher chain_cipher;
        init_cipher(chain_cipher, *v);
        const std::size_t off = chain_cipher.explicit_nonce_len();

        IoBufNodePool pool;
        IoBufChain chain = make_chain(pool, {plain});
        std::vector<std::uint8_t> header(chain_cipher.explicit_nonce_len());
        std::vector<std::uint8_t> tailer(17);
        std::vector<std::uint8_t> dst(chain_cipher.seal_output_size(plain.size()));

        const auto r = fiber::tls::tls_record_seal_in_place(chain_cipher, type, chain, header, tailer, dst);
        ASSERT_EQ(r.seal.status, TlsRecordCipher::Status::Ok);
        ASSERT_EQ(r.seal.out_len, wire.size());
        EXPECT_TRUE(r.in_chain);
        EXPECT_EQ(chain.readable_bytes(), plain.size()); // length preserved
        EXPECT_EQ(chain_bytes(chain), std::vector<std::uint8_t>(wire.begin() + off, wire.begin() + off + plain.size()));
        EXPECT_EQ(std::vector<std::uint8_t>(header.begin(), header.begin() + off),
                  std::vector<std::uint8_t>(wire.begin(), wire.begin() + off));
        EXPECT_EQ(std::vector<std::uint8_t>(tailer.begin(), tailer.begin() + r.seal.out_len - off - plain.size()),
                  std::vector<std::uint8_t>(wire.begin() + off + plain.size(), wire.end()));
    }
}

// In-place seal writes only the plaintext's own bytes: shared node storage
// (a retain_slice sibling) needs no unique() and observes the ciphertext.
TEST(TlsRecordChainSeal, InPlaceSharedStorageNeedsNoUnique) {
    const KindVec &v = tls13_vec();
    const auto plain = ramp(64, 0x90);

    TlsRecordCipher reference;
    init_cipher(reference, v);
    const auto wire = seal_wire(reference, TlsContentType::ApplicationData, plain);

    TlsRecordCipher chain_cipher;
    init_cipher(chain_cipher, v);

    IoBuf big = IoBuf::allocate(200);
    ASSERT_TRUE(big.valid());
    std::memcpy(big.writable_data(), plain.data(), plain.size());
    big.commit(plain.size());
    IoBuf slice = big.retain_slice(0, plain.size());
    ASSERT_FALSE(slice.unique());

    IoBufNodePool pool;
    IoBufChain chain(pool);
    ASSERT_TRUE(chain.append(std::move(slice)));
    std::vector<std::uint8_t> header;
    std::vector<std::uint8_t> tailer(17);
    std::vector<std::uint8_t> dst(chain_cipher.seal_output_size(plain.size()));

    const auto r = fiber::tls::tls_record_seal_in_place(chain_cipher, TlsContentType::ApplicationData, chain, header,
                                                        tailer, dst);
    ASSERT_EQ(r.seal.status, TlsRecordCipher::Status::Ok);
    EXPECT_TRUE(r.in_chain);
    EXPECT_EQ(std::vector<std::uint8_t>(big.readable_data(), big.readable_data() + plain.size()),
              std::vector<std::uint8_t>(wire.begin(), wire.begin() + plain.size())); // sibling sees ct
    EXPECT_EQ(std::vector<std::uint8_t>(tailer.begin(), tailer.begin() + 17),
              std::vector<std::uint8_t>(wire.begin() + plain.size(), wire.end()));
}

TEST(TlsRecordChainSeal, StraddlingDegradesToTranscribe) {
    const KindVec &v = tls12_vec();
    const auto plain = ramp(90, 0xA0);

    TlsRecordCipher reference;
    init_cipher(reference, v);
    const auto wire = seal_wire(reference, TlsContentType::ApplicationData, plain);

    TlsRecordCipher chain_cipher;
    init_cipher(chain_cipher, v);

    IoBufNodePool pool;
    IoBufChain chain = make_chain(pool, split_at(plain, {40}));
    std::vector<std::uint8_t> header(8);
    std::vector<std::uint8_t> tailer(17);
    std::vector<std::uint8_t> dst(chain_cipher.seal_output_size(plain.size()));

    const auto r = fiber::tls::tls_record_seal_in_place(chain_cipher, TlsContentType::ApplicationData, chain, header,
                                                        tailer, dst);
    ASSERT_EQ(r.seal.status, TlsRecordCipher::Status::Ok);
    EXPECT_FALSE(r.in_chain);
    EXPECT_EQ(dst, wire); // the whole payload landed in dst
    EXPECT_EQ(std::vector<std::uint8_t>(header), std::vector<std::uint8_t>(8, 0)); // untouched
    EXPECT_EQ(std::vector<std::uint8_t>(tailer), std::vector<std::uint8_t>(17, 0));
    EXPECT_EQ(chain_bytes(chain), plain); // plaintext survives
}

TEST(TlsRecordChainSeal, EmptyPlaintextBothForms) {
    for (const KindVec *v: {&tls13_vec(), &tls12_vec(), &tls12_chacha_vec()}) {
        TlsRecordCipher reference;
        init_cipher(reference, *v);
        const auto wire = seal_wire(reference, TlsContentType::Alert, {});

        TlsRecordCipher chain_cipher;
        init_cipher(chain_cipher, *v);
        const std::size_t off = chain_cipher.explicit_nonce_len();

        IoBufNodePool pool;
        IoBufChain empty(pool);

        std::vector<std::uint8_t> dst(chain_cipher.seal_output_size(0));
        auto r = fiber::tls::tls_record_seal_transcribe(chain_cipher, TlsContentType::Alert, empty, dst);
        ASSERT_EQ(r.status, TlsRecordCipher::Status::Ok);
        EXPECT_EQ(r.out_len, wire.size());
        EXPECT_EQ(dst, wire);

        // The in-place form runs at seq 1 — the reference seals again to match.
        const auto wire_seq1 = seal_wire(reference, TlsContentType::Alert, {});
        std::vector<std::uint8_t> header(chain_cipher.explicit_nonce_len());
        std::vector<std::uint8_t> tailer(17);
        std::vector<std::uint8_t> dst2(chain_cipher.seal_output_size(0));
        const auto r2 =
                fiber::tls::tls_record_seal_in_place(chain_cipher, TlsContentType::Alert, empty, header, tailer, dst2);
        ASSERT_EQ(r2.seal.status, TlsRecordCipher::Status::Ok);
        EXPECT_TRUE(r2.in_chain);
        EXPECT_EQ(std::vector<std::uint8_t>(header.begin(), header.begin() + off),
                  std::vector<std::uint8_t>(wire_seq1.begin(), wire_seq1.begin() + off));
        EXPECT_EQ(std::vector<std::uint8_t>(tailer.begin(), tailer.begin() + wire_seq1.size() - off),
                  std::vector<std::uint8_t>(wire_seq1.begin() + off, wire_seq1.end()));
    }
}

// ------------------------------------- full stack: in-place seal -> reader -> in-place open

TEST(TlsRecordChainRoundTrip, InPlaceSealFeedsInPlaceOpen) {
    for (const KindVec *v: {&tls13_vec(), &tls12_vec(), &tls12_chacha_vec()}) {
        TlsRecordCipher seal_side;
        init_cipher(seal_side, *v);
        TlsRecordCipher open_side;
        init_cipher(open_side, *v);

        const auto plain = ramp(150, 0xB0);
        const TlsContentType type = TlsContentType::ApplicationData;
        const std::size_t off = seal_side.explicit_nonce_len();

        IoBufNodePool pool;
        IoBufChain chain = make_chain(pool, {plain});
        std::vector<std::uint8_t> header(seal_side.explicit_nonce_len());
        std::vector<std::uint8_t> tailer(17);
        std::vector<std::uint8_t> scratch(seal_side.seal_output_size(plain.size()));
        const auto s = fiber::tls::tls_record_seal_in_place(seal_side, type, chain, header, tailer, scratch);
        ASSERT_EQ(s.seal.status, TlsRecordCipher::Status::Ok);
        ASSERT_TRUE(s.in_chain);

        // Wire up the record: header || nonce prefix || in-place ct || tailer,
        // split so the received payload straddles at the body/tag boundary
        // (the 1.3 body region includes the encrypted type byte, so the
        // boundary is length - 16 for both kinds).
        std::vector<std::uint8_t> framed;
        framed.reserve(fiber::tls::kTlsRecordHeaderSize + s.seal.out_len);
        std::array<std::uint8_t, fiber::tls::kTlsRecordHeaderSize> record_header{};
        fiber::tls::tls_encode_record_header(record_header.data(), outer_for(seal_side, type), 0x0303,
                                             static_cast<std::uint16_t>(s.seal.out_len));
        framed.insert(framed.end(), record_header.begin(), record_header.end());
        framed.insert(framed.end(), header.begin(), header.begin() + static_cast<std::ptrdiff_t>(off));
        const auto ct_bytes = chain_bytes(chain);
        framed.insert(framed.end(), ct_bytes.begin(), ct_bytes.end());
        framed.insert(framed.end(), tailer.begin(),
                      tailer.begin() + static_cast<std::ptrdiff_t>(s.seal.out_len - off - plain.size()));

        const std::size_t body_end = fiber::tls::kTlsRecordHeaderSize + s.seal.out_len - 16;
        fiber::tls::TlsRecordReader reader(pool);
        for (const auto &piece: split_at(framed, {body_end})) {
            IoBuf buf = IoBuf::allocate(piece.size());
            ASSERT_TRUE(buf.valid());
            std::memcpy(buf.writable_data(), piece.data(), piece.size());
            buf.commit(piece.size());
            ASSERT_TRUE(reader.feed(std::move(buf)));
        }
        auto r = reader.next();
        ASSERT_EQ(r.status, fiber::tls::TlsRecordReader::Result::Status::Ok);
        ASSERT_EQ(r.record.length, s.seal.out_len);

        std::vector<std::uint8_t> dst(fiber::tls::tls_record_open_dst_size(open_side, r.record.length));
        const auto o = fiber::tls::tls_record_open_in_place(open_side, r.record.type, r.record.legacy_version,
                                                            r.record.length, r.record.payload, dst);
        ASSERT_EQ(o.open.status, TlsRecordCipher::Status::Ok);
        EXPECT_TRUE(o.in_chain);
        EXPECT_EQ(o.open.inner_type, type);
        EXPECT_EQ(r.record.payload.readable_bytes(), plain.size());
        EXPECT_EQ(chain_bytes(r.record.payload), plain);
    }
}

} // namespace
