#ifndef FIBER_HTTP_HTTP3_CONNECTION_H
#define FIBER_HTTP_HTTP3_CONNECTION_H

#include <chrono>
#include <cstdint>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../quic/QuicConnection.h"

namespace fiber::http {

enum class Http3ConnectionState : std::uint8_t {
    Init,
    Running,
    Draining,
    Closing,
    Closed,
};

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
};

struct Http3Settings {
    std::uint64_t qpack_max_table_capacity = 0;
    std::uint64_t qpack_blocked_streams = 0;
    std::uint64_t max_field_section_size = 0;
    bool enable_connect_protocol = false;
};

class Http3Connection : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        Http3Settings local_settings{};
        std::chrono::milliseconds drain_timeout = std::chrono::seconds(3);
        bool enable_push = false;
    };

    explicit Http3Connection(quic::QuicConnection &quic) noexcept;
    Http3Connection(quic::QuicConnection &quic, const Options &options) noexcept;
    ~Http3Connection() = default;

    [[nodiscard]] quic::QuicConnection &quic() noexcept { return *quic_; }
    [[nodiscard]] const quic::QuicConnection &quic() const noexcept { return *quic_; }
    [[nodiscard]] quic::QuicConnectionRole role() const noexcept { return quic_->role(); }
    [[nodiscard]] Http3ConnectionState state() const noexcept { return state_; }
    [[nodiscard]] Http3ErrorCode close_error() const noexcept { return close_error_; }
    [[nodiscard]] const Http3Settings &local_settings() const noexcept { return options_.local_settings; }
    [[nodiscard]] const Http3Settings &peer_settings() const noexcept { return peer_settings_; }
    [[nodiscard]] bool peer_settings_received() const noexcept { return peer_settings_received_; }
    [[nodiscard]] bool closing() const noexcept {
        return state_ == Http3ConnectionState::Draining || state_ == Http3ConnectionState::Closing ||
               state_ == Http3ConnectionState::Closed;
    }

    common::IoResult<void> start() noexcept;
    common::IoResult<void> apply_peer_settings(const Http3Settings &settings) noexcept;
    void graceful_shutdown(Http3ErrorCode error = Http3ErrorCode::NoError) noexcept;
    void close(Http3ErrorCode error = Http3ErrorCode::NoError) noexcept;
    void mark_closed() noexcept;

private:
    quic::QuicConnection *quic_ = nullptr;
    Options options_{};
    Http3Settings peer_settings_{};
    Http3ConnectionState state_ = Http3ConnectionState::Init;
    Http3ErrorCode close_error_ = Http3ErrorCode::NoError;
    bool peer_settings_received_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP3_CONNECTION_H
