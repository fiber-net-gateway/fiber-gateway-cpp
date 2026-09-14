#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <fiber/common/mem/BufPool.h>
#include <fiber/compression/GzipEncoder.h>

#include "compression/DeflateEncoder.h"
#include "compression/DeflateState.h"

#include "support/ZlibReference.h"

namespace {

using fiber::compression::DeflateEncoder;
using fiber::compression::DeflateState;
using fiber::compression::EncodeStatus;
using fiber::compression::EncodeStep;
using fiber::mem::BufPool;

// One encoder bound to a pool-allocated state and workspace, mirroring how
// GzipEncoder owns its core.
class CoreEncoder {
public:
    explicit CoreEncoder(int level) {
        workspace_ = pool_.alloc(fiber::compression::kDeflateWorkspaceSize);
        state_ = pool_.alloc<DeflateState>();
        DeflateEncoder::init(*state_, workspace_, level);
    }

    DeflateState &state() noexcept { return *state_; }

private:
    BufPool pool_;
    void *workspace_ = nullptr;
    DeflateState *state_ = nullptr;
};

// ---- corpus builders ----

std::string random_bytes(std::size_t n, std::uint32_t seed) {
    std::mt19937 gen(seed);
    std::uniform_int_distribution<int> dist(0, 255);
    std::string out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(static_cast<char>(dist(gen)));
    }
    return out;
}

std::string repeat_pattern(std::size_t total, std::string_view pattern) {
    std::string out;
    out.reserve(total);
    while (out.size() < total) {
        out.append(pattern.substr(0, std::min(pattern.size(), total - out.size())));
    }
    return out;
}

std::string json_payload(std::size_t target) {
    const std::string pattern = R"({"id":%d,"name":"fiber-gateway","tags":["http","quic","compression"],"ok":true},)";
    return repeat_pattern(target, pattern);
}

std::string html_payload(std::size_t target) {
    const std::string pattern = "<div class=\"row\"><span>value</span><p>lorem ipsum dolor</p></div>\n";
    return repeat_pattern(target, pattern);
}

std::string utf8_payload(std::size_t target) {
    const std::string pattern = "\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c\xf0\x9f\x8c\x8d route-\xc3\xa9";
    return repeat_pattern(target, pattern);
}

std::string precompressed_payload() {
    const auto compressed = fiber::test::zlib_reference_gzip(json_payload(4096), 6);
    return compressed.output;
}

std::string all_byte_values_payload(std::size_t repeats) {
    std::string out;
    out.reserve(256 * repeats);
    for (std::size_t r = 0; r < repeats; ++r) {
        for (int i = 0; i < 256; ++i) {
            out.push_back(static_cast<char>(i));
        }
    }
    return out;
}

std::vector<std::string> corpus(std::size_t scale) {
    return {
            std::string{},           std::string("a"),
            random_bytes(scale, 1),  repeat_pattern(scale, "abcdefgh"),
            json_payload(scale),     html_payload(scale),
            utf8_payload(scale),     all_byte_values_payload(std::max<std::size_t>(1, scale / 2048)),
            precompressed_payload(),
    };
}

// ---- drivers ----

constexpr std::size_t kMaxOutChunk = 64 * 1024;

std::size_t next_chunk(std::mt19937 &rng, std::size_t max, bool randomize, std::size_t fallback) {
    if (!randomize) {
        return fallback;
    }
    return std::uniform_int_distribution<std::size_t>(0, max)(rng);
}

// Drive write() over all of `input`, resuming through NeedOutput. Zero output
// capacities are mixed in when randomize is set; the loop always follows a
// zero-capacity step with at least one byte of room so it keeps progressing.
std::string drive_write_all(CoreEncoder &enc, std::string_view input, std::size_t in_chunk, std::size_t out_chunk,
                            bool randomize, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::string out;
    std::array<std::uint8_t, kMaxOutChunk> buf{};
    std::size_t consumed = 0;
    bool force_room = false;
    while (consumed < input.size()) {
        std::size_t take = std::min(in_chunk, input.size() - consumed);
        if (randomize && take > 1) {
            take = std::uniform_int_distribution<std::size_t>(1, take)(rng);
        }
        while (take > 0) {
            std::size_t cap = next_chunk(rng, out_chunk, randomize, out_chunk);
            if (cap == 0) {
                force_room = true;
            } else if (force_room) {
                cap = std::max<std::size_t>(cap, 1);
                force_room = false;
            }
            cap = std::min(cap, buf.size());
            const EncodeStep step = DeflateEncoder::write(
                    enc.state(), {reinterpret_cast<const std::uint8_t *>(input.data()) + consumed, take},
                    {buf.data(), cap});
            out.append(reinterpret_cast<const char *>(buf.data()), step.written);
            consumed += step.consumed;
            take -= step.consumed;
            EXPECT_TRUE(step.status == EncodeStatus::NeedInput || step.status == EncodeStatus::NeedOutput);
            if (step.status == EncodeStatus::NeedInput) {
                EXPECT_EQ(take, 0u);
                break;
            }
        }
    }
    return out;
}

std::string drive_flush(CoreEncoder &enc, std::size_t out_chunk, bool randomize, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::string out;
    std::array<std::uint8_t, kMaxOutChunk> buf{};
    bool force_room = false;
    for (;;) {
        std::size_t cap = next_chunk(rng, out_chunk, randomize, out_chunk);
        if (cap == 0) {
            force_room = true;
        } else if (force_room) {
            cap = std::max<std::size_t>(cap, 1);
            force_room = false;
        }
        cap = std::min(cap, buf.size());
        const EncodeStep step = DeflateEncoder::flush(enc.state(), {buf.data(), cap});
        out.append(reinterpret_cast<const char *>(buf.data()), step.written);
        if (step.status == EncodeStatus::Flushed) {
            return out;
        }
        EXPECT_EQ(step.status, EncodeStatus::NeedOutput);
    }
}

std::string drive_finish(CoreEncoder &enc, std::size_t out_chunk, bool randomize, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::string out;
    std::array<std::uint8_t, kMaxOutChunk> buf{};
    bool force_room = false;
    for (;;) {
        std::size_t cap = next_chunk(rng, out_chunk, randomize, out_chunk);
        if (cap == 0) {
            force_room = true;
        } else if (force_room) {
            cap = std::max<std::size_t>(cap, 1);
            force_room = false;
        }
        cap = std::min(cap, buf.size());
        const EncodeStep step = DeflateEncoder::finish(enc.state(), {buf.data(), cap});
        out.append(reinterpret_cast<const char *>(buf.data()), step.written);
        if (step.status == EncodeStatus::Finished) {
            return out;
        }
        EXPECT_EQ(step.status, EncodeStatus::NeedOutput);
    }
}

// Full stream: write all input (chunked), optional flush after each burst,
// then finish. The flush points match the reference operation sequence.
std::string drive_stream(int level, const std::vector<std::string_view> &bursts, bool flush_each, std::size_t in_chunk,
                         std::size_t out_chunk, bool randomize, std::uint32_t seed) {
    CoreEncoder enc(level);
    std::string out;
    for (std::string_view burst: bursts) {
        out += drive_write_all(enc, burst, in_chunk, out_chunk, randomize, seed);
        if (flush_each) {
            out += drive_flush(enc, out_chunk, randomize, seed + 1);
        }
    }
    out += drive_finish(enc, out_chunk, randomize, seed + 2);
    return out;
}

std::string reference_raw(std::string_view input, int level) {
    const fiber::test::ZlibReferenceResult r = fiber::test::zlib_reference_deflate(input, level, -15);
    EXPECT_TRUE(r.ok) << "reference deflate status " << r.z_status;
    return r.output;
}

std::string reference_raw_sequence(int level, const std::vector<std::string_view> &bursts, bool flush_each,
                                   std::size_t out_chunk) {
    fiber::test::ZlibReferenceDeflate deflater(level, -15);
    std::string out;
    for (std::string_view burst: bursts) {
        std::size_t offset = 0;
        while (offset < burst.size()) {
            const fiber::test::ZlibReferenceResult r =
                    deflater.step(burst.substr(offset), out_chunk, fiber::test::ZlibReferenceFlush::None);
            EXPECT_TRUE(r.ok);
            out += r.output;
            offset += r.consumed;
            if (r.consumed == 0 && r.output.empty()) {
                ADD_FAILURE() << "reference deflate stalled with input left";
                return out;
            }
        }
        if (flush_each) {
            // Draining the sync marker may take several steps; the next
            // zero-output step (upstream Z_BUF_ERROR duplicate suppression)
            // means the marker is fully out.
            for (;;) {
                const fiber::test::ZlibReferenceResult r =
                        deflater.step({}, out_chunk, fiber::test::ZlibReferenceFlush::Sync);
                EXPECT_TRUE(r.ok);
                out += r.output;
                if (r.output.empty()) {
                    break;
                }
            }
        }
    }
    for (;;) {
        const fiber::test::ZlibReferenceResult r =
                deflater.step({}, out_chunk, fiber::test::ZlibReferenceFlush::Finish);
        EXPECT_TRUE(r.ok);
        out += r.output;
        if (r.stream_end) {
            return out;
        }
    }
}

// DEFLATE block type of the first block: 0 = stored, 1 = fixed Huffman,
// 2 = dynamic Huffman.
int first_block_type(std::string_view raw) {
    EXPECT_FALSE(raw.empty());
    return (raw[0] >> 1) & 0x3;
}

std::string gunzip_raw_incremental(std::string_view raw, std::size_t chunk) {
    fiber::test::ZlibReferenceInflate inflate(-15);
    std::string out;
    std::size_t offset = 0;
    while (offset < raw.size() && !inflate.stream_end()) {
        const fiber::test::ZlibReferenceResult r = inflate.step(raw.substr(offset), chunk);
        EXPECT_TRUE(r.ok) << "reference inflate status " << r.z_status;
        out += r.output;
        offset += r.consumed;
        if (r.consumed == 0 && r.output.empty()) {
            // Input exhausted without the final block: a mid-stream prefix is
            // exactly what the flush-visibility tests decode.
            break;
        }
    }
    return out;
}

} // namespace

// ---- differential: byte equality with upstream for the same parameters ----

TEST(DeflateEncoderTest, MatchesUpstreamOneShotAtEveryLevel) {
    for (int level = 1; level <= 9; ++level) {
        for (const std::string &input: corpus(60000)) {
            const std::string ours = drive_stream(level, {input}, false, 1u << 30, 64 * 1024, false, 1);
            const std::string upstream = reference_raw(input, level);
            ASSERT_EQ(ours, upstream) << "level " << level << " size " << input.size();
        }
    }
}

TEST(DeflateEncoderTest, OutputIndependentOfSlicing) {
    const std::string input = json_payload(90000) + random_bytes(40000, 3) + utf8_payload(20000);
    for (int level: {1, 4, 6, 9}) {
        const std::string reference = reference_raw(input, level);
        for (std::size_t in_chunk: {1u, 2u, 3u, 7u, 258u, 4096u}) {
            for (std::size_t out_chunk: {1u, 2u, 5u, 6u, 7u, 16 * 1024u}) {
                const std::string ours = drive_stream(level, {input}, false, in_chunk, out_chunk, false, 1);
                ASSERT_EQ(ours, reference) << "level " << level << " in " << in_chunk << " out " << out_chunk;
            }
        }
        const std::string randomized = drive_stream(level, {input}, false, 8192, 4096, true, 77);
        ASSERT_EQ(randomized, reference);
    }
}

TEST(DeflateEncoderTest, ZeroOutputSpanIsLegal) {
    CoreEncoder enc(6);
    std::array<std::uint8_t, 64> buf{};
    const EncodeStep step = DeflateEncoder::write(
            enc.state(), {reinterpret_cast<const std::uint8_t *>("hello world"), 11}, {buf.data(), 0});
    // Upstream-faithful: the input is consumed into the window and symbol
    // buffer even with no room to emit anything.
    EXPECT_EQ(step.consumed, 11u);
    EXPECT_EQ(step.written, 0u);
    EXPECT_EQ(step.status, EncodeStatus::NeedInput);
    // The accepted input still lands in the stream: finishing produces the
    // exact reference byte stream for it.
    const std::string rest = drive_finish(enc, 4096, false, 1);
    EXPECT_EQ(rest, reference_raw("hello world", 6));
}

TEST(DeflateEncoderTest, EmptyWriteIsInert) {
    CoreEncoder enc(6);
    std::array<std::uint8_t, 64> buf{};
    const EncodeStep step = DeflateEncoder::write(enc.state(), {}, {buf.data(), buf.size()});
    EXPECT_EQ(step.consumed, 0u);
    EXPECT_EQ(step.written, 0u);
    EXPECT_EQ(step.status, EncodeStatus::NeedInput);

    const std::string rest = drive_write_all(enc, "payload", 4096, 4096, false, 1) + drive_finish(enc, 4096, false, 1);
    EXPECT_EQ(rest, reference_raw("payload", 6));
}

// ---- block types ----

TEST(DeflateEncoderTest, EmitsStoredBlockForIncompressibleData) {
    const std::string input = random_bytes(128 * 1024, 11);
    for (int level: {1, 6, 9}) {
        const std::string ours = drive_stream(level, {input}, false, 1u << 30, 64 * 1024, false, 1);
        EXPECT_EQ(first_block_type(ours), 0) << "level " << level;
        EXPECT_EQ(ours, reference_raw(input, level));
    }
}

TEST(DeflateEncoderTest, EmitsFixedBlockForTinyLiteralInput) {
    for (std::string_view input: {std::string_view("abc"), std::string_view("hello")}) {
        const std::string ours = drive_stream(6, {input}, false, 4096, 4096, false, 1);
        EXPECT_EQ(first_block_type(ours), 1) << "input " << input;
        EXPECT_EQ(ours, reference_raw(input, 6));
    }
}

TEST(DeflateEncoderTest, EmitsDynamicBlockForTextCorpus) {
    const std::string input = json_payload(20000);
    for (int level: {1, 6, 9}) {
        const std::string ours = drive_stream(level, {input}, false, 1u << 30, 64 * 1024, false, 1);
        EXPECT_EQ(first_block_type(ours), 2) << "level " << level;
        EXPECT_EQ(ours, reference_raw(input, level));
    }
}

// ---- window behavior ----

TEST(DeflateEncoderTest, WindowBoundarySizesRoundTripAndMatch) {
    for (std::size_t size: {32766u, 32767u, 32768u, 32769u, 65535u, 65536u, 65537u, 65538u}) {
        const std::string compressible = repeat_pattern(size, "pattern-for-window-boundaries-");
        const std::string incompressible = random_bytes(size, 500 + size);
        for (int level: {1, 6, 9}) {
            const std::string out1 = drive_stream(level, {compressible}, false, 1u << 30, 64 * 1024, false, 1);
            ASSERT_EQ(out1, reference_raw(compressible, level)) << "size " << size;
            ASSERT_EQ(gunzip_raw_incremental(out1, 4096), compressible);
            const std::string out2 = drive_stream(level, {incompressible}, false, 1u << 30, 64 * 1024, false, 1);
            ASSERT_EQ(out2, reference_raw(incompressible, level)) << "size " << size;
            ASSERT_EQ(gunzip_raw_incremental(out2, 4096), incompressible);
        }
    }
}

TEST(DeflateEncoderTest, LargeIncompressibleStreamsMatchUpstream) {
    // Regression for a hash-size porting bug: with 16-bit hash buckets
    // instead of upstream's memLevel+7 = 15, deflate_fast occasionally found
    // matches upstream never sees, shifting the symbol tally and, with it,
    // the stored-block boundaries and BFINAL placement. The minimal diverging
    // input was 65732 random bytes fed as 65536 + 196 (a level-1 run whose
    // symbol buffer then fills exactly during the finish drain).
    for (std::size_t size: {65732u, 256 * 1024u, 1024 * 1024u}) {
        const std::string input = random_bytes(size, 7);
        for (int level: {1, 6, 9}) {
            const std::string ours = drive_stream(level, {input}, false, 64 * 1024, 64 * 1024, false, 1);
            ASSERT_EQ(ours, reference_raw(input, level)) << "size " << size << " level " << level;
        }
    }
}

TEST(DeflateEncoderTest, MatchesAcrossWindowSlideWithLongRepeats) {
    // Period 30000 fits within MAX_DIST (32506), so matches survive slides.
    const std::string period = random_bytes(30000, 21);
    const std::string input = repeat_pattern(200000, period);
    for (int level: {1, 6, 9}) {
        const std::string ours = drive_stream(level, {input}, false, 1u << 30, 64 * 1024, false, 1);
        ASSERT_EQ(ours, reference_raw(input, level));
        ASSERT_EQ(gunzip_raw_incremental(ours, 16384), input);
    }
}

TEST(DeflateEncoderTest, FarMatchNearMaxDist) {
    // A repeat at distance ~32400 (just inside MAX_DIST) and one at ~33000
    // (outside); upstream and the port must agree byte for byte either way.
    for (std::size_t filler_size: {32100u, 33000u}) {
        const std::string chunk = random_bytes(300, 31);
        const std::string filler = random_bytes(filler_size, 32);
        const std::string input = chunk + filler + chunk;
        const std::string ours = drive_stream(6, {input}, false, 1u << 30, 64 * 1024, false, 1);
        ASSERT_EQ(ours, reference_raw(input, 6)) << "filler " << filler_size;
    }
}

TEST(DeflateEncoderTest, MinAndMaxMatchLengths) {
    const std::string min_match = repeat_pattern(3000, "abc");
    const std::string max_match = repeat_pattern(30000, std::string(258, 'x'));
    for (const std::string *input: {&min_match, &max_match}) {
        const std::string ours = drive_stream(6, {*input}, false, 1u << 30, 64 * 1024, false, 1);
        ASSERT_EQ(ours, reference_raw(*input, 6));
        ASSERT_EQ(gunzip_raw_incremental(ours, 4096), *input);
    }
}

// ---- flush semantics ----

TEST(DeflateEncoderTest, FlushEmitsSyncMarkerAndIsDecodableWithoutEof) {
    CoreEncoder enc(6);
    const std::string input = "hello world, hello world, hello";
    const std::string body = drive_write_all(enc, input, 4096, 4096, false, 1);
    const std::string marker = drive_flush(enc, 4096, false, 1);
    EXPECT_GE(marker.size(), 4u);
    EXPECT_EQ(marker.substr(marker.size() - 4), std::string("\x00\x00\xff\xff", 4));

    // Everything before the marker must be decodable without the final block.
    EXPECT_EQ(gunzip_raw_incremental(body + marker, 4096), input);

    const std::string tail = drive_write_all(enc, "!!", 4096, 4096, false, 1) + drive_finish(enc, 4096, false, 1);
    EXPECT_EQ(gunzip_raw_incremental(body + marker + tail, 4096), input + "!!");
}

TEST(DeflateEncoderTest, RepeatedFlushWithoutInputEmitsSingleMarker) {
    CoreEncoder enc(1);
    const std::string first = drive_write_all(enc, "abcdef", 4096, 4096, false, 1);
    EXPECT_TRUE(first.empty()); // nothing forced out by a plain write
    const std::string markers = drive_flush(enc, 4096, false, 1);
    for (int i = 0; i < 3; ++i) {
        const std::string extra = drive_flush(enc, 4096, false, 1);
        EXPECT_TRUE(extra.empty());
    }
    EXPECT_EQ(markers.substr(markers.size() - 4), std::string("\x00\x00\xff\xff", 4));

    const std::string full = first + markers + drive_finish(enc, 4096, false, 1);
    EXPECT_EQ(full, reference_raw_sequence(1, {"abcdef"}, true, 4096));
}

TEST(DeflateEncoderTest, FlushMarkerSplitByteByByte) {
    const std::string input = "streaming visibility";
    const std::string coarse = drive_stream(1, {input}, true, 4096, 4096, false, 1);
    const std::string fine = drive_stream(1, {input}, true, 4096, 1, false, 1);
    EXPECT_EQ(coarse, fine);

    // And the reference agrees for the identical operation sequence.
    EXPECT_EQ(coarse, reference_raw_sequence(1, {input}, true, 4096));
}

TEST(DeflateEncoderTest, FlushOnVirginStreamEmitsBareMarker) {
    // Upstream emits the sync marker even when no input was ever written; the
    // gzip wrapper (not the core) is what suppresses virgin flushes.
    CoreEncoder enc(6);
    std::array<std::uint8_t, 16> buf{};
    const EncodeStep step = DeflateEncoder::flush(enc.state(), {buf.data(), buf.size()});
    ASSERT_EQ(step.status, EncodeStatus::Flushed);
    ASSERT_EQ(step.written, 5u); // 3-bit header + pad + LEN + NLEN
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(buf.data()), 5),
              std::string_view("\x00\x00\x00\xff\xff", 5));

    // The same operation sequence on the reference.
    fiber::test::ZlibReferenceDeflate ref(6, -15);
    std::string expected = ref.step({}, 4096, fiber::test::ZlibReferenceFlush::Sync).output;
    ASSERT_EQ(expected.size(), 5u);

    const std::string rest = drive_write_all(enc, "data", 4096, 4096, false, 1) + drive_finish(enc, 4096, false, 1);
    std::size_t offset = 0;
    while (offset < 4) {
        const fiber::test::ZlibReferenceResult r =
                ref.step(std::string_view{"data"}.substr(offset), 4096, fiber::test::ZlibReferenceFlush::None);
        ASSERT_TRUE(r.ok);
        expected += r.output;
        offset += r.consumed;
    }
    for (;;) {
        const fiber::test::ZlibReferenceResult r = ref.step({}, 4096, fiber::test::ZlibReferenceFlush::Finish);
        ASSERT_TRUE(r.ok);
        expected += r.output;
        if (r.stream_end) {
            break;
        }
    }
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(buf.data()), step.written) + rest, expected);
}

TEST(DeflateEncoderTest, FlushSequenceMatchesUpstreamByteForByte) {
    const std::string a = json_payload(30000);
    const std::string b = random_bytes(5000, 61);
    const std::string c = utf8_payload(10000);
    for (int level: {1, 6, 9}) {
        const std::string ours = drive_stream(level, {a, b, c}, true, 8192, 16 * 1024, false, 5);
        ASSERT_EQ(ours, reference_raw_sequence(level, {a, b, c}, true, 16 * 1024)) << "level " << level;
        ASSERT_EQ(gunzip_raw_incremental(ours, 16384), a + b + c);
    }
}

// ---- finish semantics ----

TEST(DeflateEncoderTest, EmptyStreamFinishIsEmptyFixedBlock) {
    CoreEncoder enc(6);
    const std::string out = drive_finish(enc, 4096, false, 1);
    EXPECT_EQ(out, std::string("\x03\x00", 2));
}

TEST(DeflateEncoderTest, FinishResumableWithOneByteOutput) {
    const std::string input = json_payload(50000);
    const std::string ours = drive_stream(6, {input}, false, 1u << 30, 1, false, 1);
    EXPECT_EQ(ours, reference_raw(input, 6));
}

TEST(DeflateEncoderTest, FinishIsIdempotent) {
    CoreEncoder enc(6);
    (void) drive_write_all(enc, "abc", 4096, 4096, false, 1);
    (void) drive_finish(enc, 4096, false, 1);
    std::array<std::uint8_t, 16> buf{};
    const EncodeStep again = DeflateEncoder::finish(enc.state(), {buf.data(), buf.size()});
    EXPECT_EQ(again.written, 0u);
    EXPECT_EQ(again.status, EncodeStatus::Finished);
}

TEST(DeflateEncoderTest, LastInputFullyConsumedBeforeFinish) {
    // finish() after the final write that ended exactly on a block boundary.
    for (std::size_t size: {16383u, 16384u, 16385u}) {
        const std::string input = repeat_pattern(size, "0123456789abcdef");
        CoreEncoder enc(6);
        const std::string body = drive_write_all(enc, input, size, 1u << 30, false, 1);
        const std::string out = body + drive_finish(enc, 4096, false, 1);
        ASSERT_EQ(out, reference_raw(input, 6)) << "size " << size;
        ASSERT_EQ(gunzip_raw_incremental(out, 4096), input);
    }
}
