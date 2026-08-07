#ifndef FIBER_HTTP_HTTP3_PROTOCOL_H
#define FIBER_HTTP_HTTP3_PROTOCOL_H

#include <cstdint>

namespace fiber::http {

enum class Http3ErrorCode : std::uint64_t {
    NoError = 0x100,
    GeneralProtocolError = 0x101,
    InternalError = 0x102,
    StreamCreationError = 0x103,
    ClosedCriticalStream = 0x104,
    FrameUnexpected = 0x105,
    FrameError = 0x106,
    ExcessiveLoad = 0x107,
    IdError = 0x108,
    SettingsError = 0x109,
    MissingSettings = 0x10A,
    RequestRejected = 0x10B,
    RequestCancelled = 0x10C,
    RequestIncomplete = 0x10D,
    MessageError = 0x10E,
    ConnectError = 0x10F,
    VersionFallback = 0x110,
    QpackDecompressionFailed = 0x200,
    QpackEncoderStreamError = 0x201,
    QpackDecoderStreamError = 0x202,
};

struct Http3Settings {
    std::uint64_t qpack_max_table_capacity = 0;
    std::uint64_t qpack_blocked_streams = 0;
    std::uint64_t max_field_section_size = 0;
    bool enable_connect_protocol = false;
};

enum class Http3StreamKind : std::uint8_t {
    Unclassified,
    Request,
    Control,
    Push,
    QpackEncoder,
    QpackDecoder,
    UnknownUni,
};

enum class Http3StreamType : std::uint64_t {
    Control = 0x00,
    Push = 0x01,
    QpackEncoder = 0x02,
    QpackDecoder = 0x03,
};

enum class Http3FrameType : std::uint64_t {
    Data = 0x00,
    Headers = 0x01,
    CancelPush = 0x03,
    Settings = 0x04,
    PushPromise = 0x05,
    Goaway = 0x07,
    MaxPushId = 0x0d,
};

enum class Http3SettingId : std::uint64_t {
    QpackMaxTableCapacity = 0x01,
    MaxFieldSectionSize = 0x06,
    QpackBlockedStreams = 0x07,
    EnableConnectProtocol = 0x08,
};

struct Http3FrameHeader {
    std::uint64_t type = 0;
    std::uint64_t length = 0;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP3_PROTOCOL_H
