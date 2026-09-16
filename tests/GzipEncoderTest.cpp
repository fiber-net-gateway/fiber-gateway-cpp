#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <fiber/common/IoError.h>
#include <fiber/common/mem/BufPool.h>
#include <fiber/common/util/Crc32.h>
#include <fiber/compression/GzipEncoder.h>

#include "support/ZlibReference.h"

namespace {

using fiber::common::IoErr;
using fiber::compression::EncodeStatus;
using fiber::compression::GzipEncoder;
using fiber::mem::BufPool;

// Count allocations on this thread so the steady-state test can prove that
// write/flush cycles after initialization do not allocate.
thread_local std::size_t g_thread_allocations = 0;

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
    return repeat_pattern(target, R"({"id":%d,"name":"fiber-gateway","tags":["http","quic"],"ok":true},)");
}

std::string html_payload(std::size_t target) {
    return repeat_pattern(target, "<div class=\"row\"><span>value</span><p>lorem ipsum dolor</p></div>\n");
}

std::string utf8_payload(std::size_t target) {
    return repeat_pattern(target, "\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c\xf0\x9f\x8c\x8d route-\xc3\xa9");
}

std::string precompressed_payload() { return fiber::test::zlib_reference_gzip(json_payload(4096), 6).output; }

std::vector<std::string> corpus(std::size_t scale) {
    return {
            std::string{},
            std::string("a"),
            std::string("hello hello hello hello"),
            random_bytes(scale, 1),
            repeat_pattern(scale, "abcdefgh"),
            json_payload(scale),
            html_payload(scale),
            utf8_payload(scale),
            precompressed_payload(),
    };
}

// ---- gzip drivers ----

constexpr std::size_t kMaxOutChunk = 64 * 1024;

class ScopedEncoder {
public:
    ScopedEncoder(BufPool &pool, int level) {
        auto created = GzipEncoder::create(pool, fiber::compression::GzipEncoderOptions{level});
        if (created.has_value()) {
            encoder_ = *created;
        }
    }

    explicit operator bool() const noexcept { return encoder_ != nullptr; }
    GzipEncoder &operator*() const noexcept { return *encoder_; }
    GzipEncoder *operator->() const noexcept { return encoder_; }

private:
    GzipEncoder *encoder_ = nullptr;
};

std::size_t next_out_chunk(std::mt19937 &rng, std::size_t max, bool randomize) {
    if (!randomize) {
        return max;
    }
    return std::uniform_int_distribution<std::size_t>(0, max)(rng);
}

std::string drive_write_all(GzipEncoder &enc, std::string_view input, std::size_t in_chunk, std::size_t out_chunk,
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
            std::size_t cap = next_out_chunk(rng, out_chunk, randomize);
            if (cap == 0) {
                force_room = true;
            } else if (force_room) {
                cap = std::max<std::size_t>(cap, 1);
                force_room = false;
            }
            cap = std::min(cap, buf.size());
            const auto step = enc.write({reinterpret_cast<const std::uint8_t *>(input.data()) + consumed, take},
                                        {buf.data(), cap});
            EXPECT_TRUE(step.has_value());
            out.append(reinterpret_cast<const char *>(buf.data()), step->written);
            consumed += step->consumed;
            take -= step->consumed;
            if (step->status == EncodeStatus::NeedInput) {
                EXPECT_EQ(take, 0u);
                break;
            }
            EXPECT_EQ(step->status, EncodeStatus::NeedOutput);
        }
    }
    return out;
}

std::string drive_flush(GzipEncoder &enc, std::size_t out_chunk, bool randomize, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::string out;
    std::array<std::uint8_t, kMaxOutChunk> buf{};
    bool force_room = false;
    for (;;) {
        std::size_t cap = next_out_chunk(rng, out_chunk, randomize);
        if (cap == 0) {
            force_room = true;
        } else if (force_room) {
            cap = std::max<std::size_t>(cap, 1);
            force_room = false;
        }
        cap = std::min(cap, buf.size());
        const auto step = enc.flush({buf.data(), cap});
        EXPECT_TRUE(step.has_value());
        out.append(reinterpret_cast<const char *>(buf.data()), step->written);
        if (step->status == EncodeStatus::Flushed) {
            return out;
        }
        EXPECT_EQ(step->status, EncodeStatus::NeedOutput);
    }
}

std::string drive_finish(GzipEncoder &enc, std::size_t out_chunk, bool randomize, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::string out;
    std::array<std::uint8_t, kMaxOutChunk> buf{};
    bool force_room = false;
    for (;;) {
        std::size_t cap = next_out_chunk(rng, out_chunk, randomize);
        if (cap == 0) {
            force_room = true;
        } else if (force_room) {
            cap = std::max<std::size_t>(cap, 1);
            force_room = false;
        }
        cap = std::min(cap, buf.size());
        const auto step = enc.finish({buf.data(), cap});
        EXPECT_TRUE(step.has_value());
        out.append(reinterpret_cast<const char *>(buf.data()), step->written);
        if (step->status == EncodeStatus::Finished) {
            return out;
        }
        EXPECT_EQ(step->status, EncodeStatus::NeedOutput);
    }
}

std::string gzip_stream(BufPool &pool, int level, const std::vector<std::string_view> &bursts, bool flush_each,
                        std::size_t in_chunk, std::size_t out_chunk, bool randomize, std::uint32_t seed) {
    ScopedEncoder enc(pool, level);
    EXPECT_TRUE(static_cast<bool>(enc));
    std::string out;
    for (std::string_view burst: bursts) {
        out += drive_write_all(*enc, burst, in_chunk, out_chunk, randomize, seed);
        if (flush_each) {
            out += drive_flush(*enc, out_chunk, randomize, seed + 1);
        }
    }
    out += drive_finish(*enc, out_chunk, randomize, seed + 2);
    return out;
}

std::string reference_gzip(std::string_view input, int level) {
    const fiber::test::ZlibReferenceResult r = fiber::test::zlib_reference_gzip(input, level);
    EXPECT_TRUE(r.ok) << "reference gzip status " << r.z_status;
    return r.output;
}

std::string reference_gzip_sequence(int level, const std::vector<std::string_view> &bursts, bool flush_each,
                                    std::size_t out_chunk) {
    fiber::test::ZlibReferenceDeflate deflater(level, 15 + 16);
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

std::string gunzip_incremental(std::string_view member, std::size_t chunk) {
    const fiber::test::ZlibReferenceResult r = fiber::test::zlib_reference_gunzip(member, false, chunk);
    EXPECT_TRUE(r.ok) << "reference gunzip status " << r.z_status;
    return r.output;
}

} // namespace

// Global allocation counter used by the steady-state test below.
//
// The TSan runtime installs its own new/delete interceptors, so replacing
// them here would collide at link time. Without the replacement the counter
// stays zero and the steady-state check passes vacuously under TSan; every
// other build runs the real assertion.

#if !defined(__SANITIZE_THREAD__) && !(defined(__has_feature) && __has_feature(thread_sanitizer))

void *operator new(std::size_t size) {
    ++g_thread_allocations;
    void *ptr = std::malloc(size ? size : 1);
    if (ptr == nullptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

void operator delete(void *ptr) noexcept { std::free(ptr); }
void operator delete(void *ptr, std::size_t) noexcept { std::free(ptr); }
void *operator new[](std::size_t size) { return operator new(size); }
void operator delete[](void *ptr) noexcept { std::free(ptr); }
void operator delete[](void *ptr, std::size_t) noexcept { std::free(ptr); }

#endif

// ---- creation ----

TEST(GzipEncoderTest, CreateRejectsInvalidLevels) {
    BufPool pool;
    for (int level: {0, -1, -5, 10, 100}) {
        const auto created = GzipEncoder::create(pool, fiber::compression::GzipEncoderOptions{level});
        ASSERT_FALSE(created.has_value());
        EXPECT_EQ(created.error(), IoErr::Invalid);
    }
    for (int level = 1; level <= 9; ++level) {
        const auto created = GzipEncoder::create(pool, fiber::compression::GzipEncoderOptions{level});
        ASSERT_TRUE(created.has_value());
    }
}

// ---- framing ----

TEST(GzipEncoderTest, HeaderFieldsFollowLevelAndPlatform) {
    BufPool pool;
    const std::uint8_t expect_os =
#if defined(__APPLE__)
            19;
#else
            3;
#endif
    struct LevelXfl {
        int level;
        std::uint8_t xfl;
    };
    const LevelXfl specs[] = {{1, 4}, {6, 0}, {9, 2}};
    for (const LevelXfl &spec: specs) {
        const std::string member = gzip_stream(pool, spec.level, {std::string_view("")}, false, 4096, 4096, false, 1);
        ASSERT_GE(member.size(), 18u);
        EXPECT_EQ(static_cast<std::uint8_t>(member[0]), 0x1fu);
        EXPECT_EQ(static_cast<std::uint8_t>(member[1]), 0x8bu);
        EXPECT_EQ(static_cast<std::uint8_t>(member[2]), 8u); // CM = deflate
        EXPECT_EQ(static_cast<std::uint8_t>(member[3]), 0u); // FLG: no extras
        EXPECT_EQ(member.substr(4, 4), std::string("\x00\x00\x00\x00", 4)); // MTIME = 0
        EXPECT_EQ(static_cast<std::uint8_t>(member[8]), spec.xfl) << "level " << spec.level;
        EXPECT_EQ(static_cast<std::uint8_t>(member[9]), expect_os);
    }
}

TEST(GzipEncoderTest, EmptyStreamFinishIsEmptyMember) {
    BufPool pool;
    for (int level: {1, 5, 9}) {
        const std::string member = gzip_stream(pool, level, {std::string_view("")}, false, 4096, 4096, false, 1);
        EXPECT_EQ(member, reference_gzip("", level));
        ASSERT_EQ(member.size(), 20u); // 10 header + 2 final empty block + 8 trailer
        EXPECT_EQ(member.substr(10), std::string("\x03\x00", 2) + std::string(8, '\x00'));
        EXPECT_TRUE(gunzip_incremental(member, 4096).empty());
    }
}

TEST(GzipEncoderTest, TrailerCarriesCrcAndLittleEndianIsize) {
    BufPool pool;
    const std::string input = json_payload(50000) + utf8_payload(1000);
    const std::string member = gzip_stream(pool, 6, {input}, false, 1u << 30, 64 * 1024, false, 1);
    ASSERT_GE(member.size(), 18u);
    const std::string trailer = member.substr(member.size() - 8);
    const std::uint32_t crc = fiber::util::Crc32::compute(input);
    const std::uint32_t isize = static_cast<std::uint32_t>(input.size());
    std::array<char, 8> expected{};
    for (int i = 0; i < 4; ++i) {
        expected[i] = static_cast<char>(crc >> (8 * i));
        expected[4 + i] = static_cast<char>(isize >> (8 * i));
    }
    const std::string_view expected_view{expected.data(), 8};
    EXPECT_EQ(trailer, expected_view);
    EXPECT_EQ(gunzip_incremental(member, 8192), input);
}

// ---- differential byte equality ----

TEST(GzipEncoderTest, MatchesUpstreamAtEveryLevel) {
    BufPool pool;
    for (int level = 1; level <= 9; ++level) {
        for (const std::string &input: corpus(60000)) {
            ASSERT_EQ(gzip_stream(pool, level, {input}, false, 1u << 30, 64 * 1024, false, 1),
                      reference_gzip(input, level))
                    << "level " << level << " size " << input.size();
        }
    }
}

TEST(GzipEncoderTest, OutputIndependentOfSlicing) {
    BufPool pool;
    const std::string input = json_payload(90000) + random_bytes(40000, 3) + utf8_payload(20000);
    const std::string reference = reference_gzip(input, 6);
    for (std::size_t in_chunk: {1u, 2u, 3u, 7u, 4096u}) {
        for (std::size_t out_chunk: {1u, 2u, 5u, 6u, 7u, 16 * 1024u}) {
            ASSERT_EQ(gzip_stream(pool, 6, {input}, false, in_chunk, out_chunk, false, 1), reference)
                    << "in " << in_chunk << " out " << out_chunk;
        }
    }
    ASSERT_EQ(gzip_stream(pool, 6, {input}, false, 8192, 4096, true, 77), reference);
}

TEST(GzipEncoderTest, FlushSequencesMatchUpstreamByteForByte) {
    BufPool pool;
    const std::string a = json_payload(30000);
    const std::string b = random_bytes(5000, 61);
    const std::string c = utf8_payload(10000);
    for (int level: {1, 6, 9}) {
        const std::string ours = gzip_stream(pool, level, {a, b, c}, true, 8192, 16 * 1024, false, 5);
        ASSERT_EQ(ours, reference_gzip_sequence(level, {a, b, c}, true, 16 * 1024)) << "level " << level;
        ASSERT_EQ(gunzip_incremental(ours, 16384), a + b + c);
    }
}

// ---- operation state machine ----

TEST(GzipEncoderTest, EmptyWriteDoesNotStartTheStream) {
    BufPool pool;
    ScopedEncoder enc(pool, 6);
    ASSERT_TRUE(static_cast<bool>(enc));
    std::array<std::uint8_t, 64> buf{};
    const auto step = enc->write({}, {buf.data(), buf.size()});
    ASSERT_TRUE(step.has_value());
    EXPECT_EQ(step->consumed, 0u);
    EXPECT_EQ(step->written, 0u);
    EXPECT_EQ(step->status, EncodeStatus::NeedInput);

    const std::string rest =
            drive_write_all(*enc, "payload", 4096, 4096, false, 1) + drive_finish(*enc, 4096, false, 1);
    EXPECT_EQ(rest, reference_gzip("payload", 6));
}

TEST(GzipEncoderTest, VirginFlushProducesNothingAndStartsNoHeader) {
    BufPool pool;
    ScopedEncoder enc(pool, 6);
    ASSERT_TRUE(static_cast<bool>(enc));
    std::array<std::uint8_t, 64> buf{};
    const auto step = enc->flush({buf.data(), buf.size()});
    ASSERT_TRUE(step.has_value());
    EXPECT_EQ(step->written, 0u);
    EXPECT_EQ(step->status, EncodeStatus::Flushed);

    const std::string rest = drive_write_all(*enc, "data", 4096, 4096, false, 1) + drive_finish(*enc, 4096, false, 1);
    EXPECT_EQ(rest, reference_gzip("data", 6));
}

TEST(GzipEncoderTest, ZeroOutputSpanIsLegal) {
    BufPool pool;
    ScopedEncoder enc(pool, 6);
    ASSERT_TRUE(static_cast<bool>(enc));
    std::array<std::uint8_t, 64> buf{};
    const auto step = enc->write({reinterpret_cast<const std::uint8_t *>("hello"), 5}, {buf.data(), 0});
    ASSERT_TRUE(step.has_value());
    // The wrapper contract: with no output room nothing is consumed either;
    // the caller resubmits the input once space exists.
    EXPECT_EQ(step->consumed, 0u);
    EXPECT_EQ(step->written, 0u);
    EXPECT_EQ(step->status, EncodeStatus::NeedOutput);

    const std::string rest = drive_write_all(*enc, "hello", 4096, 4096, false, 1) + drive_finish(*enc, 4096, false, 1);
    EXPECT_EQ(rest, reference_gzip("hello", 6));
}

TEST(GzipEncoderTest, HeaderCompletesAcrossTinyOutputs) {
    BufPool pool;
    ScopedEncoder enc(pool, 1);
    ASSERT_TRUE(static_cast<bool>(enc));
    const std::string head = drive_write_all(*enc, "abc", 4096, 1, false, 1);
    ASSERT_EQ(head.size(), 10u); // header only; the core holds "abc" back
    ASSERT_EQ(drive_finish(*enc, 4096, false, 1), reference_gzip("abc", 1).substr(10));
}

TEST(GzipEncoderTest, FlushBarrierIsDecodableBeforeEof) {
    BufPool pool;
    ScopedEncoder enc(pool, 6);
    ASSERT_TRUE(static_cast<bool>(enc));
    const std::string first =
            drive_write_all(*enc, "part-one-", 4096, 4096, false, 1) + drive_flush(*enc, 4096, false, 1);
    ASSERT_FALSE(first.empty());

    // Before any EOF, a decoder must recover everything written so far.
    fiber::test::ZlibReferenceInflate inflate(15 + 16);
    const fiber::test::ZlibReferenceResult part = inflate.step(first, 4096);
    ASSERT_TRUE(part.ok);
    EXPECT_EQ(part.output, "part-one-");
    EXPECT_FALSE(inflate.stream_end());

    const std::string tail =
            drive_write_all(*enc, "part-two", 4096, 4096, false, 1) + drive_finish(*enc, 4096, false, 1);
    const fiber::test::ZlibReferenceResult rest = inflate.step(tail, 4096);
    ASSERT_TRUE(rest.ok);
    EXPECT_EQ(rest.output, "part-two");
    EXPECT_TRUE(inflate.stream_end());
}

TEST(GzipEncoderTest, FlushCompletesPartiallySentHeader) {
    BufPool pool;
    ScopedEncoder enc(pool, 1);
    ASSERT_TRUE(static_cast<bool>(enc));
    std::array<std::uint8_t, 16> buf{};
    const auto step = enc->write({reinterpret_cast<const std::uint8_t *>("body"), 4}, {buf.data(), 5});
    ASSERT_TRUE(step.has_value());
    EXPECT_EQ(step->written, 5u); // first five header bytes
    EXPECT_EQ(step->consumed, 0u);
    EXPECT_EQ(step->status, EncodeStatus::NeedOutput);

    // With no input accepted yet, flush only finishes the header (the wrapper
    // suppresses virgin markers); the caller must still resubmit "body".
    const std::string completed = drive_flush(*enc, 4096, false, 1);
    ASSERT_EQ(completed.size(), 5u); // header bytes 5..9: MTIME zeros, XFL=4, OS
    EXPECT_EQ(completed, std::string("\x00\x00\x00\x04\x03", 5));

    const std::string rest = drive_write_all(*enc, "body", 4096, 4096, false, 1) + drive_finish(*enc, 4096, false, 1);
    const std::string member =
            std::string(reinterpret_cast<const char *>(buf.data()), step->written) + completed + rest;
    EXPECT_EQ(member, reference_gzip("body", 1));
}

TEST(GzipEncoderTest, RepeatedFinishIsIdempotentAndGuardsFollow) {
    BufPool pool;
    ScopedEncoder enc(pool, 6);
    ASSERT_TRUE(static_cast<bool>(enc));
    (void) drive_write_all(*enc, "abc", 4096, 4096, false, 1);
    (void) drive_finish(*enc, 4096, false, 1);

    std::array<std::uint8_t, 16> buf{};
    const auto again = enc->finish({buf.data(), buf.size()});
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->written, 0u);
    EXPECT_EQ(again->status, EncodeStatus::Finished);

    const auto write_late = enc->write({reinterpret_cast<const std::uint8_t *>("x"), 1}, {buf.data(), buf.size()});
    ASSERT_FALSE(write_late.has_value());
    EXPECT_EQ(write_late.error(), IoErr::Already);
    const auto flush_late = enc->flush({buf.data(), buf.size()});
    ASSERT_FALSE(flush_late.has_value());
    EXPECT_EQ(flush_late.error(), IoErr::Already);
}

TEST(GzipEncoderTest, FinishDuringUnfinishedFlushIsBusy) {
    BufPool pool;
    ScopedEncoder enc(pool, 6);
    ASSERT_TRUE(static_cast<bool>(enc));
    const std::string body = json_payload(40000);
    std::string out = drive_write_all(*enc, body, 4096, 4096, false, 1);

    // Start a flush with a single byte of output, leaving it mid-flush.
    std::array<std::uint8_t, 1> tiny{};
    const auto partial = enc->flush({tiny.data(), tiny.size()});
    ASSERT_TRUE(partial.has_value());
    EXPECT_EQ(partial->status, EncodeStatus::NeedOutput);

    std::array<std::uint8_t, 64> buf{};
    const auto early_finish = enc->finish({buf.data(), buf.size()});
    ASSERT_FALSE(early_finish.has_value());
    EXPECT_EQ(early_finish.error(), IoErr::Busy);
    const auto late_write = enc->write({reinterpret_cast<const std::uint8_t *>("x"), 1}, {buf.data(), buf.size()});
    ASSERT_FALSE(late_write.has_value());
    EXPECT_EQ(late_write.error(), IoErr::Busy);

    // Drain the flush, then finish normally.
    out.append(reinterpret_cast<const char *>(tiny.data()), partial->written);
    for (;;) {
        std::array<std::uint8_t, 4096> chunk{};
        const auto flushed = enc->flush({chunk.data(), chunk.size()});
        ASSERT_TRUE(flushed.has_value());
        out.append(reinterpret_cast<const char *>(chunk.data()), flushed->written);
        if (flushed->status == EncodeStatus::Flushed) {
            break;
        }
    }
    out += drive_finish(*enc, 4096, false, 1);
    EXPECT_EQ(gunzip_incremental(out, 4096), body);
}

TEST(GzipEncoderTest, AbortPoisonsEveryOperation) {
    BufPool pool;
    ScopedEncoder enc(pool, 6);
    ASSERT_TRUE(static_cast<bool>(enc));
    (void) drive_write_all(*enc, "data", 4096, 4096, false, 1);
    enc->abort();

    std::array<std::uint8_t, 64> buf{};
    const auto w = enc->write({reinterpret_cast<const std::uint8_t *>("x"), 1}, {buf.data(), buf.size()});
    ASSERT_FALSE(w.has_value());
    EXPECT_EQ(w.error(), IoErr::Canceled);
    const auto f = enc->flush({buf.data(), buf.size()});
    ASSERT_FALSE(f.has_value());
    EXPECT_EQ(f.error(), IoErr::Canceled);
    const auto fin = enc->finish({buf.data(), buf.size()});
    ASSERT_FALSE(fin.has_value());
    EXPECT_EQ(fin.error(), IoErr::Canceled);
}

TEST(GzipEncoderTest, NeverFinishedEncoderIsSimplyDropped) {
    // Pool-backed encoder destroyed mid-stream: no finish, no crash; the pool
    // reclaims everything. Under sanitizers this doubles as a lifetime check.
    BufPool pool;
    {
        ScopedEncoder enc(pool, 6);
        ASSERT_TRUE(static_cast<bool>(enc));
        (void) drive_write_all(*enc, json_payload(100000), 4096, 4096, false, 1);
        (void) drive_flush(*enc, 4096, false, 1);
    }
    SUCCEED();
}

TEST(GzipEncoderTest, SteadyStateOpsAllocateNothing) {
    BufPool pool;
    ScopedEncoder enc(pool, 6);
    ASSERT_TRUE(static_cast<bool>(enc));
    std::array<std::uint8_t, 16 * 1024> out_buf{};
    const std::string chunk = json_payload(4096);

    // Warm up every internal path, including a window slide (> 64 KiB total).
    for (int i = 0; i < 40; ++i) {
        const auto step = enc->write({reinterpret_cast<const std::uint8_t *>(chunk.data()), chunk.size()},
                                     {out_buf.data(), out_buf.size()});
        ASSERT_TRUE(step.has_value());
    }
    (void) drive_flush(*enc, out_buf.size(), false, 1);

    g_thread_allocations = 0;
    std::uint64_t written_total = 0;
    for (int i = 0; i < 200; ++i) {
        const auto step = enc->write({reinterpret_cast<const std::uint8_t *>(chunk.data()), chunk.size()},
                                     {out_buf.data(), out_buf.size()});
        ASSERT_TRUE(step.has_value());
        written_total += step->written;
        if ((i % 8) == 7) {
            for (;;) {
                const auto flushed = enc->flush({out_buf.data(), out_buf.size()});
                ASSERT_TRUE(flushed.has_value());
                written_total += flushed->written;
                if (flushed->status == EncodeStatus::Flushed) {
                    break;
                }
            }
        }
    }
    EXPECT_GT(written_total, 0u);
    EXPECT_EQ(g_thread_allocations, 0u) << "steady-state write/flush allocated";
}

// ---- extended acceptance test (run once per migration acceptance) ----

TEST(GzipEncoderExtendedTest, IsizeWrapsModulo2To32AndCrcMatches) {
    // Stream strictly more than 4 GiB through a small fixed buffer while
    // decoding incrementally, so the trailer ISIZE must wrap mod 2^32.
    BufPool pool;
    ScopedEncoder enc(pool, 1);
    ASSERT_TRUE(static_cast<bool>(enc));

    const std::size_t kPeriod = 30000; // within MAX_DIST: long matches, tiny output
    std::string period;
    period.reserve(kPeriod);
    {
        std::mt19937 gen(2024);
        std::uniform_int_distribution<int> dist(0, 255);
        for (std::size_t i = 0; i < kPeriod; ++i) {
            period.push_back(static_cast<char>(dist(gen)));
        }
    }
    const std::string batch = repeat_pattern(300000, period);
    const std::uint64_t target = (1ull << 32) + 65536;

    fiber::util::Crc32 crc;
    std::uint64_t fed = 0;
    std::array<std::uint8_t, 64 * 1024> out_buf{};
    std::array<std::uint8_t, 64 * 1024> decode_buf{};
    std::string expected;
    expected.reserve(decode_buf.size());

    fiber::test::ZlibReferenceInflate inflate(15 + 16);
    auto pump = [&](std::string_view compressed) {
        std::size_t offset = 0;
        while (offset < compressed.size() && !inflate.stream_end()) {
            const fiber::test::ZlibReferenceResult r = inflate.step(compressed.substr(offset), decode_buf.size());
            ASSERT_TRUE(r.ok) << "inflate status " << r.z_status;
            if (!r.output.empty()) {
                const std::uint64_t base = inflate.total_out() - r.output.size();
                expected.clear();
                for (std::size_t i = 0; i < r.output.size(); ++i) {
                    expected.push_back(period[(base + i) % kPeriod]);
                }
                ASSERT_EQ(r.output, expected) << "decoded mismatch at output offset " << base;
            }
            offset += r.consumed;
            if (r.consumed == 0 && r.output.empty() && !r.stream_end) {
                ADD_FAILURE() << "reference inflate stalled";
                return;
            }
        }
    };

    while (fed < target) {
        const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(batch.size(), target - fed));
        std::size_t consumed = 0;
        while (consumed < take) {
            const auto step =
                    enc->write({reinterpret_cast<const std::uint8_t *>(batch.data()) + consumed, take - consumed},
                               {out_buf.data(), out_buf.size()});
            ASSERT_TRUE(step.has_value());
            pump({reinterpret_cast<const char *>(out_buf.data()), step->written});
            consumed += step->consumed;
        }
        crc.update(std::string_view{batch.data(), take});
        fed += take;
    }

    std::string tail;
    for (;;) {
        const auto step = enc->finish({out_buf.data(), out_buf.size()});
        ASSERT_TRUE(step.has_value());
        tail.append(reinterpret_cast<const char *>(out_buf.data()), step->written);
        if (step->status == EncodeStatus::Finished) {
            break;
        }
    }
    pump(tail);

    ASSERT_TRUE(inflate.stream_end());
    ASSERT_EQ(inflate.total_out(), target);

    // Trailer: CRC32 of all input and ISIZE = total mod 2^32.
    ASSERT_GE(tail.size(), 8u);
    const std::string_view trailer{tail.data() + tail.size() - 8, 8};
    const std::uint32_t crc_expected = crc.value();
    const std::uint32_t isize_expected = static_cast<std::uint32_t>(target); // 65536
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(static_cast<std::uint8_t>(trailer[i]), static_cast<std::uint8_t>(crc_expected >> (8 * i)));
        EXPECT_EQ(static_cast<std::uint8_t>(trailer[4 + i]), static_cast<std::uint8_t>(isize_expected >> (8 * i)));
    }
}
