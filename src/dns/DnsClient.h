#ifndef FIBER_DNS_DNS_CLIENT_H
#define FIBER_DNS_DNS_CLIENT_H

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "../async/Spawn.h"
#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "../net/SocketAddress.h"
#include "DnsProtocol.h"

namespace fiber::net {
class UdpSocket;
}

namespace fiber::dns {

class DnsClient : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        net::SocketAddress server{};
        net::SocketAddress bind_addr{};
        std::chrono::milliseconds timeout{2000};
        std::uint8_t attempts = 2;
        bool enable_tcp_fallback = true;
        std::uint16_t max_inflight = 128;
        std::uint16_t max_udp_packet_size = 1232;
        QueryOptions query_options{};
    };

    DnsClient() noexcept;
    ~DnsClient();

    [[nodiscard]] bool init(event::EventLoop &loop, Options options) noexcept;
    void close() noexcept;
    void release() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] event::EventLoop &loop() const noexcept;
    [[nodiscard]] const net::SocketAddress &server() const noexcept;

    async::Task<common::IoResult<std::size_t>> query_raw(const QuestionSpec &question, std::uint8_t *dst,
                                                         std::size_t cap) noexcept;

private:
    static constexpr std::uint16_t kInvalidSlot = 0xffffU;
    static constexpr std::uint16_t kInvalidIdMapping = 0xffffU;
    using InflightCancelFn = void (*)(void *) noexcept;

    struct InflightSlot {
        bool active = false;
        bool completed = false;
        bool need_tcp_fallback = false;
        std::uint16_t id = 0;
        std::uint16_t next_free = kInvalidSlot;
        std::coroutine_handle<> waiter{};
        std::uint8_t *request_buf = nullptr;
        std::size_t request_size = 0;
        std::uint8_t *response_dst = nullptr;
        std::size_t response_cap = 0;
        std::size_t response_size = 0;
        common::IoErr completion_err = common::IoErr::None;
        void *cancel_context = nullptr;
        InflightCancelFn cancel = nullptr;
    };

    class ResponseAwaiter {
    public:
        ResponseAwaiter(DnsClient &client, std::uint16_t slot_index) noexcept;
        ~ResponseAwaiter();

        bool await_ready() noexcept;
        bool await_suspend(std::coroutine_handle<> handle) noexcept;
        common::IoResult<std::size_t> await_resume() noexcept;

    private:
        DnsClient *client_ = nullptr;
        std::uint16_t slot_index_ = kInvalidSlot;
        std::coroutine_handle<> handle_{};
        bool armed_ = false;
    };

    async::Task<void> recv_loop() noexcept;
    async::Task<common::IoResult<std::size_t>> query_tcp(std::uint16_t slot_index) noexcept;

    [[nodiscard]] bool init_storage() noexcept;
    void reset_state() noexcept;
    void cancel_all_inflight(common::IoErr err) noexcept;

    [[nodiscard]] std::uint16_t allocate_slot() noexcept;
    void release_slot(std::uint16_t slot_index) noexcept;
    [[nodiscard]] common::IoResult<std::uint16_t> allocate_query_id() noexcept;
    void clear_query_id(std::uint16_t id, std::uint16_t slot_index) noexcept;

    [[nodiscard]] bool response_ready(std::uint16_t slot_index) const noexcept;
    [[nodiscard]] bool arm_waiter(std::uint16_t slot_index, std::coroutine_handle<> handle) noexcept;
    void cancel_waiter(std::uint16_t slot_index, std::coroutine_handle<> handle) noexcept;
    [[nodiscard]] common::IoResult<std::size_t> wait_result(std::uint16_t slot_index) noexcept;
    void arm_inflight_cancel(std::uint16_t slot_index, void *context, InflightCancelFn cancel) noexcept;
    void disarm_inflight_cancel(std::uint16_t slot_index, void *context) noexcept;

    void complete_slot(std::uint16_t slot_index, common::IoErr err, std::size_t response_size,
                       bool need_tcp_fallback) noexcept;
    void handle_udp_packet(const std::uint8_t *packet, std::size_t packet_len, const net::SocketAddress &peer) noexcept;

    Options options_{};
    event::EventLoop *loop_ = nullptr;
    std::unique_ptr<net::UdpSocket> socket_{};
    std::unique_ptr<InflightSlot[]> slots_{};
    std::unique_ptr<std::uint16_t[]> id_to_slot_{};
    std::unique_ptr<std::uint8_t[]> request_buffers_{};
    std::unique_ptr<std::uint8_t[]> recv_buffer_{};
    std::uint16_t free_head_ = kInvalidSlot;
    std::uint16_t next_id_ = 0;
    bool closing_ = false;
};

} // namespace fiber::dns

#endif // FIBER_DNS_DNS_CLIENT_H
