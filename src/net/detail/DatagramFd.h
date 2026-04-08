#ifndef FIBER_NET_DETAIL_DATAGRAM_FD_H
#define FIBER_NET_DETAIL_DATAGRAM_FD_H

#include <coroutine>
#include <cstddef>
#include <optional>

#include "../../async/Task.h"
#include "../../common/Assert.h"
#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../../event/EventLoop.h"
#include "../UdpPacket.h"
#include "../SocketAddress.h"
#include "RWFd.h"

namespace fiber::net {

struct UdpBindOptions;
struct UdpRecvResult;

} // namespace fiber::net

namespace fiber::net::detail {

class DatagramFd : public common::NonCopyable, public common::NonMovable {
public:
    class RecvFromAwaiter;
    class SendToAwaiter;
    class RecvPacketAwaiter;
    class SendPacketAwaiter;
    using WaitEventAwaiter = RWFd::WaitEventAwaiter;
    using WaitReadableAwaiter = RWFd::WaitReadableAwaiter;
    using WaitWritableAwaiter = RWFd::WaitWritableAwaiter;

    explicit DatagramFd(fiber::event::EventLoop &loop);
    ~DatagramFd();

    fiber::common::IoResult<void> bind(const SocketAddress &addr, const UdpBindOptions &options);
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] const SocketAddress &local_addr() const noexcept;
    void close();

    [[nodiscard]] RecvFromAwaiter recv_from(void *buf, size_t len) noexcept;
    [[nodiscard]] SendToAwaiter send_to(const void *buf, size_t len, const SocketAddress &peer) noexcept;
    [[nodiscard]] fiber::common::IoResult<UdpRecvResult> try_recv_from(void *buf, size_t len) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_send_to(const void *buf,
                                                              size_t len,
                                                              const SocketAddress &peer) noexcept;

    [[nodiscard]] RecvPacketAwaiter recv_packet(void *buf, size_t len) noexcept;
    [[nodiscard]] SendPacketAwaiter send_packet(UdpPacketSendSpec spec) noexcept;
    [[nodiscard]] fiber::common::IoResult<UdpPacketRecvResult> try_recv_packet(void *buf, size_t len) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_send_packet(const UdpPacketSendSpec &spec) noexcept;
    [[nodiscard]] WaitEventAwaiter wait_event(fiber::event::IoEvent interested) noexcept;
    [[nodiscard]] WaitReadableAwaiter wait_readable() noexcept;
    [[nodiscard]] WaitWritableAwaiter wait_writable() noexcept;
    fiber::async::Task<fiber::common::IoResult<fiber::event::IoEvent>>
    wait_event(fiber::event::IoEvent interested, std::chrono::milliseconds timeout) noexcept;

private:
    fiber::common::IoResult<void> refresh_local_addr() noexcept;
    fiber::common::IoErr recv_packet_once(void *buf, size_t len, UdpPacketRecvResult &out) noexcept;
    fiber::common::IoErr send_packet_once(const UdpPacketSendSpec &spec, size_t &out) noexcept;

    RWFd rwfd_;
    SocketAddress local_addr_{};
};

class DatagramFd::RecvFromAwaiter {
public:
    RecvFromAwaiter(DatagramFd &socket, void *buf, size_t len) noexcept;

    RecvFromAwaiter(const RecvFromAwaiter &) = delete;
    RecvFromAwaiter &operator=(const RecvFromAwaiter &) = delete;
    RecvFromAwaiter(RecvFromAwaiter &&) = delete;
    RecvFromAwaiter &operator=(RecvFromAwaiter &&) = delete;
    ~RecvFromAwaiter();

    bool await_ready() noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> handle);
    fiber::common::IoResult<UdpRecvResult> await_resume() noexcept;

private:
    DatagramFd *socket_ = nullptr;
    void *buf_ = nullptr;
    size_t len_ = 0;
    UdpPacketRecvResult packet_{};
    fiber::common::IoErr err_{fiber::common::IoErr::None};
    std::optional<RWFd::WaitReadableAwaiter> waiter_{};
    bool waiting_ = false;
    bool completed_ = false;
};

class DatagramFd::SendToAwaiter {
public:
    SendToAwaiter(DatagramFd &socket, const void *buf, size_t len, SocketAddress peer) noexcept;

    SendToAwaiter(const SendToAwaiter &) = delete;
    SendToAwaiter &operator=(const SendToAwaiter &) = delete;
    SendToAwaiter(SendToAwaiter &&) = delete;
    SendToAwaiter &operator=(SendToAwaiter &&) = delete;
    ~SendToAwaiter();

    bool await_ready() noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> handle);
    fiber::common::IoResult<size_t> await_resume() noexcept;

private:
    DatagramFd *socket_ = nullptr;
    UdpPacketSendSpec spec_{};
    fiber::common::IoErr err_{fiber::common::IoErr::None};
    std::optional<RWFd::WaitWritableAwaiter> waiter_{};
    bool waiting_ = false;
    size_t result_ = 0;
    bool completed_ = false;
};

class DatagramFd::RecvPacketAwaiter {
public:
    RecvPacketAwaiter(DatagramFd &socket, void *buf, size_t len) noexcept;

    RecvPacketAwaiter(const RecvPacketAwaiter &) = delete;
    RecvPacketAwaiter &operator=(const RecvPacketAwaiter &) = delete;
    RecvPacketAwaiter(RecvPacketAwaiter &&) = delete;
    RecvPacketAwaiter &operator=(RecvPacketAwaiter &&) = delete;
    ~RecvPacketAwaiter();

    bool await_ready() noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> handle);
    fiber::common::IoResult<UdpPacketRecvResult> await_resume() noexcept;

private:
    DatagramFd *socket_ = nullptr;
    void *buf_ = nullptr;
    size_t len_ = 0;
    UdpPacketRecvResult result_{};
    fiber::common::IoErr err_{fiber::common::IoErr::None};
    std::optional<RWFd::WaitReadableAwaiter> waiter_{};
    bool waiting_ = false;
    bool completed_ = false;
};

class DatagramFd::SendPacketAwaiter {
public:
    SendPacketAwaiter(DatagramFd &socket, UdpPacketSendSpec spec) noexcept;

    SendPacketAwaiter(const SendPacketAwaiter &) = delete;
    SendPacketAwaiter &operator=(const SendPacketAwaiter &) = delete;
    SendPacketAwaiter(SendPacketAwaiter &&) = delete;
    SendPacketAwaiter &operator=(SendPacketAwaiter &&) = delete;
    ~SendPacketAwaiter();

    bool await_ready() noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> handle);
    fiber::common::IoResult<size_t> await_resume() noexcept;

private:
    DatagramFd *socket_ = nullptr;
    UdpPacketSendSpec spec_{};
    fiber::common::IoErr err_{fiber::common::IoErr::None};
    std::optional<RWFd::WaitWritableAwaiter> waiter_{};
    bool waiting_ = false;
    size_t result_ = 0;
    bool completed_ = false;
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_DATAGRAM_FD_H
