#ifndef FIBER_SCRIPT_GC_WTF8_H
#define FIBER_SCRIPT_GC_WTF8_H

#include <cstddef>
#include <cstdint>

namespace fiber::script::gc_detail {

// Canonical WTF-8 uses normal four-byte UTF-8 for paired surrogates and
// three-byte surrogate encodings only for isolated UTF-16 code units.
struct Wtf8MeasureResult {
    std::size_t byte_len = 0;
    bool well_formed = true;
};

struct Wtf8Cursor {
    const char *data = nullptr;
    std::size_t len = 0;
    std::size_t pos = 0;
    char16_t pending = 0;
    bool has_pending = false;
    bool malformed = false;
};

struct Wtf8SlicePlan {
    std::size_t copy_begin = 0;
    std::size_t copy_end = 0;
    bool leading_low_surrogate = false;
    bool trailing_high_surrogate = false;
    char16_t leading_unit = 0;
    char16_t trailing_unit = 0;
};

bool wtf8_measure_utf16(const char16_t *data, std::size_t len, Wtf8MeasureResult &out) noexcept;
bool wtf8_write_utf16(const char16_t *data, std::size_t len, char *dst, std::size_t dst_len) noexcept;

bool wtf8_measure_latin1(const std::uint8_t *data, std::size_t len, std::size_t &byte_len) noexcept;
bool wtf8_write_latin1(const std::uint8_t *data, std::size_t len, char *dst, std::size_t dst_len) noexcept;

bool wtf8_next_utf16_unit(Wtf8Cursor &cursor, char16_t &unit) noexcept;
bool wtf8_is_well_formed(const char *data, std::size_t len) noexcept;
bool wtf8_starts_with_low_surrogate(const char *data, std::size_t len, char16_t &unit) noexcept;
bool wtf8_ends_with_high_surrogate(const char *data, std::size_t len, char16_t &unit) noexcept;
bool wtf8_plan_utf16_slice(const char *data, std::size_t byte_len, std::size_t utf16_len, std::size_t begin,
                           std::size_t end, Wtf8SlicePlan &out) noexcept;

std::uint8_t wtf8_write_codepoint(std::uint32_t codepoint, char *out) noexcept;

} // namespace fiber::script::gc_detail

#endif // FIBER_SCRIPT_GC_WTF8_H
