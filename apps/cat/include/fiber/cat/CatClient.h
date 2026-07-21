#ifndef FIBER_CAT_CLIENT_H
#define FIBER_CAT_CLIENT_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <utility>

#include <async/Task.h>
#include <common/IoError.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <event/EventLoop.h>

#include "CatClientConfig.h"

namespace fiber::dns {
class AddressResolver;
}

namespace fiber::cat {

namespace detail {
class CatClientCore;
class CatClientImpl;
} // namespace detail

class MessageTrace;

enum class CatClientState : std::uint8_t {
    Created,
    Running,
    Stopping,
    Stopped,
};

enum class CatClientCreateError : std::uint8_t {
    InvalidOptions,
    InvalidResolver,
    NoMemory,
};

struct CatClientStats {
    std::size_t queued_messages = 0;
    std::size_t queued_bytes = 0;
    std::uint64_t submitted_messages = 0;
    std::uint64_t sent_messages = 0;
    std::uint64_t sent_bytes = 0;
    std::uint64_t dropped_queue_full = 0;
    std::uint64_t dropped_unavailable = 0;
    std::uint64_t dropped_sampled = 0;
    std::uint64_t dropped_partial_frame = 0;
    std::uint64_t encode_failures = 0;
    std::uint64_t router_successes = 0;
    std::uint64_t router_failures = 0;
    std::uint64_t connect_successes = 0;
    std::uint64_t connect_failures = 0;
    std::uint64_t write_failures = 0;
};

class CatClient : public common::NonCopyable, public common::NonMovable {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<CatClient>, CatClientCreateError>
    create(event::EventLoop &sender_loop, CatClientConfig config, CatClientOptions options = {},
           dns::AddressResolver *resolver = nullptr) noexcept;

    ~CatClient();

    [[nodiscard]] common::IoResult<void> start() noexcept;
    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] CatClientState state() const noexcept;
    [[nodiscard]] CatClientStats stats() const noexcept;
    [[nodiscard]] event::EventLoop &sender_loop() const noexcept;

private:
    friend class MessageTrace;

    explicit CatClient(std::shared_ptr<detail::CatClientImpl> impl) noexcept : impl_(std::move(impl)) {}

    [[nodiscard]] std::shared_ptr<detail::CatClientCore> core() const noexcept;

    std::shared_ptr<detail::CatClientImpl> impl_;
};

} // namespace fiber::cat

#endif // FIBER_CAT_CLIENT_H
