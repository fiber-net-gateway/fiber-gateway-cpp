#ifndef FIBER_COMPRESSION_GZIP_ENCODER_H
#define FIBER_COMPRESSION_GZIP_ENCODER_H

// Streaming gzip (RFC 1952) encoder over a bare-DEFLATE core ported from
// zlib 1.3.2 (Jean-loup Gailly; see src/compression/UPSTREAM.md). The encoder
// is incremental: every operation consumes at most as much input and fills at
// most as much output as the caller provides, and reports its progress so the
// caller can resume. All state lives in one memory-pool allocation.
//
// Framing: exactly one gzip member per encoder (fixed header, no name/extra/
// comment), XFL set from the compression level (2 for level 9, 4 for level 1,
// 0 otherwise), OS byte 3 (Unix) or 19 (Apple), trailer = CRC32 then ISIZE,
// both little-endian, ISIZE = input length mod 2^32. Levels 1..9.

#include <cstddef>
#include <cstdint>
#include <span>

#include <fiber/common/IoError.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/common/mem/BufPool.h>

namespace fiber::compression {

struct GzipEncoderOptions {
    int compression_level = 1; // 1..9
};

// What an encoder operation wants from the caller next.
enum class EncodeStatus : std::uint8_t {
    NeedInput, // write() only: all submitted input was accepted; send more
    NeedOutput, // output span is full; resume with fresh output space
    Flushed, // flush() completed; all buffered output has been produced
    Finished, // finish() completed; the gzip member is fully emitted
};

// Progress of one encoder operation over the caller's spans.
struct EncodeStep {
    std::size_t consumed = 0; // input bytes taken (write() only)
    std::size_t written = 0; // output bytes produced
    EncodeStatus status = EncodeStatus::NeedInput;
};

class GzipEncoder final : public common::NonCopyable, public common::NonMovable {
public:
    // Create an encoder backed by `pool`. The single allocation (state plus
    // DEFLATE workspace) is owned by the encoder and released in the
    // destructor; the pool must outlive it. `compression_level` outside 1..9
    // returns Invalid.
    static common::IoResult<GzipEncoder *> create(mem::BufPool &pool, GzipEncoderOptions options) noexcept;

    // Return the encoder's memory to the pool. Pool allocations do not run
    // destructors, so this is what actually destroys the internal state.
    ~GzipEncoder() noexcept;

    // Append input. The gzip header is emitted with the first producing
    // operation. Any input not accepted in one call (output span full) is
    // neither consumed nor retained: the caller resubmits the remainder.
    // Returns Busy while a flush() is in progress, Already after finish(),
    // Canceled after abort().
    common::IoResult<EncodeStep> write(std::span<const std::uint8_t> input, std::span<std::uint8_t> output) noexcept;

    // End the current DEFLATE block and emit the sync marker (an empty
    // stored block) so the stream can be decoded up to this point. Repeated
    // calls without intervening writes emit nothing. Returns Already after
    // finish(), Canceled after abort().
    common::IoResult<EncodeStep> flush(std::span<std::uint8_t> output) noexcept;

    // Emit the final block, gzip trailer, and finish the member. Repeat with
    // fresh output space until status == Finished; idempotent afterwards
    // (returns Finished with written == 0). Returns Busy while a flush() is
    // still draining, Canceled after abort().
    common::IoResult<EncodeStep> finish(std::span<std::uint8_t> output) noexcept;

    // Poison the encoder: every subsequent operation returns Invalid. The
    // memory is still released by the destructor.
    void abort() noexcept;

private:
    struct State;
    explicit GzipEncoder(State &state) noexcept;

    State *state_;
};

} // namespace fiber::compression

#endif // FIBER_COMPRESSION_GZIP_ENCODER_H
