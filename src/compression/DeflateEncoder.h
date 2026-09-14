#ifndef FIBER_COMPRESSION_DEFLATE_ENCODER_H
#define FIBER_COMPRESSION_DEFLATE_ENCODER_H

// Private resumable bare-DEFLATE encoder interface, derived from zlib 1.3.2
// deflate.c (Jean-loup Gailly; see src/compression/UPSTREAM.md). This layer
// knows nothing about gzip framing; GzipEncoder drives it and owns the CRC
// and length accounting.
//
// Resumability model: every operation reports consumed/written over the
// caller's spans plus an EncodeStatus, mirroring how the upstream deflate()
// loop bails out through avail_in/avail_out. The caller may pass zero-length
// output spans; operations then still consume input as far as the internal
// buffers allow and return NeedOutput.

#include <cstddef>
#include <cstdint>
#include <span>

#include <fiber/compression/GzipEncoder.h>

#include "DeflateState.h"

namespace fiber::compression {

// Workspace bytes bound to one encoder, in addition to sizeof(DeflateState):
// [window 64K | prev 64K | head 64K | pending 64K]. The block must be
// max_align_t-aligned and must not move while the state is live.
inline constexpr std::size_t kDeflateWindowBytes = static_cast<std::size_t>(kDeflateWindowSize);
inline constexpr std::size_t kDeflatePrevBytes = static_cast<std::size_t>(kDeflateWSize) * sizeof(DeflatePos);
inline constexpr std::size_t kDeflateHeadBytes = static_cast<std::size_t>(kDeflateHashSize) * sizeof(DeflatePos);
inline constexpr std::size_t kDeflatePendingBytes = static_cast<std::size_t>(kDeflatePendingBufSize);
inline constexpr std::size_t kDeflateWorkspaceSize =
        kDeflateWindowBytes + kDeflatePrevBytes + kDeflateHeadBytes + kDeflatePendingBytes;

class DeflateEncoder final {
public:
    DeflateEncoder() = delete;

    // Bind `workspace` (>= kDeflateWorkspaceSize bytes) and reset `state` to
    // a fresh stream at `level` (1..9, caller-validated).
    static void init(DeflateState &state, void *workspace, int level) noexcept;

    // Append input. Empty input never enters the compression driver: it only
    // drains pending output (an empty write must not be observable in the
    // stream state). Must not be called after the stream finished.
    static EncodeStep write(DeflateState &state, std::span<const std::uint8_t> input,
                            std::span<std::uint8_t> output) noexcept;

    // Emit an sync marker (empty stored block) once per data burst. Must not
    // be called after the stream finished.
    static EncodeStep flush(DeflateState &state, std::span<std::uint8_t> output) noexcept;

    // Emit the final block and end the raw stream. Idempotent: after the
    // first Finished, later calls return Finished with written == 0.
    static EncodeStep finish(DeflateState &state, std::span<std::uint8_t> output) noexcept;
};

} // namespace fiber::compression

#endif // FIBER_COMPRESSION_DEFLATE_ENCODER_H
