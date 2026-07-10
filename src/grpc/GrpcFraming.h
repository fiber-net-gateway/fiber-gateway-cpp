#ifndef FIBER_GRPC_GRPC_FRAMING_H
#define FIBER_GRPC_GRPC_FRAMING_H

#include <cstddef>
#include <string>
#include <string_view>

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
class GrpcFrameReader {
public:
    GrpcFrameReader() noexcept = default;

    common::IoResult<void> append(std::string_view bytes) noexcept;
    common::IoResult<void> append(const mem::IoBufChain &chain) noexcept;

    // Extract the next complete frame's payload into `out`.
    // Returns true if a frame was extracted, false if more bytes are needed.
    common::IoResult<bool> next_payload(std::string &out) noexcept;

    [[nodiscard]] std::size_t buffered_bytes() const noexcept;

private:
    std::string buffer_;
};

} // namespace fiber::grpc

#endif // FIBER_GRPC_GRPC_FRAMING_H
