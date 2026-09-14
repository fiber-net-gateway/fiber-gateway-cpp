// Streaming gzip encoder (RFC 1952) over the private DEFLATE core. The gzip
// framing follows zlib 1.3.2 deflate.c's GZIP wrapper logic (Jean-loup Gailly;
// see src/compression/UPSTREAM.md): exactly one member, fixed 10-byte header,
// XFL from the level, OS from the build platform, little-endian CRC32+ISIZE
// trailer. The CRC and length are computed here over exactly the bytes the
// core reports as consumed, mirroring where upstream updates them in
// read_buf(). Header and trailer are emitted through fixed buffers and
// cursors so arbitrary (including 1-byte) output slicing works.

#include <cstring>
#include <expected>
#include <new>

#include <fiber/common/Assert.h>
#include <fiber/common/util/Crc32.h>

#include "DeflateEncoder.h"
#include "DeflateState.h"

namespace fiber::compression {

namespace {

// OS byte of the gzip header (zlib OS_CODE on the platforms this project
// builds for; RFC 1952: 3 = Unix, 19 = Apple).
#if defined(__APPLE__)
inline constexpr std::uint8_t kGzipOsCode = 19;
#else
inline constexpr std::uint8_t kGzipOsCode = 3;
#endif

inline constexpr std::size_t kGzipHeaderSize = 10;
inline constexpr std::size_t kGzipTrailerSize = 8;

// XFL byte: 2 for level 9, 4 for level 1, 0 otherwise (zlib gzip header).
std::uint8_t gzip_xfl(int level) noexcept {
    if (level == 9) {
        return 2;
    }
    if (level == 1) {
        return 4;
    }
    return 0;
}

} // namespace

struct GzipEncoder::State {
    enum class Phase : std::uint8_t { Active, Flushing, Finishing, Finished, Aborted };

    DeflateState deflate; // core state; workspace is bound to it
    util::Crc32 crc; // CRC32 of all consumed input
    std::uint64_t total_in = 0; // total consumed input (wide; ISIZE = low 32 bits)
    std::uint8_t header[kGzipHeaderSize] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, kGzipOsCode};
    std::uint8_t trailer[kGzipTrailerSize] = {};
    std::size_t header_pos = 0; // bytes of the header already emitted
    std::size_t trailer_pos = 0; // bytes of the trailer already emitted
    Phase phase = Phase::Active;
};

namespace {

// Copy bytes of a fixed stage buffer (header/trailer) into the remaining
// output; advances the output cursor and the stage cursor.
std::size_t emit_fixed(const std::uint8_t *from, std::size_t total, std::size_t &pos, std::uint8_t *&out,
                       std::size_t &room) noexcept {
    const std::size_t n = room < total - pos ? room : total - pos;
    if (n != 0) {
        std::memcpy(out, from + pos, n);
        out += n;
        room -= n;
        pos += n;
    }
    return n;
}

} // namespace

common::IoResult<GzipEncoder *> GzipEncoder::create(mem::BufPool &pool, GzipEncoderOptions options) noexcept {
    if (options.compression_level < 1 || options.compression_level > 9) {
        return std::unexpected(common::IoErr::Invalid);
    }

    // One allocation: [State, padded | DEFLATE workspace | GzipEncoder].
    constexpr std::size_t kStateBytes = (sizeof(State) + 15) & ~std::size_t{15};
    void *mem = pool.alloc(kStateBytes + kDeflateWorkspaceSize + sizeof(GzipEncoder));
    if (mem == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }

    auto *state = new (mem) State;
    state->header[8] = gzip_xfl(options.compression_level);
    DeflateEncoder::init(state->deflate, static_cast<std::uint8_t *>(mem) + kStateBytes, options.compression_level);

    auto *encoder = new (static_cast<std::uint8_t *>(mem) + kStateBytes + kDeflateWorkspaceSize) GzipEncoder(*state);
    return encoder;
}

GzipEncoder::GzipEncoder(State &state) noexcept : state_(&state) {}

// The state is trivially destructible and the memory belongs to the pool; the
// destructor is intentionally a no-op.
GzipEncoder::~GzipEncoder() noexcept = default;

common::IoResult<EncodeStep> GzipEncoder::write(std::span<const std::uint8_t> input,
                                                std::span<std::uint8_t> output) noexcept {
    State &s = *state_;
    switch (s.phase) {
        case State::Phase::Active:
            break;
        case State::Phase::Flushing:
            return std::unexpected(common::IoErr::Busy);
        case State::Phase::Finishing:
        case State::Phase::Finished:
            return std::unexpected(common::IoErr::Already);
        case State::Phase::Aborted:
            return std::unexpected(common::IoErr::Canceled);
    }

    std::uint8_t *out = output.data();
    std::size_t room = output.size();
    std::size_t written = 0;

    // The header starts with the first producing operation; an empty write
    // never starts the stream but does continue a partially sent header.
    if (s.header_pos != 0 || !input.empty()) {
        written += emit_fixed(s.header, kGzipHeaderSize, s.header_pos, out, room);
        if (s.header_pos < kGzipHeaderSize) {
            return EncodeStep{0, written, EncodeStatus::NeedOutput};
        }
    }

    const EncodeStep core = DeflateEncoder::write(s.deflate, input, std::span{out, room});
    s.crc.update(std::span<const std::uint8_t>{input.data(), core.consumed});
    s.total_in += core.consumed;

    return EncodeStep{core.consumed, written + core.written, core.status};
}

common::IoResult<EncodeStep> GzipEncoder::flush(std::span<std::uint8_t> output) noexcept {
    State &s = *state_;
    switch (s.phase) {
        case State::Phase::Active:
        case State::Phase::Flushing:
            break;
        case State::Phase::Finishing:
        case State::Phase::Finished:
            return std::unexpected(common::IoErr::Already);
        case State::Phase::Aborted:
            return std::unexpected(common::IoErr::Canceled);
    }

    // A flush before any input was accepted produces nothing at all: no
    // header, no marker (the HTTP layer relies on this to keep tiny responses
    // uncompressed-looking until data actually flows). A partially sent
    // header still needs to complete.
    if (s.total_in == 0) {
        if (s.header_pos == 0) {
            return EncodeStep{0, 0, EncodeStatus::Flushed};
        }
        std::uint8_t *out = output.data();
        std::size_t room = output.size();
        const std::size_t written = emit_fixed(s.header, kGzipHeaderSize, s.header_pos, out, room);
        if (s.header_pos < kGzipHeaderSize) {
            return EncodeStep{0, written, EncodeStatus::NeedOutput};
        }
        return EncodeStep{0, written, EncodeStatus::Flushed};
    }

    s.phase = State::Phase::Flushing;

    std::uint8_t *out = output.data();
    std::size_t room = output.size();
    std::size_t written = 0;

    written += emit_fixed(s.header, kGzipHeaderSize, s.header_pos, out, room);
    if (s.header_pos < kGzipHeaderSize) {
        return EncodeStep{0, written, EncodeStatus::NeedOutput};
    }

    const EncodeStep core = DeflateEncoder::flush(s.deflate, std::span{out, room});
    written += core.written;
    if (core.status == EncodeStatus::Flushed) {
        s.phase = State::Phase::Active;
        return EncodeStep{0, written, EncodeStatus::Flushed};
    }
    return EncodeStep{0, written, EncodeStatus::NeedOutput};
}

common::IoResult<EncodeStep> GzipEncoder::finish(std::span<std::uint8_t> output) noexcept {
    State &s = *state_;
    switch (s.phase) {
        case State::Phase::Active:
            s.phase = State::Phase::Finishing;
            break;
        case State::Phase::Flushing:
            return std::unexpected(common::IoErr::Busy);
        case State::Phase::Finishing:
            break;
        case State::Phase::Finished:
            return EncodeStep{0, 0, EncodeStatus::Finished};
        case State::Phase::Aborted:
            return std::unexpected(common::IoErr::Canceled);
    }

    std::uint8_t *out = output.data();
    std::size_t room = output.size();
    std::size_t written = 0;

    // finish produces the full member even for an empty stream: header, final
    // (empty) block, trailer.
    written += emit_fixed(s.header, kGzipHeaderSize, s.header_pos, out, room);
    if (s.header_pos < kGzipHeaderSize) {
        return EncodeStep{0, written, EncodeStatus::NeedOutput};
    }

    // Drive the core to the end of the raw stream. On the first completion,
    // capture the trailer before any bytes are emitted so later calls only
    // drain the buffer.
    const EncodeStep core = DeflateEncoder::finish(s.deflate, std::span{out, room});
    written += core.written;
    // The core advanced its own cursor over the caller's span; mirror it here
    // so the trailer lands after the final block instead of on top of it.
    out += core.written;
    room -= core.written;
    if (core.status != EncodeStatus::Finished) {
        return EncodeStep{0, written, EncodeStatus::NeedOutput};
    }
    if (s.trailer_pos == 0) {
        const std::uint32_t crc = s.crc.value();
        const std::uint32_t isize = static_cast<std::uint32_t>(s.total_in);
        for (int i = 0; i < 4; ++i) {
            s.trailer[i] = static_cast<std::uint8_t>(crc >> (8 * i));
            s.trailer[4 + i] = static_cast<std::uint8_t>(isize >> (8 * i));
        }
    }

    written += emit_fixed(s.trailer, kGzipTrailerSize, s.trailer_pos, out, room);
    if (s.trailer_pos < kGzipTrailerSize) {
        return EncodeStep{0, written, EncodeStatus::NeedOutput};
    }

    s.phase = State::Phase::Finished;
    return EncodeStep{0, written, EncodeStatus::Finished};
}

void GzipEncoder::abort() noexcept { state_->phase = State::Phase::Aborted; }

} // namespace fiber::compression
