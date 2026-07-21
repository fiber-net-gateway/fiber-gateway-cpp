#ifndef FIBER_CAT_CLIENT_IMPL_H
#define FIBER_CAT_CLIENT_IMPL_H

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <async/Spawn.h>
#include <async/Task.h>
#include <async/WaitGroup.h>
#include <async/Watch.h>
#include <common/IoError.h>
#include <event/EventLoop.h>
#include <event/MpscQueue.h>
#include <fiber/cat/CatClient.h>
#include <net/SocketAddress.h>
#include <net/TcpStream.h>

#include "CatClientCore.h"

namespace fiber::dns {
class AddressResolver;
}

namespace fiber::cat::detail {

class CatClientImpl final : public CatClientCore, public std::enable_shared_from_this<CatClientImpl> {
public:
    CatClientImpl(event::EventLoop &sender_loop, CatClientConfig config, CatClientOptions options,
                  dns::AddressResolver *resolver) noexcept;
    ~CatClientImpl() override;

    [[nodiscard]] common::IoResult<void> start() noexcept;
    [[nodiscard]] async::Task<void> shutdown() noexcept;
    void begin_stop() noexcept;

    [[nodiscard]] CatClientState state() const noexcept { return state_.load(std::memory_order_acquire); }
    [[nodiscard]] CatClientStats stats() const noexcept;
    [[nodiscard]] event::EventLoop &sender_loop() const noexcept { return *loop_; }

    [[nodiscard]] ClientEncodeContext encode_context() const noexcept override;
    [[nodiscard]] bool accepts_messages() const noexcept override;
    void submit_encoded(mem::IoBuf message) noexcept override;
    void on_encode_failure(EncodeError error) noexcept override;

private:
    struct OutboundFrame {
        explicit OutboundFrame(mem::IoBuf value) noexcept :
            message(std::move(value)), original_size(message.readable()), submit_node(this) {}

        mem::IoBuf message;
        std::size_t original_size = 0;
        event::MpscQueue<OutboundFrame *>::Node submit_node;
        OutboundFrame *local_next = nullptr;
    };

    enum class NotifyState : std::uint8_t {
        Idle,
        Posted,
        Running,
    };

    struct AtomicStats {
        std::atomic<std::uint64_t> submitted_messages{0};
        std::atomic<std::uint64_t> sent_messages{0};
        std::atomic<std::uint64_t> sent_bytes{0};
        std::atomic<std::uint64_t> dropped_queue_full{0};
        std::atomic<std::uint64_t> dropped_unavailable{0};
        std::atomic<std::uint64_t> dropped_sampled{0};
        std::atomic<std::uint64_t> dropped_partial_frame{0};
        std::atomic<std::uint64_t> encode_failures{0};
        std::atomic<std::uint64_t> router_successes{0};
        std::atomic<std::uint64_t> router_failures{0};
        std::atomic<std::uint64_t> connect_successes{0};
        std::atomic<std::uint64_t> connect_failures{0};
        std::atomic<std::uint64_t> write_failures{0};
    };

    [[nodiscard]] bool reserve_budget(std::size_t bytes) noexcept;
    void release_budget(std::size_t bytes) noexcept;
    [[nodiscard]] bool sampled_in() noexcept;

    void request_submit_notify() noexcept;
    static void on_submit_notify(CatClientImpl *client) noexcept;
    void handle_submit_notify() noexcept;
    void append_submission_nodes(event::MpscQueue<OutboundFrame *>::Node *nodes) noexcept;
    void append_local(OutboundFrame *frame) noexcept;

    static void on_pump_deferred(CatClientImpl *client) noexcept;
    void drive_write() noexcept;
    void consume_written(std::size_t bytes) noexcept;
    void arm_write_wait() noexcept;
    void clear_write_wait() noexcept;
    static void on_write_ready(void *ctx, common::IoErr error) noexcept;
    static void on_write_timeout(CatClientImpl *client) noexcept;
    void fail_connection(common::IoErr error) noexcept;
    void install_connection(std::unique_ptr<net::TcpStream> stream) noexcept;
    void close_connection() noexcept;
    void drop_front_frame(bool partial) noexcept;
    void drop_all_frames() noexcept;

    void notify_control() noexcept;
    [[nodiscard]] async::DetachedTask run_control() noexcept;
    [[nodiscard]] async::Task<void> wait_control(std::chrono::steady_clock::duration delay,
                                                 async::Watch<std::uint64_t>::Subscriber &wake,
                                                 std::uint64_t &version) noexcept;
    [[nodiscard]] async::Task<bool> refresh_router() noexcept;
    [[nodiscard]] async::Task<std::optional<std::vector<net::SocketAddress>>>
    resolve_endpoint(std::string_view host, std::uint16_t port) noexcept;
    [[nodiscard]] async::Task<std::optional<std::string>> fetch_router_body(const CatRouterEndpoint &router) noexcept;
    [[nodiscard]] async::Task<bool> connect_collector() noexcept;
    [[nodiscard]] async::Task<void> finish_shutdown() noexcept;

    event::EventLoop *loop_ = nullptr;
    CatClientConfig config_;
    CatClientOptions options_;
    dns::AddressResolver *resolver_ = nullptr;

    std::atomic<CatClientState> state_{CatClientState::Created};
    std::atomic_bool accepting_{false};
    std::atomic_bool blocked_{false};
    std::atomic<std::uint64_t> sample_cutoff_{~std::uint64_t{0}};
    std::atomic<std::uint64_t> sample_sequence_{0};
    std::atomic<std::uint32_t> active_submitters_{0};
    std::atomic<std::uint64_t> queue_budget_{0};
    AtomicStats stats_;

    event::MpscQueue<OutboundFrame *> submission_queue_;
    std::atomic<NotifyState> notify_state_{NotifyState::Idle};
    event::EventLoop::NotifyEntry submit_notify_entry_{};
    event::EventLoop::DeferEntry pump_defer_entry_{};
    OutboundFrame *local_head_ = nullptr;
    OutboundFrame *local_tail_ = nullptr;

    std::unique_ptr<net::TcpStream> stream_;
    event::EventLoop::TimerEntry write_timer_{};
    bool write_callback_armed_ = false;

    async::Watch<std::uint64_t> control_wake_{0};
    std::optional<async::Watch<std::uint64_t>::Publisher> control_publisher_;
    std::atomic<std::uint64_t> control_generation_{0};
    async::WaitGroup control_done_;
    std::vector<net::SocketAddress> collectors_;
    std::size_t collector_index_ = 0;
    std::size_t router_index_ = 0;
};

} // namespace fiber::cat::detail

#endif // FIBER_CAT_CLIENT_IMPL_H
