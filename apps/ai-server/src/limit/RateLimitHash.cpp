#include "RateLimitHash.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <string>

namespace fiber::ai_server {
namespace {

constexpr std::uint64_t kC1 = 0x87c37b91114253d5ULL;
constexpr std::uint64_t kC2 = 0x4cf5ad432745937fULL;

std::uint64_t little_endian(const std::uint8_t *data) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(data[i]) << (i * 8);
    }
    return value;
}

std::uint64_t fmix(std::uint64_t value) noexcept {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33;
    return value;
}

} // namespace

std::uint64_t rate_limit_hash64(std::string_view value) noexcept {
    const auto *data = reinterpret_cast<const std::uint8_t *>(value.data());
    std::uint64_t h1 = 0;
    std::uint64_t h2 = 0;
    const std::size_t block_count = value.size() >> 4;
    for (std::size_t i = 0; i < block_count; ++i) {
        const std::uint8_t *block = data + (i << 4);
        std::uint64_t k1 = little_endian(block);
        std::uint64_t k2 = little_endian(block + 8);

        k1 *= kC1;
        k1 = std::rotl(k1, 31);
        k1 *= kC2;
        h1 ^= k1;
        h1 = std::rotl(h1, 27);
        h1 += h2;
        h1 = h1 * 5 + 0x52dce729ULL;

        k2 *= kC2;
        k2 = std::rotl(k2, 33);
        k2 *= kC1;
        h2 ^= k2;
        h2 = std::rotl(h2, 31);
        h2 += h1;
        h2 = h2 * 5 + 0x38495ab5ULL;
    }

    std::uint64_t k1 = 0;
    std::uint64_t k2 = 0;
    const std::uint8_t *tail = data + (block_count << 4);
    switch (value.size() & 15U) {
        case 15:
            k2 ^= static_cast<std::uint64_t>(tail[14]) << 48;
            [[fallthrough]];
        case 14:
            k2 ^= static_cast<std::uint64_t>(tail[13]) << 40;
            [[fallthrough]];
        case 13:
            k2 ^= static_cast<std::uint64_t>(tail[12]) << 32;
            [[fallthrough]];
        case 12:
            k2 ^= static_cast<std::uint64_t>(tail[11]) << 24;
            [[fallthrough]];
        case 11:
            k2 ^= static_cast<std::uint64_t>(tail[10]) << 16;
            [[fallthrough]];
        case 10:
            k2 ^= static_cast<std::uint64_t>(tail[9]) << 8;
            [[fallthrough]];
        case 9:
            k2 ^= static_cast<std::uint64_t>(tail[8]);
            k2 *= kC2;
            k2 = std::rotl(k2, 33);
            k2 *= kC1;
            h2 ^= k2;
            [[fallthrough]];
        case 8:
            k1 ^= static_cast<std::uint64_t>(tail[7]) << 56;
            [[fallthrough]];
        case 7:
            k1 ^= static_cast<std::uint64_t>(tail[6]) << 48;
            [[fallthrough]];
        case 6:
            k1 ^= static_cast<std::uint64_t>(tail[5]) << 40;
            [[fallthrough]];
        case 5:
            k1 ^= static_cast<std::uint64_t>(tail[4]) << 32;
            [[fallthrough]];
        case 4:
            k1 ^= static_cast<std::uint64_t>(tail[3]) << 24;
            [[fallthrough]];
        case 3:
            k1 ^= static_cast<std::uint64_t>(tail[2]) << 16;
            [[fallthrough]];
        case 2:
            k1 ^= static_cast<std::uint64_t>(tail[1]) << 8;
            [[fallthrough]];
        case 1:
            k1 ^= static_cast<std::uint64_t>(tail[0]);
            k1 *= kC1;
            k1 = std::rotl(k1, 31);
            k1 *= kC2;
            h1 ^= k1;
            [[fallthrough]];
        default:
            break;
    }

    h1 ^= value.size();
    h2 ^= value.size();
    h1 += h2;
    h2 += h1;
    h1 = fmix(h1);
    h2 = fmix(h2);
    h1 += h2;
    return h1;
}

std::uint64_t rate_limit_key_hash64(std::string_view user_id, std::string_view model_name) noexcept {
    constexpr std::size_t kStackCapacity = 256;
    const std::size_t size = user_id.size() + 1 + model_name.size();
    if (size <= kStackCapacity) {
        std::array<char, kStackCapacity> key{};
        std::memcpy(key.data(), user_id.data(), user_id.size());
        key[user_id.size()] = '\0';
        std::memcpy(key.data() + user_id.size() + 1, model_name.data(), model_name.size());
        return rate_limit_hash64(std::string_view(key.data(), size));
    }
    std::string key;
    key.reserve(size);
    key.append(user_id);
    key.push_back('\0');
    key.append(model_name);
    return rate_limit_hash64(key);
}

} // namespace fiber::ai_server
