#ifndef FIBER_TESTS_SUPPORT_ZLIB_REFERENCE_H
#define FIBER_TESTS_SUPPORT_ZLIB_REFERENCE_H

// Independent zlib 1.3.2 reference codec for tests only.
//
// Backed by the prefixed C objects under tests/support/third_party/zlib_1_3_2
// (see that directory's README). Used to validate the in-tree gzip encoder and
// the HTTP compression writer against the upstream implementation. This header
// is test support code: it must not be included by, or linked into, any
// production target.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace fiber::test {

struct ZlibReferenceResult {
    // true when the operation completed without a zlib error. An empty
    // `output` with ok=true is a legitimate empty payload; callers must not
    // use an empty string as the failure signal.
    bool ok = false;
    // Input bytes consumed by the operation.
    std::size_t consumed = 0;
    // Bytes produced by the operation.
    std::string output;
    // inflate only: true when the decoder reached the end of the stream
    // (gzip trailer validated for gzip members).
    bool stream_end = false;
    // Raw zlib status of the final call, for diagnostics.
    int z_status = 0;
};

// Decode one complete gzip member (windowBits = 15 + 16). Fails unless the
// stream ends cleanly with a valid trailer and, unless allow_trailing is set,
// no unconsumed input remains after the member.
ZlibReferenceResult zlib_reference_gunzip(std::string_view input, bool allow_trailing = false,
                                          std::size_t out_chunk = 4096);

// Compress `input` into a single gzip member with the upstream deflate
// implementation (windowBits = 15 + 16, memLevel = 8, default strategy).
ZlibReferenceResult zlib_reference_gzip(std::string_view input, int level);

// One-shot DEFLATE with a caller-selected raw windowBits (negative for raw
// streams, e.g. -15) using memLevel 8 and the default strategy.
ZlibReferenceResult zlib_reference_deflate(std::string_view input, int level, int window_bits);

// Reference CRC-32 (poly 0xEDB88320) of `data`; `crc` is the previously
// returned (finalized) value, 0 for a fresh computation.
std::uint32_t zlib_reference_crc32(std::uint32_t crc, std::string_view data);

// Incremental gzip-member decoder. Each step() consumes as much input as
// possible while producing at most `out_capacity` bytes, mirroring how a
// streaming consumer would drive a decoder across chunked output.
class ZlibReferenceInflate {
public:
    explicit ZlibReferenceInflate(int window_bits = 15 + 16);
    ~ZlibReferenceInflate();

    ZlibReferenceInflate(const ZlibReferenceInflate &) = delete;
    ZlibReferenceInflate &operator=(const ZlibReferenceInflate &) = delete;

    // Returns ok=false on the first zlib error; later steps keep returning
    // ok=false without touching the stream.
    ZlibReferenceResult step(std::string_view input, std::size_t out_capacity);

    [[nodiscard]] std::uint64_t total_in() const noexcept;
    [[nodiscard]] std::uint64_t total_out() const noexcept;
    [[nodiscard]] bool stream_end() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] int last_status() const noexcept;

private:
    struct Stream; // z_stream plus init state
    Stream *stream_ = nullptr;
    bool stream_end_ = false;
    bool failed_ = false;
    int last_status_ = 0;
};

// Flush mode for ZlibReferenceDeflate::step(); mirrors the upstream deflate
// flush levels this project cares about.
enum class ZlibReferenceFlush : std::uint8_t {
    None,
    Sync,
    Finish,
};

// Incremental upstream deflate encoder. Each step() consumes as much input as
// possible while producing at most `out_capacity` bytes, mirroring the
// streaming operation sequences used against the in-tree encoders.
class ZlibReferenceDeflate {
public:
    explicit ZlibReferenceDeflate(int level, int window_bits = 15 + 16);
    ~ZlibReferenceDeflate();

    ZlibReferenceDeflate(const ZlibReferenceDeflate &) = delete;
    ZlibReferenceDeflate &operator=(const ZlibReferenceDeflate &) = delete;

    ZlibReferenceResult step(std::string_view input, std::size_t out_capacity, ZlibReferenceFlush flush);

    [[nodiscard]] std::uint64_t total_in() const noexcept;
    [[nodiscard]] std::uint64_t total_out() const noexcept;
    [[nodiscard]] bool stream_end() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] int last_status() const noexcept;

private:
    struct Stream; // z_stream plus init state
    Stream *stream_ = nullptr;
    bool stream_end_ = false;
    bool failed_ = false;
    int last_status_ = 0;
};

} // namespace fiber::test

#endif // FIBER_TESTS_SUPPORT_ZLIB_REFERENCE_H
