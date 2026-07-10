#ifndef FIBER_GRPC_PROTO_CODEC_H
#define FIBER_GRPC_PROTO_CODEC_H

#include <string_view>

#include <google/protobuf/message_lite.h>

#include "../common/IoError.h"
#include "../common/mem/IoBufChain.h"

namespace fiber::grpc {

// Serialize a protobuf lite message into a single-node IoBufChain ready to be
// handed to ClientHttp2Exchange::write_body. Sized via ByteSizeLong() then
// SerializeToArray directly into the IoBuf writable region - no std::string.
// An empty message yields a zero-byte chain.
common::IoResult<mem::IoBufChain> encode(mem::IoBufNodePool &node_pool,
                                         const google::protobuf::MessageLite &msg) noexcept;

// Parse a contiguous byte range into `out`.
common::IoResult<void> decode(std::string_view bytes, google::protobuf::MessageLite &out) noexcept;

// Parse an IoBufChain (e.g. from ClientHttp2Exchange::read_body) into `out`.
// Single-segment chains are parsed in place (zero-copy); multi-segment chains
// are coalesced into a temporary buffer first.
common::IoResult<void> decode(const mem::IoBufChain &chain, google::protobuf::MessageLite &out) noexcept;

} // namespace fiber::grpc

#endif // FIBER_GRPC_PROTO_CODEC_H
