#ifndef FIBER_GRPC_GRPC_FRAMING_H
#define FIBER_GRPC_GRPC_FRAMING_H

#include <cstddef>

#include "../common/IoError.h"
#include "../common/mem/IoBufChain.h"

namespace fiber::grpc {

// gRPC length-prefixed message frame over HTTP/2:
//   [1 byte compressed-flag][4 byte big-endian length][payload]
// compressed-flag 0 = identity (the only mode supported here); 1 is rejected.

// Prepend a 5-byte frame header to `payload` (in place). `payload` must be a
// bound IoBufChain (e.g. from ProtoCodec::encode). Returns the framed chain
// (header node + payload nodes) ready for ClientHttp2Exchange::write_body.
common::IoResult<mem::IoBufChain> frame(mem::IoBufChain payload) noexcept;

// Streaming deframer for the receive side: feed body chunks from
// ClientHttp2Exchange::read_body and extract complete message payloads.
//
// The reader accumulates appended chunks as a single IoBufChain (no byte
// copies) and extracts each frame's payload via IoBufChain::take_prefix, so the
// payload bytes stay in the original IoBuf storage all the way to the caller.
class GrpcFrameReader {
public:
    GrpcFrameReader() noexcept = default;

    // Append a body chunk. Moves the chunk's nodes into the internal buffer
    // (zero-copy). The first chunk fixes the node-pool binding; every chunk
    // must share that pool (as the per-request read_body chunks do).
    common::IoResult<void> append(mem::IoBufChain chunk) noexcept;

    // Extract the next complete frame's payload into `out`. `out` must be an
    // empty, unbound chain (it is bound to the reader's pool here). On success
    // returns true and `out` holds exactly the payload bytes (zero-copy). On
    // false, more bytes are needed. Errors (e.g. unsupported compression) are
    // returned as unexpected.
    common::IoResult<bool> next_payload(mem::IoBufChain &out) noexcept;

    [[nodiscard]] std::size_t buffered_bytes() const noexcept;

private:
    mem::IoBufChain buffer_;
};

} // namespace fiber::grpc

#endif // FIBER_GRPC_GRPC_FRAMING_H
