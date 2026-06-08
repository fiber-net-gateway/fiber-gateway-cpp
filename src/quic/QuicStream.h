#ifndef FIBER_QUIC_QUIC_STREAM_H
#define FIBER_QUIC_QUIC_STREAM_H

#include <cstdint>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"

namespace fiber::quic {

enum class QuicStreamType : std::uint8_t {
    Bidirectional,
    Unidirectional,
};

class QuicStream : public common::NonCopyable, public common::NonMovable {
public:
    explicit QuicStream(std::uint64_t stream_id) noexcept;

    [[nodiscard]] std::uint64_t stream_id() const noexcept { return stream_id_; }
    [[nodiscard]] std::uint64_t sequence() const noexcept;
    [[nodiscard]] QuicStreamType type() const noexcept;
    [[nodiscard]] bool bidirectional() const noexcept;
    [[nodiscard]] bool unidirectional() const noexcept;

    [[nodiscard]] static std::uint64_t stream_sequence(std::uint64_t stream_id) noexcept;
    [[nodiscard]] static bool is_bidirectional_stream_id(std::uint64_t stream_id) noexcept;
    [[nodiscard]] static bool is_unidirectional_stream_id(std::uint64_t stream_id) noexcept;

private:
    std::uint64_t stream_id_ = 0;
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_STREAM_H
