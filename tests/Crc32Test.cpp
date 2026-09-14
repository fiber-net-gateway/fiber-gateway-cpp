#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <fiber/common/util/Crc32.h>

#include "support/ZlibReference.h"

namespace {

using fiber::util::Crc32;

std::uint32_t reference_crc32(std::string_view data) { return fiber::test::zlib_reference_crc32(0, data); }

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

TEST(Crc32Test, KnownCheckValue) {
    EXPECT_EQ(Crc32::compute(std::string_view("123456789")), 0xCBF43926u);
    const std::uint8_t digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    EXPECT_EQ(Crc32::compute(std::span<const std::uint8_t>{digits}), 0xCBF43926u);
}

TEST(Crc32Test, EmptyInputIsZero) {
    EXPECT_EQ(Crc32::compute(std::string_view("")), 0u);
    EXPECT_EQ(Crc32::compute(std::span<const std::uint8_t>{}), 0u);

    Crc32 crc;
    EXPECT_EQ(crc.value(), 0u);
}

TEST(Crc32Test, SingleNullByte) {
    const std::string zero("\0", 1);
    EXPECT_EQ(Crc32::compute(zero), 0xD202EF8Du);
    EXPECT_EQ(Crc32::compute(zero), reference_crc32(zero));
}

TEST(Crc32Test, EverySingleByteValueMatchesReference) {
    for (int i = 0; i < 256; ++i) {
        const std::string one(1, static_cast<char>(i));
        ASSERT_EQ(Crc32::compute(one), reference_crc32(one)) << "byte " << i;
    }
}

TEST(Crc32Test, AllByteValueSequenceMatchesReference) {
    std::string all;
    for (int repeat = 0; repeat < 8; ++repeat) {
        for (int i = 0; i < 256; ++i) {
            all.push_back(static_cast<char>(i));
        }
    }
    EXPECT_EQ(Crc32::compute(all), reference_crc32(all));
}

TEST(Crc32Test, IncrementalEqualsOneShotAcrossBraidBoundaries) {
    // The braided path needs N*W + W - 1 = 47 bytes on 64-bit targets; cover
    // every length around it plus a set of larger sizes.
    std::vector<std::size_t> sizes;
    for (std::size_t n = 0; n <= 64; ++n) {
        sizes.push_back(n);
    }
    for (std::size_t n: {100u, 1000u, 4096u, 65536u, 100003u}) {
        sizes.push_back(n);
    }
    const std::vector<std::size_t> chunks = {1, 2, 3, 5, 7, 8, 13, 64, 1000, 33333};

    std::uint32_t seed = 1;
    for (std::size_t size: sizes) {
        const std::string data = random_bytes(size, seed++);
        const std::uint32_t one_shot = Crc32::compute(data);
        ASSERT_EQ(one_shot, reference_crc32(data)) << "size " << size;
        for (std::size_t chunk: chunks) {
            if (chunk > size && size != 0) {
                continue;
            }
            Crc32 crc;
            std::size_t offset = 0;
            while (offset < data.size()) {
                const std::size_t take = std::min(chunk, data.size() - offset);
                crc.update(std::string_view{data.data() + offset, take});
                offset += take;
            }
            ASSERT_EQ(crc.value(), one_shot) << "size " << size << " chunk " << chunk;
        }
    }
}

TEST(Crc32Test, IncrementalRandomSlicingMatchesOneShot) {
    const std::string data = random_bytes(100003, 42);
    const std::uint32_t one_shot = Crc32::compute(data);
    std::mt19937 gen(7);
    for (int trial = 0; trial < 25; ++trial) {
        Crc32 crc;
        std::size_t offset = 0;
        while (offset < data.size()) {
            const std::size_t take = std::uniform_int_distribution<std::size_t>(1, 512)(gen);
            const std::size_t n = std::min(take, data.size() - offset);
            crc.update(std::string_view{data.data() + offset, n});
            offset += n;
        }
        ASSERT_EQ(crc.value(), one_shot) << "trial " << trial;
    }
}

TEST(Crc32Test, UnalignedStartsEnterBraidedPathCorrectly) {
    // Spans that begin mid-word force the byte-wise head alignment inside the
    // braided update before full words are processed.
    const std::string data = random_bytes(512, 99);
    for (std::size_t start = 0; start < 16; ++start) {
        for (std::size_t len = 30; len <= 80; ++len) {
            if (start + len > data.size()) {
                break;
            }
            const std::string_view view{data.data() + start, len};
            ASSERT_EQ(Crc32::compute(view), reference_crc32(view)) << "start " << start << " len " << len;
        }
    }
}

TEST(Crc32Test, ResetRestartsComputation) {
    Crc32 crc;
    crc.update("123456789");
    const std::uint32_t first = crc.value();
    EXPECT_EQ(first, 0xCBF43926u);

    crc.update("x");
    EXPECT_NE(crc.value(), first);

    crc.reset();
    EXPECT_EQ(crc.value(), 0u);
    crc.update("123456789");
    EXPECT_EQ(crc.value(), first);
}

TEST(Crc32Test, ReferenceCrc32IsChainedLikeUpstream) {
    // The reference wrapper must accept a previously finalized value exactly
    // like upstream crc32(crc, buf, len).
    const std::string first = random_bytes(1000, 5);
    const std::string second = random_bytes(2000, 6);
    const std::uint32_t chained =
            fiber::test::zlib_reference_crc32(fiber::test::zlib_reference_crc32(0, first), second);
    const std::uint32_t joined = reference_crc32(first + second);
    EXPECT_EQ(chained, joined);
}

} // namespace
