#include "CatClientCore.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <coroutine>
#include <limits>
#include <new>
#include <string>
#include <utility>

#include <async/Sleep.h>
#include <async/Timeout.h>
#include <common/Assert.h>
#include <common/mem/BufPool.h>
#include <common/mem/IoBufChain.h>
#include <common/util/UrlForm.h>
#include <dns/DnsResolver.h>
#include <http/ClientHttp1Exchange.h>
#include <http/Http1ClientConnection.h>
#include <http/HttpHeaders.h>
#include <net/IpAddress.h>

#include "CatRouter.h"

namespace fiber::cat::detail {

namespace {

inline constexpr std::uint64_t kBudgetUnitMask = std::numeric_limits<std::uint32_t>::max();
inline constexpr std::size_t kWriteIovCapacity = 16;

std::uint64_t pack_budget(std::uint32_t messages, std::uint32_t bytes) noexcept {
    return static_cast<std::uint64_t>(messages) << 32 | bytes;
}

std::uint32_t budget_messages(std::uint64_t budget) noexcept {
    return static_cast<std::uint32_t>(budget >> 32) & std::numeric_limits<std::int32_t>::max();
}

std::uint32_t budget_bytes(std::uint64_t budget) noexcept { return static_cast<std::uint32_t>(budget); }

std::uint64_t splitmix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

std::uint64_t sample_cutoff(double sample) noexcept {
    if (sample <= 0.0) {
        return 0;
    }
    if (sample >= 1.0) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(sample * static_cast<long double>(std::numeric_limits<std::uint64_t>::max()));
}

std::chrono::milliseconds grow_backoff(std::chrono::milliseconds current, std::chrono::milliseconds maximum) noexcept {
    FIBER_ASSERT(current <= maximum);
    return current >= maximum - current ? maximum : current + current;
}

class NotifyDrainAwaiter {
public:
    NotifyDrainAwaiter() noexcept = default;
    NotifyDrainAwaiter(const NotifyDrainAwaiter &) = delete;
    NotifyDrainAwaiter &operator=(const NotifyDrainAwaiter &) = delete;
    NotifyDrainAwaiter(NotifyDrainAwaiter &&) = delete;
    NotifyDrainAwaiter &operator=(NotifyDrainAwaiter &&) = delete;

    ~NotifyDrainAwaiter() {
        if (!loop_) {
            return;
        }
        FIBER_ASSERT(loop_->in_loop());
        if (timer_entry_.is_in_heap()) {
            loop_->cancel<NotifyDrainAwaiter, &NotifyDrainAwaiter::timer_entry_>(*this);
        }
        if (defer_entry_.is_in_queue()) {
            loop_->cancel<NotifyDrainAwaiter, &NotifyDrainAwaiter::defer_entry_>(*this);
        }
    }

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> handle) noexcept {
        loop_ = &event::EventLoop::current();
        handle_ = handle;
        loop_->post_at<NotifyDrainAwaiter, &NotifyDrainAwaiter::timer_entry_, &NotifyDrainAwaiter::on_timer>(
                loop_->now(), *this);
    }

    void await_resume() noexcept {
        FIBER_ASSERT(!handle_);
        loop_ = nullptr;
    }

private:
    static void on_timer(NotifyDrainAwaiter *awaiter) noexcept {
        awaiter->loop_
                ->post_local<NotifyDrainAwaiter, &NotifyDrainAwaiter::defer_entry_, &NotifyDrainAwaiter::on_deferred>(
                        *awaiter);
    }

    static void on_deferred(NotifyDrainAwaiter *awaiter) noexcept {
        std::coroutine_handle<> handle = std::exchange(awaiter->handle_, {});
        FIBER_ASSERT(handle);
        handle.resume();
    }

    event::EventLoop *loop_ = nullptr;
    std::coroutine_handle<> handle_{};
    event::EventLoop::TimerEntry timer_entry_{};
    event::EventLoop::DeferEntry defer_entry_{};
};

} // namespace

CatClientCore::CatClientCore(event::EventLoop &sender_loop, CatClientConfig config, CatClientOptions options,
                             dns::AddressResolver *resolver) noexcept :
    loop_(&sender_loop), config_(std::move(config)), options_(std::move(options)), resolver_(resolver),
    collectors_(config_.bootstrap_collectors()) {
    control_publisher_ = control_wake_.acquire_publisher();
    FIBER_ASSERT(control_publisher_.has_value());
}

CatClientCore::~CatClientCore() {
    FIBER_ASSERT(state() == CatClientState::Created || state() == CatClientState::Stopped);
    FIBER_ASSERT(active_submitters_.load(std::memory_order_relaxed) == 0);
    FIBER_ASSERT((outstanding_state_.load(std::memory_order_relaxed) & ~kClosedMask) == 0);
    FIBER_ASSERT(local_head_ == nullptr);
}

common::IoResult<void> CatClientCore::start() noexcept {
    if (!loop_->in_loop()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    CatClientState expected = CatClientState::Created;
    if (!state_.compare_exchange_strong(expected, CatClientState::Running, std::memory_order_acq_rel)) {
        return std::unexpected(common::IoErr::Already);
    }
    const std::uint64_t previous = outstanding_state_.fetch_and(~kClosedMask, std::memory_order_acq_rel);
    FIBER_ASSERT((previous & ~kClosedMask) == 0);
    if (state() != CatClientState::Running) {
        outstanding_state_.fetch_or(kClosedMask, std::memory_order_acq_rel);
    }
    control_done_.add();
    std::shared_ptr<CatClientCore> self = shared_from_this();
    async::spawn([self = std::move(self)]() { return self->run_control(); });
    return {};
}

async::Task<void> CatClientCore::shutdown() noexcept {
    begin_stop();
    co_await control_done_.join();
}

void CatClientCore::begin_stop() noexcept {
    CatClientState current = state_.load(std::memory_order_acquire);
    for (;;) {
        if (current == CatClientState::Created) {
            if (state_.compare_exchange_weak(current, CatClientState::Stopped, std::memory_order_acq_rel)) {
                outstanding_state_.fetch_or(kClosedMask, std::memory_order_acq_rel);
                return;
            }
            continue;
        }
        if (current == CatClientState::Running) {
            if (state_.compare_exchange_weak(current, CatClientState::Stopping, std::memory_order_acq_rel)) {
                outstanding_state_.fetch_or(kClosedMask, std::memory_order_acq_rel);
                notify_control();
                return;
            }
            continue;
        }
        outstanding_state_.fetch_or(kClosedMask, std::memory_order_acq_rel);
        return;
    }
}

CatClientStats CatClientCore::stats() const noexcept {
    const std::uint64_t budget = outstanding_state_.load(std::memory_order_acquire);
    return {
            .queued_messages = budget_messages(budget),
            .queued_bytes = budget_bytes(budget),
            .submitted_messages = stats_.submitted_messages.load(std::memory_order_relaxed),
            .sent_messages = stats_.sent_messages.load(std::memory_order_relaxed),
            .sent_bytes = stats_.sent_bytes.load(std::memory_order_relaxed),
            .dropped_queue_full = stats_.dropped_queue_full.load(std::memory_order_relaxed),
            .dropped_unavailable = stats_.dropped_unavailable.load(std::memory_order_relaxed),
            .dropped_sampled = stats_.dropped_sampled.load(std::memory_order_relaxed),
            .dropped_partial_frame = stats_.dropped_partial_frame.load(std::memory_order_relaxed),
            .encode_failures = stats_.encode_failures.load(std::memory_order_relaxed),
            .router_successes = stats_.router_successes.load(std::memory_order_relaxed),
            .router_failures = stats_.router_failures.load(std::memory_order_relaxed),
            .connect_successes = stats_.connect_successes.load(std::memory_order_relaxed),
            .connect_failures = stats_.connect_failures.load(std::memory_order_relaxed),
            .write_failures = stats_.write_failures.load(std::memory_order_relaxed),
    };
}

ClientEncodeContext CatClientCore::encode_context() const noexcept {
    return {
            .app_key = config_.app_key(),
            .hostname = config_.hostname(),
            .ip = config_.ip(),
            .thread_group_name = config_.thread_group_name(),
            .thread_id = config_.thread_id(),
            .thread_name = config_.thread_name(),
    };
}

bool CatClientCore::accepts_messages() const noexcept {
    return state() == CatClientState::Running &&
           (outstanding_state_.load(std::memory_order_acquire) & kClosedMask) == 0 &&
           !blocked_.load(std::memory_order_acquire) && sample_cutoff_.load(std::memory_order_acquire) != 0;
}

CatClientCore::ReserveResult CatClientCore::reserve_budget(std::size_t bytes) noexcept {
    if (bytes > options_.max_queued_bytes || bytes > kBudgetUnitMask) {
        return ReserveResult::Full;
    }
    const auto bytes32 = static_cast<std::uint32_t>(bytes);
    std::uint64_t current = outstanding_state_.load(std::memory_order_relaxed);
    for (;;) {
        if (state() != CatClientState::Running || (current & kClosedMask) != 0) {
            return ReserveResult::Closed;
        }
        const std::uint32_t messages = budget_messages(current);
        const std::uint32_t queued_bytes = budget_bytes(current);
        if (messages >= options_.max_queued_messages || queued_bytes > options_.max_queued_bytes - bytes) {
            return ReserveResult::Full;
        }
        const std::uint64_t next = pack_budget(messages + 1, queued_bytes + bytes32);
        if (outstanding_state_.compare_exchange_weak(current, next, std::memory_order_acq_rel,
                                                     std::memory_order_relaxed)) {
            return ReserveResult::Reserved;
        }
    }
}

void CatClientCore::release_budget(std::size_t bytes) noexcept {
    FIBER_ASSERT(bytes <= kBudgetUnitMask);
    const std::uint64_t delta = pack_budget(1, static_cast<std::uint32_t>(bytes));
    const std::uint64_t previous = outstanding_state_.fetch_sub(delta, std::memory_order_acq_rel);
    FIBER_ASSERT(budget_messages(previous) > 0);
    FIBER_ASSERT(budget_bytes(previous) >= bytes);
}

bool CatClientCore::sampled_in() noexcept {
    const std::uint64_t cutoff = sample_cutoff_.load(std::memory_order_acquire);
    if (cutoff == 0) {
        return false;
    }
    if (cutoff == std::numeric_limits<std::uint64_t>::max()) {
        return true;
    }
    const std::uint64_t sequence = sample_sequence_.fetch_add(1, std::memory_order_relaxed);
    return splitmix64(sequence) <= cutoff;
}

void CatClientCore::submit_encoded(mem::IoBuf message) noexcept {
    active_submitters_.fetch_add(1, std::memory_order_acq_rel);
    if ((outstanding_state_.load(std::memory_order_acquire) & kClosedMask) != 0 ||
        blocked_.load(std::memory_order_acquire)) {
        stats_.dropped_unavailable.fetch_add(1, std::memory_order_relaxed);
        active_submitters_.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }
    if (!sampled_in()) {
        stats_.dropped_sampled.fetch_add(1, std::memory_order_relaxed);
        active_submitters_.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }

    const std::size_t bytes = message.readable();
    if (bytes == 0) {
        stats_.dropped_queue_full.fetch_add(1, std::memory_order_relaxed);
        active_submitters_.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }
    const ReserveResult reserved = reserve_budget(bytes);
    if (reserved != ReserveResult::Reserved) {
        if (reserved == ReserveResult::Full) {
            stats_.dropped_queue_full.fetch_add(1, std::memory_order_relaxed);
        } else {
            stats_.dropped_unavailable.fetch_add(1, std::memory_order_relaxed);
        }
        active_submitters_.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }
    auto *frame = new (std::nothrow) OutboundFrame(*this, std::move(message));
    if (!frame) {
        release_budget(bytes);
        stats_.dropped_unavailable.fetch_add(1, std::memory_order_relaxed);
        active_submitters_.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }
    if (loop_->in_loop()) {
        handle_frame_notify(frame);
    } else {
        loop_->post<OutboundFrame, &OutboundFrame::notify_entry, &OutboundFrame::on_notify>(*frame);
    }
    stats_.submitted_messages.fetch_add(1, std::memory_order_relaxed);
    active_submitters_.fetch_sub(1, std::memory_order_acq_rel);
}

void CatClientCore::on_encode_failure(EncodeError /*error*/) noexcept {
    stats_.encode_failures.fetch_add(1, std::memory_order_relaxed);
}

void CatClientCore::OutboundFrame::on_notify(OutboundFrame *frame) noexcept {
    FIBER_ASSERT(frame);
    FIBER_ASSERT(frame->core);
    frame->core->handle_frame_notify(frame);
}

void CatClientCore::handle_frame_notify(OutboundFrame *frame) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(frame);
    FIBER_ASSERT(frame->core == this);
    if (blocked_.load(std::memory_order_acquire)) {
        drop_detached_frame(frame);
        return;
    }
    append_local(frame);
    schedule_pump();
}

void CatClientCore::append_local(OutboundFrame *frame) noexcept {
    FIBER_ASSERT(frame);
    FIBER_ASSERT(frame->local_next == nullptr);
    if (local_tail_) {
        local_tail_->local_next = frame;
    } else {
        local_head_ = frame;
    }
    local_tail_ = frame;
}

void CatClientCore::schedule_pump() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    loop_->post_local<CatClientCore, &CatClientCore::pump_defer_entry_, &CatClientCore::on_pump_deferred>(*this);
}

void CatClientCore::on_pump_deferred(CatClientCore *client) noexcept {
    if (client->blocked_.load(std::memory_order_acquire)) {
        client->drop_all_frames();
        return;
    }
    client->drive_write();
    if (!client->stream_ && client->local_head_ && client->state() == CatClientState::Running) {
        client->notify_control();
    }
}

void CatClientCore::drive_write() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (!stream_ || !local_head_ || write_callback_armed_) {
        return;
    }

    std::size_t pump_bytes = 0;
    std::size_t pump_calls = 0;
    while (stream_ && local_head_ && pump_bytes < options_.max_send_bytes_per_pump &&
           pump_calls < options_.max_send_calls_per_pump) {
        std::array<iovec, kWriteIovCapacity> iov{};
        std::size_t count = 0;
        std::size_t batch_bytes = 0;
        for (OutboundFrame *frame = local_head_; frame && count < options_.max_batch_messages;
             frame = frame->local_next) {
            const std::size_t readable = frame->message.readable();
            if (count != 0 &&
                (batch_bytes >= options_.max_batch_bytes || readable > options_.max_batch_bytes - batch_bytes)) {
                break;
            }
            iov[count].iov_base = frame->message.readable_data();
            iov[count].iov_len = readable;
            batch_bytes += readable;
            ++count;
        }
        FIBER_ASSERT(count > 0);

        auto written = stream_->try_writev(iov.data(), static_cast<int>(count));
        ++pump_calls;
        if (!written) {
            if (written.error() == common::IoErr::WouldBlock) {
                arm_write_wait();
            } else {
                fail_connection(written.error());
            }
            return;
        }
        if (*written == 0) {
            fail_connection(common::IoErr::BrokenPipe);
            return;
        }
        pump_bytes += *written;
        consume_written(*written);
    }

    if (stream_ && local_head_ && !write_callback_armed_) {
        schedule_pump();
    }
}

void CatClientCore::consume_written(std::size_t bytes) noexcept {
    stats_.sent_bytes.fetch_add(bytes, std::memory_order_relaxed);
    std::size_t remaining = bytes;
    while (remaining > 0) {
        FIBER_ASSERT(local_head_);
        const std::size_t readable = local_head_->message.readable();
        const std::size_t consumed = std::min(readable, remaining);
        local_head_->message.consume(consumed);
        remaining -= consumed;
        if (consumed != readable) {
            break;
        }

        OutboundFrame *completed = local_head_;
        local_head_ = completed->local_next;
        if (!local_head_) {
            local_tail_ = nullptr;
        }
        release_budget(completed->original_size);
        stats_.sent_messages.fetch_add(1, std::memory_order_relaxed);
        delete completed;
    }
}

void CatClientCore::arm_write_wait() noexcept {
    FIBER_ASSERT(stream_);
    FIBER_ASSERT(!write_callback_armed_);
    const common::IoErr result = stream_->set_write_callback(&CatClientCore::on_write_ready, this);
    if (result != common::IoErr::None) {
        fail_connection(result);
        return;
    }
    write_callback_armed_ = true;
    loop_->post_at<CatClientCore, &CatClientCore::write_timer_, &CatClientCore::on_write_timeout>(
            loop_->now() + options_.collector_write_timeout, *this);
}

void CatClientCore::clear_write_wait() noexcept {
    if (write_timer_.is_in_heap()) {
        loop_->cancel<CatClientCore, &CatClientCore::write_timer_>(*this);
    }
    if (write_callback_armed_ && stream_) {
        (void) stream_->clear_write_callback(&CatClientCore::on_write_ready, this);
    }
    write_callback_armed_ = false;
}

void CatClientCore::on_write_ready(void *ctx, common::IoErr error) noexcept {
    auto *client = static_cast<CatClientCore *>(ctx);
    client->clear_write_wait();
    if (error != common::IoErr::None) {
        client->fail_connection(error);
        return;
    }
    client->drive_write();
}

void CatClientCore::on_write_timeout(CatClientCore *client) noexcept {
    client->clear_write_wait();
    client->fail_connection(common::IoErr::TimedOut);
}

void CatClientCore::fail_connection(common::IoErr /*error*/) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    stats_.write_failures.fetch_add(1, std::memory_order_relaxed);
    if (local_head_ && local_head_->message.readable() != local_head_->original_size) {
        drop_front_frame(true);
    }
    close_connection();
    notify_control();
}

void CatClientCore::install_connection(std::unique_ptr<net::TcpStream> stream) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    close_connection();
    stream_ = std::move(stream);
    schedule_pump();
}

void CatClientCore::close_connection() noexcept {
    clear_write_wait();
    if (stream_) {
        stream_->close();
        stream_.reset();
    }
}

void CatClientCore::drop_detached_frame(OutboundFrame *frame) noexcept {
    FIBER_ASSERT(frame);
    FIBER_ASSERT(frame->core == this);
    FIBER_ASSERT(frame->local_next == nullptr);
    release_budget(frame->original_size);
    stats_.dropped_unavailable.fetch_add(1, std::memory_order_relaxed);
    delete frame;
}

void CatClientCore::drop_front_frame(bool partial) noexcept {
    FIBER_ASSERT(local_head_);
    OutboundFrame *dropped = local_head_;
    local_head_ = dropped->local_next;
    if (!local_head_) {
        local_tail_ = nullptr;
    }
    release_budget(dropped->original_size);
    if (partial) {
        stats_.dropped_partial_frame.fetch_add(1, std::memory_order_relaxed);
    } else {
        stats_.dropped_unavailable.fetch_add(1, std::memory_order_relaxed);
    }
    delete dropped;
}

void CatClientCore::drop_all_frames() noexcept {
    while (local_head_) {
        drop_front_frame(local_head_->message.readable() != local_head_->original_size);
    }
}

void CatClientCore::notify_control() noexcept {
    const std::uint64_t generation = control_generation_.fetch_add(1, std::memory_order_relaxed) + 1;
    control_publisher_->publish(generation);
}

async::Task<void> CatClientCore::wait_control(std::chrono::steady_clock::duration delay,
                                              async::Watch<std::uint64_t>::Subscriber &wake,
                                              std::uint64_t &version) noexcept {
    if (delay <= std::chrono::steady_clock::duration::zero()) {
        co_return;
    }
    auto result = co_await async::timeout_for([&wake, version]() { return wake.next(version); }, delay);
    if (result) {
        version = result->version;
    } else {
        FIBER_ASSERT(result.error() == common::IoErr::TimedOut);
    }
}

async::DetachedTask CatClientCore::run_control() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    auto wake = control_wake_.subscribe();
    std::uint64_t wake_version = wake.current().version;
    auto reconnect_delay = options_.reconnect_initial_delay;
    auto next_connect_at = loop_->now();
    auto next_router_at = config_.routers().empty() ? std::chrono::steady_clock::time_point::max() : loop_->now();

    while (state() == CatClientState::Running) {
        auto now = loop_->now();
        if (!config_.routers().empty() && now >= next_router_at) {
            const bool refreshed = co_await refresh_router();
            now = loop_->now();
            next_router_at = now + (refreshed ? options_.router_refresh_interval : options_.reconnect_max_delay);
            if (refreshed) {
                reconnect_delay = options_.reconnect_initial_delay;
                next_connect_at = now;
            }
        }
        if (state() != CatClientState::Running) {
            break;
        }

        if (!blocked_.load(std::memory_order_acquire) && !stream_ && !collectors_.empty() && now >= next_connect_at) {
            const bool connected = co_await connect_collector();
            now = loop_->now();
            if (connected) {
                reconnect_delay = options_.reconnect_initial_delay;
            } else {
                next_connect_at = now + reconnect_delay;
                reconnect_delay = grow_backoff(reconnect_delay, options_.reconnect_max_delay);
            }
        }
        if (state() != CatClientState::Running) {
            break;
        }

        now = loop_->now();
        auto next_action = next_router_at;
        if (!blocked_.load(std::memory_order_acquire) && !stream_ && !collectors_.empty()) {
            next_action = std::min(next_action, next_connect_at);
        }
        if (next_action == std::chrono::steady_clock::time_point::max()) {
            next_action = now + options_.router_refresh_interval;
        }
        co_await wait_control(std::max(next_action - now, std::chrono::steady_clock::duration::zero()), wake,
                              wake_version);
    }

    co_await finish_shutdown();
    state_.store(CatClientState::Stopped, std::memory_order_release);
    control_done_.done();
}

async::Task<std::optional<std::vector<net::SocketAddress>>>
CatClientCore::resolve_endpoint(std::string_view host, std::uint16_t port) noexcept {
    net::IpAddress literal;
    if (net::IpAddress::parse(host, literal) && !literal.is_unspecified()) {
        std::vector<net::SocketAddress> result;
        result.emplace_back(literal, port);
        co_return result;
    }
    if (!resolver_ || !resolver_->valid()) {
        co_return std::nullopt;
    }
    dns::EndpointResolveResult resolved;
    if (!resolved.init({.max_records = 16, .max_name_storage = 512})) {
        co_return std::nullopt;
    }
    auto status = co_await resolver_->resolve(host, port, resolved);
    if (!status || *status != dns::ResolveStatus::Success || resolved.record_count() == 0) {
        co_return std::nullopt;
    }
    std::vector<net::SocketAddress> result;
    result.reserve(resolved.record_count());
    for (std::uint16_t index = 0; index < resolved.record_count(); ++index) {
        result.push_back(resolved.records()[index]);
    }
    co_return result;
}

async::Task<std::optional<std::string>> CatClientCore::fetch_router_body(const CatRouterEndpoint &router) noexcept {
    auto endpoints = co_await resolve_endpoint(router.host, router.port);
    if (!endpoints) {
        co_return std::nullopt;
    }

    std::string target = "/cat/s/router?op=json&domain=";
    util::form_encode(config_.app_key(), target);
    target.append("&ip=");
    util::form_encode(config_.ip(), target);
    target.append("&hostname=");
    util::form_encode(config_.hostname(), target);

    net::IpAddress router_literal;
    const bool router_is_v6 = net::IpAddress::parse(router.host, router_literal) && router_literal.is_v6();
    std::string host_header;
    if (router_is_v6) {
        host_header.push_back('[');
        host_header.append(router.host);
        host_header.push_back(']');
    } else {
        host_header = router.host;
    }
    if (router.port != 80) {
        host_header.push_back(':');
        host_header.append(std::to_string(router.port));
    }

    for (const net::SocketAddress &endpoint: *endpoints) {
        http::Http1ClientConnectionOptions connection_options;
        connection_options.peer_addr = endpoint;
        http::Http1ClientConnection connection(*loop_, std::move(connection_options));
        auto connected = co_await connection.connect(options_.router_connect_timeout);
        if (!connected) {
            continue;
        }

        std::optional<std::string> body;
        {
            mem::BufPool pool;
            http::HttpHeaders headers(pool);
            if (!headers.add_view("host", host_header) || !headers.add_view("connection", "close")) {
                connection.close();
                co_return std::nullopt;
            }
            http::ClientHttp1Exchange exchange(connection, pool);
            http::Http1RequestHead request{
                    .method = http::HttpMethod::Get,
                    .target = target,
                    .headers = &headers,
                    .body = http::HttpBodySpec::None(),
            };
            auto sent = co_await exchange.send_header(request, true, options_.router_request_timeout);
            if (!sent) {
                continue;
            }
            auto response = co_await exchange.read_header(options_.router_request_timeout);
            if (!response || (*response)->status_code != 200) {
                continue;
            }

            std::string collected;
            bool failed = false;
            for (;;) {
                const std::size_t remaining = collected.size() <= options_.max_router_response_bytes
                                                      ? options_.max_router_response_bytes - collected.size()
                                                      : 0;
                auto chunk = co_await exchange.read_body(std::min<std::size_t>(remaining + 1, 16 * 1024),
                                                         options_.router_request_timeout);
                if (!chunk) {
                    failed = true;
                    break;
                }
                const bool complete = chunk->complete();
                while (auto *front = chunk->front()) {
                    if (front->readable() == 0) {
                        chunk->drop_empty_front();
                        continue;
                    }
                    if (front->readable() > options_.max_router_response_bytes -
                                                    std::min(collected.size(), options_.max_router_response_bytes)) {
                        failed = true;
                        break;
                    }
                    collected.append(reinterpret_cast<const char *>(front->readable_data()), front->readable());
                    chunk->consume_and_compact(front->readable());
                }
                if (failed || collected.size() > options_.max_router_response_bytes) {
                    failed = true;
                    break;
                }
                if (complete) {
                    break;
                }
            }
            if (!failed) {
                body.emplace(std::move(collected));
            }
        }
        connection.close();
        if (body) {
            co_return body;
        }
    }
    co_return std::nullopt;
}

async::Task<bool> CatClientCore::refresh_router() noexcept {
    const std::size_t count = config_.routers().size();
    for (std::size_t offset = 0; offset < count && state() == CatClientState::Running; ++offset) {
        const std::size_t index = (router_index_ + offset) % count;
        auto body = co_await fetch_router_body(config_.routers()[index]);
        if (!body) {
            continue;
        }
        auto snapshot = parse_router_response(*body, options_.max_collectors);
        if (!snapshot) {
            continue;
        }

        router_index_ = (index + 1) % count;
        collectors_ = std::move(snapshot->collectors);
        collector_index_ = 0;
        sample_cutoff_.store(sample_cutoff(snapshot->sample), std::memory_order_release);
        blocked_.store(snapshot->block, std::memory_order_release);
        if (snapshot->block) {
            close_connection();
            drop_all_frames();
        }
        stats_.router_successes.fetch_add(1, std::memory_order_relaxed);
        co_return true;
    }
    stats_.router_failures.fetch_add(1, std::memory_order_relaxed);
    co_return false;
}

async::Task<bool> CatClientCore::connect_collector() noexcept {
    const std::size_t count = collectors_.size();
    for (std::size_t offset = 0; offset < count && state() == CatClientState::Running; ++offset) {
        const std::size_t index = (collector_index_ + offset) % count;
        auto connected =
                co_await net::TcpStream::connect(*loop_, collectors_[index], options_.collector_connect_timeout);
        if (!connected) {
            stats_.connect_failures.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        auto *raw_stream = new (std::nothrow) net::TcpStream(std::move(*connected));
        if (!raw_stream) {
            stats_.connect_failures.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        std::unique_ptr<net::TcpStream> stream(raw_stream);
        const common::IoErr configured = stream->apply_socket_options(options_.collector_tcp);
        if (configured != common::IoErr::None) {
            stream->close();
            stats_.connect_failures.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        collector_index_ = (index + 1) % count;
        install_connection(std::move(stream));
        stats_.connect_successes.fetch_add(1, std::memory_order_relaxed);
        co_return true;
    }
    co_return false;
}

async::Task<void> CatClientCore::finish_shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    while (active_submitters_.load(std::memory_order_acquire) != 0) {
        co_await async::sleep(std::chrono::milliseconds(1));
    }
    co_await NotifyDrainAwaiter{};

    const auto deadline = loop_->now() + options_.shutdown_drain_timeout;
    while ((outstanding_state_.load(std::memory_order_acquire) & ~kClosedMask) != 0 && loop_->now() < deadline) {
        if (stream_ && !write_callback_armed_) {
            schedule_pump();
        }
        co_await async::sleep(std::chrono::milliseconds(1));
    }

    loop_->cancel<CatClientCore, &CatClientCore::pump_defer_entry_>(*this);
    close_connection();
    drop_all_frames();
    FIBER_ASSERT((outstanding_state_.load(std::memory_order_acquire) & ~kClosedMask) == 0);
}

} // namespace fiber::cat::detail
