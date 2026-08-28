#include <fiber/dns/DnsClient.h>

#include <fiber/dns/DnsMessage.h>
#include "dns/detail/DnsQuerySecurity.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <expected>
#include <limits>
#include <span>
#include <utility>

#include <openssl/rand.h>

#include <fiber/async/Timeout.h>
#include <fiber/common/Assert.h>
#include <fiber/net/TcpStream.h>
#include <fiber/net/UdpSocket.h>

namespace fiber::dns {

namespace {

constexpr std::size_t kDnsHeaderSize = 12;

constexpr std::size_t udp_transport_index(net::IpFamily family) noexcept {
    return family == net::IpFamily::V4 ? 0U : 1U;
}

std::uint16_t read_be16(const std::uint8_t *data) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8U) | data[1]);
}

void write_be16(std::uint8_t *dst, std::uint16_t value) noexcept {
    dst[0] = static_cast<std::uint8_t>(value >> 8U);
    dst[1] = static_cast<std::uint8_t>(value & 0xffU);
}

bool ip_equal(const net::IpAddress &a, const net::IpAddress &b) noexcept {
    if (a.family() != b.family()) {
        return false;
    }
    if (a.is_v4()) {
        return a.v4_bytes() == b.v4_bytes();
    }
    return a.scope_id() == b.scope_id() && a.v6_bytes() == b.v6_bytes();
}

bool socket_address_equal(const net::SocketAddress &a, const net::SocketAddress &b) noexcept {
    return a.port() == b.port() && ip_equal(a.ip(), b.ip());
}

bool is_generic_unspecified(const net::SocketAddress &addr) noexcept {
    return addr.port() == 0 && addr.ip().is_unspecified();
}

bool is_terminal_local_error(common::IoErr err) noexcept {
    switch (err) {
        case common::IoErr::Canceled:
        case common::IoErr::Invalid:
        case common::IoErr::BadFd:
        case common::IoErr::Busy:
        case common::IoErr::Permission:
        case common::IoErr::NoMem:
        case common::IoErr::MessageTooLarge:
        case common::IoErr::NotSupported:
            return true;
        default:
            return false;
    }
}

std::chrono::steady_clock::time_point make_deadline(event::EventLoop &loop,
                                                    std::chrono::milliseconds timeout) noexcept {
    if (timeout == std::chrono::milliseconds::max()) {
        return std::chrono::steady_clock::time_point::max();
    }
    return loop.now() + timeout;
}

std::chrono::milliseconds remaining_timeout(event::EventLoop &loop,
                                            std::chrono::steady_clock::time_point deadline) noexcept {
    if (deadline == std::chrono::steady_clock::time_point::max()) {
        return std::chrono::milliseconds::max();
    }
    const auto now = loop.now();
    if (now >= deadline) {
        return std::chrono::milliseconds::zero();
    }
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    if (remaining == std::chrono::milliseconds::zero()) {
        remaining = std::chrono::milliseconds(1);
    }
    return remaining;
}

common::IoResult<std::size_t> consume_stream_read(fiber::common::IoResult<std::size_t> result) noexcept {
    if (!result) {
        return std::unexpected(result.error());
    }
    if (*result == 0) {
        return std::unexpected(common::IoErr::ConnReset);
    }
    return result;
}

template<typename Awaiter>
void cancel_awaiter(void *context) noexcept {
    static_cast<Awaiter *>(context)->cancel();
}

void cancel_tcp_stream(void *context) noexcept {
    auto *stream = static_cast<net::TcpStream *>(context);
    if (stream->valid()) {
        stream->close();
    }
}

} // namespace

DnsClient::DnsClient() noexcept = default;

DnsClient::UdpTransport::UdpTransport() noexcept = default;

DnsClient::UdpTransport::~UdpTransport() = default;

DnsClient::UdpReadDispatchGuard::UdpReadDispatchGuard(DnsClient &owner) noexcept : owner_(&owner) {
    FIBER_ASSERT_MSG(owner.udp_read_dispatch_invalidated_observer_ == nullptr,
                     "DnsClient UDP read dispatch cannot re-enter");
    owner.udp_read_dispatch_invalidated_observer_ = &invalidated_;
}

DnsClient::UdpReadDispatchGuard::~UdpReadDispatchGuard() noexcept {
    if (invalidated_) {
        return;
    }
    FIBER_ASSERT(owner_ != nullptr);
    FIBER_ASSERT(owner_->udp_read_dispatch_invalidated_observer_ == &invalidated_);
    owner_->udp_read_dispatch_invalidated_observer_ = nullptr;
}

DnsClient::ResponseAwaiter::ResponseAwaiter(DnsClient &client, std::uint16_t slot_index) noexcept :
    client_(&client), slot_index_(slot_index) {}

DnsClient::ResponseAwaiter::~ResponseAwaiter() {
    if (client_ && armed_) {
        client_->cancel_waiter(slot_index_, handle_);
    }
}

bool DnsClient::ResponseAwaiter::await_ready() noexcept {
    FIBER_ASSERT(client_ != nullptr);
    return client_->response_ready(slot_index_);
}

bool DnsClient::ResponseAwaiter::await_suspend(std::coroutine_handle<> handle) noexcept {
    FIBER_ASSERT(client_ != nullptr);
    handle_ = handle;
    armed_ = client_->arm_waiter(slot_index_, handle);
    return armed_;
}

common::IoResult<std::size_t> DnsClient::ResponseAwaiter::await_resume() noexcept {
    armed_ = false;
    FIBER_ASSERT(client_ != nullptr);
    return client_->wait_result(slot_index_);
}

DnsClient::~DnsClient() {
    invalidate_udp_read_dispatch();
    if (has_open_udp_transport()) {
        FIBER_ASSERT(loop_ != nullptr);
        FIBER_ASSERT(loop_->in_loop());
        close();
    }
}

bool DnsClient::init(event::EventLoop &loop, Options options) noexcept {
    release();
    if (options.nameservers.empty() || options.max_inflight == 0 || options.max_udp_packet_size < kDnsHeaderSize ||
        options.attempts == 0) {
        return false;
    }

    bool need_v4 = false;
    bool need_v6 = false;
    for (const net::SocketAddress &server: options.nameservers.view()) {
        if (server.ip().is_unspecified() || server.port() == 0) {
            return false;
        }
        need_v4 = need_v4 || server.family() == net::IpFamily::V4;
        need_v6 = need_v6 || server.family() == net::IpFamily::V6;
    }

    net::SocketAddress bind_v4 = net::SocketAddress::any_v4();
    net::SocketAddress bind_v6 = net::SocketAddress::any_v6();
    if (!is_generic_unspecified(options.bind_addr)) {
        if (need_v4 && need_v6) {
            return false;
        }
        if ((need_v4 && options.bind_addr.family() != net::IpFamily::V4) ||
            (need_v6 && options.bind_addr.family() != net::IpFamily::V6)) {
            return false;
        }
        if (need_v4) {
            bind_v4 = options.bind_addr;
        } else {
            bind_v6 = options.bind_addr;
        }
    }
    if (options.query_options.use_edns) {
        options.query_options.max_udp_payload_size = options.max_udp_packet_size;
    }

    options_ = options;
    loop_ = &loop;
    closing_ = false;
    next_server_ = 0;

    if ((need_v4 && !init_udp_transport(net::IpFamily::V4, bind_v4)) ||
        (need_v6 && !init_udp_transport(net::IpFamily::V6, bind_v6))) {
        release();
        return false;
    }
    if (!init_storage()) {
        release();
        return false;
    }

    return true;
}

void DnsClient::close() noexcept {
    invalidate_udp_read_dispatch();
    if (!loop_) {
        return;
    }
    FIBER_ASSERT(loop_->in_loop());
    if (closing_) {
        return;
    }
    closing_ = true;
    for (UdpTransport &transport: udp_transports_) {
        if (!transport.active) {
            continue;
        }
        transport.read_callback_registered = false;
        transport.send_queue.close();
        if (transport.socket && transport.socket->valid()) {
            transport.socket->close();
        }
    }
    cancel_all_inflight(common::IoErr::Canceled);
}

void DnsClient::release() noexcept {
    invalidate_udp_read_dispatch();
    if (has_open_udp_transport()) {
        FIBER_ASSERT(loop_ != nullptr);
        FIBER_ASSERT(loop_->in_loop());
        close();
    }
    for (UdpTransport &transport: udp_transports_) {
        transport.socket.reset();
        transport.send_queue.reset();
        transport.owner = nullptr;
        transport.active = false;
        transport.read_callback_registered = false;
    }
    recv_buffer_.reset();
    request_buffers_.reset();
    id_to_slot_.reset();
    slots_.reset();
    reset_state();
}

bool DnsClient::valid() const noexcept {
    if (loop_ == nullptr || options_.nameservers.empty() || !slots_ || !id_to_slot_ || !request_buffers_ ||
        !recv_buffer_) {
        return false;
    }
    for (const net::SocketAddress &server: options_.nameservers.view()) {
        const UdpTransport &transport = udp_transport(server.family());
        if (!transport.active || !transport.socket || !transport.socket->valid()) {
            return false;
        }
    }
    return true;
}

event::EventLoop &DnsClient::loop() const noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    return *loop_;
}

async::Task<common::IoResult<std::size_t>> DnsClient::query_raw(const QuestionSpec &question, std::uint8_t *dst,
                                                                std::size_t cap) noexcept {
    return query_raw(question, dst, cap, ResponseValidator{});
}

async::Task<common::IoResult<std::size_t>> DnsClient::query_raw(const QuestionSpec &question, std::uint8_t *dst,
                                                                std::size_t cap, ResponseValidator validator) noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    if (!valid() || closing_) {
        co_return std::unexpected(common::IoErr::Canceled);
    }
    if ((dst == nullptr && cap != 0) || cap < kDnsHeaderSize) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    const common::IoErr read_callback_err = ensure_udp_read_callbacks();
    if (read_callback_err != common::IoErr::None) {
        co_return std::unexpected(read_callback_err);
    }

    const std::uint16_t slot_index = allocate_slot();
    if (slot_index == kInvalidSlot) {
        co_return std::unexpected(common::IoErr::Busy);
    }

    InflightSlot &slot = slots_[slot_index];
    slot.active = true;
    slot.response_dst = dst;
    slot.response_cap = cap;
    slot.validator = validator;

    common::IoErr final_err = common::IoErr::TimedOut;
    std::size_t last_server_error_response_size = 0;
    const std::uint8_t server_count = options_.nameservers.size();
    const std::uint8_t start_server = options_.rotate_nameservers ? next_server_ : 0;
    if (options_.rotate_nameservers) {
        next_server_ = static_cast<std::uint8_t>((next_server_ + 1U) % server_count);
    }

    bool stop = false;
    for (std::uint8_t round = 0; round < options_.attempts && !stop; ++round) {
        for (std::uint8_t offset = 0; offset < server_count; ++offset) {
            const std::uint8_t server_index = static_cast<std::uint8_t>((start_server + offset) % server_count);
            const net::SocketAddress &selected_server = options_.nameservers[server_index];
            UdpTransport &transport = udp_transport(selected_server.family());
            const auto attempt_deadline = make_deadline(*loop_, options_.timeout);

            slot.request_size = 0;
            slot.response_size = 0;
            slot.completion_err = common::IoErr::None;
            slot.completed = false;
            slot.need_tcp_fallback = false;
            slot.retry_server = false;
            slot.server_index = server_index;
            slot.waiter = {};

            auto prepared = prepare_request(slot, question);
            if (!prepared) {
                final_err = prepared.error();
                stop = true;
                break;
            }
            slot.request_size = *prepared;
            id_to_slot_[slot.id] = slot_index;

            common::IoResult<std::size_t> send_result = std::unexpected(common::IoErr::Unknown);
            detail::DnsUdpSendQueue::Owner send_owner;
            if (transport.send_queue.fast_path_available()) {
                send_result = transport.socket->try_send_to(slot.request_buf, slot.request_size, selected_server);
                if (!send_result && send_result.error() == common::IoErr::WouldBlock) {
                    send_owner = transport.send_queue.take_ownership_after_would_block();
                }
            } else {
                auto owner_result =
                        co_await async::timeout_for([&transport]() noexcept { return transport.send_queue.acquire(); },
                                                    remaining_timeout(*loop_, attempt_deadline));
                if (owner_result) {
                    send_owner = std::move(*owner_result);
                } else {
                    send_result = std::unexpected(owner_result.error());
                }
            }

            if (send_owner.owns()) {
                if (!valid() || closing_) {
                    send_result = std::unexpected(common::IoErr::Canceled);
                } else {
                    send_result =
                            co_await transport.socket->send_to(slot.request_buf, slot.request_size, selected_server,
                                                               remaining_timeout(*loop_, attempt_deadline));
                }
                send_owner.release();
            }
            if (!send_result) {
                final_err = send_result.error();
                clear_query_id(slot.id, slot_index);
                if (closing_ || is_terminal_local_error(final_err)) {
                    stop = true;
                    break;
                }
                continue;
            }

            auto wait_result = co_await async::timeout_for(ResponseAwaiter(*this, slot_index),
                                                           remaining_timeout(*loop_, attempt_deadline));
            clear_query_id(slot.id, slot_index);

            if (!wait_result) {
                final_err = wait_result.error();
                slot.waiter = {};
                slot.completed = false;
                slot.need_tcp_fallback = false;
                slot.retry_server = false;
                if (closing_ || is_terminal_local_error(final_err)) {
                    stop = true;
                    break;
                }
                continue;
            }

            if (slot.retry_server) {
                final_err = common::IoErr::Invalid;
                continue;
            }

            last_server_error_response_size = 0;
            std::size_t response_size = slot.response_size;
            if (slot.need_tcp_fallback) {
                auto tcp_result = co_await query_tcp(slot_index, server_index);
                if (!tcp_result) {
                    final_err = tcp_result.error();
                    if (closing_ || (is_terminal_local_error(final_err) && final_err != common::IoErr::Invalid)) {
                        stop = true;
                        break;
                    }
                    continue;
                }
                response_size = *tcp_result;
            }

            if (should_retry_server_error(slot.response_dst, response_size)) {
                last_server_error_response_size = response_size;
                continue;
            }

            release_slot(slot_index);
            co_return response_size;
        }
    }

    if (last_server_error_response_size != 0 && !stop && !closing_) {
        release_slot(slot_index);
        co_return last_server_error_response_size;
    }
    release_slot(slot_index);
    co_return std::unexpected(final_err);
}

bool DnsClient::init_udp_transport(net::IpFamily family, const net::SocketAddress &bind_addr) noexcept {
    UdpTransport &transport = udp_transport(family);
    FIBER_ASSERT(!transport.active);
    FIBER_ASSERT(!transport.socket);
    transport.owner = this;
    transport.family = family;
    transport.active = true;
    transport.read_callback_registered = false;
    transport.send_queue.init(*loop_);
    transport.socket = std::make_unique<net::UdpSocket>(*loop_);
    if (!transport.socket) {
        return false;
    }
    net::UdpBindOptions bind_options{};
    bind_options.v6_only = family == net::IpFamily::V6;
    return transport.socket->bind(bind_addr, bind_options).has_value();
}

common::IoErr DnsClient::ensure_udp_read_callbacks() noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    for (UdpTransport &transport: udp_transports_) {
        if (!transport.active || transport.read_callback_registered) {
            continue;
        }
        FIBER_ASSERT(transport.socket && transport.socket->valid());
        const common::IoErr err = transport.socket->set_read_callback(&DnsClient::on_udp_readable, &transport);
        if (err != common::IoErr::None) {
            return err;
        }
        transport.read_callback_registered = true;
    }
    return common::IoErr::None;
}

void DnsClient::drain_udp_reads(UdpTransport &transport) noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(transport.owner == this);
    FIBER_ASSERT(transport.socket && transport.socket->valid());
    UdpReadDispatchGuard dispatch(*this);

    for (;;) {
        auto recv_result = transport.socket->try_recv_from(recv_buffer_.get(), options_.max_udp_packet_size);
        if (!recv_result) {
            return;
        }
        handle_udp_packet(recv_buffer_.get(), recv_result->size, recv_result->peer);
        if (dispatch.invalidated()) {
            return;
        }
    }
}

void DnsClient::invalidate_udp_read_dispatch() noexcept {
    if (!udp_read_dispatch_invalidated_observer_) {
        return;
    }
    *udp_read_dispatch_invalidated_observer_ = true;
    udp_read_dispatch_invalidated_observer_ = nullptr;
}

void DnsClient::on_udp_readable(void *ctx, common::IoErr err) noexcept {
    if (err != common::IoErr::None) {
        return;
    }
    auto *transport = static_cast<UdpTransport *>(ctx);
    FIBER_ASSERT(transport != nullptr);
    FIBER_ASSERT(transport->owner != nullptr);
    transport->owner->drain_udp_reads(*transport);
}

async::Task<common::IoResult<std::size_t>> DnsClient::query_tcp(std::uint16_t slot_index,
                                                                std::uint8_t server_index) noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(slot_index < options_.max_inflight);
    FIBER_ASSERT(server_index < options_.nameservers.size());
    InflightSlot &slot = slots_[slot_index];
    const net::SocketAddress &selected_server = options_.nameservers[server_index];
    const auto deadline = make_deadline(*loop_, options_.timeout);

    auto connect_operation = net::TcpStream::connect(*loop_, selected_server, remaining_timeout(*loop_, deadline));
    arm_inflight_cancel(slot_index, &connect_operation, &cancel_awaiter<decltype(connect_operation)>);
    auto connect_result = co_await connect_operation;
    disarm_inflight_cancel(slot_index, &connect_operation);
    if (!connect_result) {
        co_return std::unexpected(connect_result.error());
    }

    net::TcpStream stream(std::move(*connect_result));
    arm_inflight_cancel(slot_index, &stream, &cancel_tcp_stream);
    auto close_stream = [this, slot_index, &stream]() noexcept {
        disarm_inflight_cancel(slot_index, &stream);
        if (stream.valid()) {
            stream.close();
        }
    };
    std::array<std::uint8_t, 2> len_prefix{};
    write_be16(len_prefix.data(), static_cast<std::uint16_t>(slot.request_size));

    std::size_t prefix_written = 0;
    while (prefix_written < len_prefix.size()) {
        auto write_prefix =
                co_await stream.write(len_prefix.data() + prefix_written, len_prefix.size() - prefix_written,
                                      remaining_timeout(*loop_, deadline));
        if (!write_prefix) {
            close_stream();
            co_return std::unexpected(write_prefix.error());
        }
        if (*write_prefix == 0) {
            close_stream();
            co_return std::unexpected(common::IoErr::ConnReset);
        }
        prefix_written += *write_prefix;
    }

    std::size_t written = 0;
    while (written < slot.request_size) {
        auto write_result = co_await stream.write(slot.request_buf + written, slot.request_size - written,
                                                  remaining_timeout(*loop_, deadline));
        if (!write_result) {
            close_stream();
            co_return std::unexpected(write_result.error());
        }
        if (*write_result == 0) {
            close_stream();
            co_return std::unexpected(common::IoErr::ConnReset);
        }
        written += *write_result;
    }

    auto read_prefix = co_await stream.read(len_prefix.data(), len_prefix.size(), remaining_timeout(*loop_, deadline));
    auto prefix_result = consume_stream_read(read_prefix);
    if (!prefix_result) {
        close_stream();
        co_return std::unexpected(prefix_result.error());
    }
    std::size_t prefix_read = *prefix_result;
    while (prefix_read < len_prefix.size()) {
        auto more = co_await stream.read(len_prefix.data() + prefix_read, len_prefix.size() - prefix_read,
                                         remaining_timeout(*loop_, deadline));
        auto more_result = consume_stream_read(more);
        if (!more_result) {
            close_stream();
            co_return std::unexpected(more_result.error());
        }
        prefix_read += *more_result;
    }

    const std::size_t response_len = read_be16(len_prefix.data());
    if (response_len > slot.response_cap) {
        close_stream();
        co_return std::unexpected(common::IoErr::NoMem);
    }

    std::size_t total_read = 0;
    while (total_read < response_len) {
        auto read_result = co_await stream.read(slot.response_dst + total_read, response_len - total_read,
                                                remaining_timeout(*loop_, deadline));
        auto payload_result = consume_stream_read(read_result);
        if (!payload_result) {
            close_stream();
            co_return std::unexpected(payload_result.error());
        }
        total_read += *payload_result;
    }

    if (!detail::response_matches_query(slot.request_buf, slot.request_size, slot.response_dst, response_len,
                                        options_.enable_0x20)) {
        close_stream();
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (validate_response(slot, slot.response_dst, response_len) == ResponseDisposition::RetryServer) {
        close_stream();
        co_return std::unexpected(common::IoErr::Invalid);
    }
    close_stream();
    co_return response_len;
}

bool DnsClient::init_storage() noexcept {
    slots_ = std::make_unique<InflightSlot[]>(options_.max_inflight);
    id_to_slot_ = std::make_unique<std::uint16_t[]>(std::numeric_limits<std::uint16_t>::max() + 1u);
    request_buffers_ = std::make_unique<std::uint8_t[]>(static_cast<std::size_t>(options_.max_inflight) *
                                                        options_.max_udp_packet_size);
    recv_buffer_ = std::make_unique<std::uint8_t[]>(options_.max_udp_packet_size);
    if (!slots_ || !id_to_slot_ || !request_buffers_ || !recv_buffer_) {
        return false;
    }

    for (std::size_t i = 0; i < options_.max_inflight; ++i) {
        InflightSlot &slot = slots_[i];
        slot = {};
        slot.request_buf = request_buffers_.get() + (i * options_.max_udp_packet_size);
        slot.next_free = i + 1 < options_.max_inflight ? static_cast<std::uint16_t>(i + 1) : kInvalidSlot;
    }
    for (std::size_t i = 0; i <= std::numeric_limits<std::uint16_t>::max(); ++i) {
        id_to_slot_[i] = kInvalidIdMapping;
    }
    free_head_ = 0;
    return true;
}

common::IoResult<std::size_t> DnsClient::prepare_request(InflightSlot &slot, const QuestionSpec &question) noexcept {
    constexpr std::size_t kIdRandomBytes = 4;
    std::array<std::uint8_t, kIdRandomBytes + detail::kDns0x20RandomBytes> random{};
    const std::size_t random_size = options_.enable_0x20 ? random.size() : kIdRandomBytes;
    if (RAND_bytes(random.data(), random_size) != 1) {
        return std::unexpected(common::IoErr::Unknown);
    }

    auto id_result = allocate_query_id(read_be16(random.data()), read_be16(random.data() + 2));
    if (!id_result) {
        return std::unexpected(id_result.error());
    }
    slot.id = *id_result;

    QueryOptions query_options = options_.query_options;
    query_options.id = slot.id;
    auto encoded = encode_query(query_options, question, slot.request_buf, options_.max_udp_packet_size);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    if (!options_.enable_0x20) {
        return *encoded;
    }

    const auto case_random = std::span<const std::uint8_t, detail::kDns0x20RandomBytes>(random.data() + kIdRandomBytes,
                                                                                        detail::kDns0x20RandomBytes);
    const common::IoErr randomize_err = detail::apply_query_name_0x20(slot.request_buf, *encoded, case_random);
    if (randomize_err != common::IoErr::None) {
        return std::unexpected(randomize_err);
    }
    return *encoded;
}

void DnsClient::reset_state() noexcept {
    FIBER_ASSERT(udp_read_dispatch_invalidated_observer_ == nullptr);
    loop_ = nullptr;
    options_ = {};
    free_head_ = kInvalidSlot;
    next_server_ = 0;
    closing_ = false;
}

void DnsClient::cancel_all_inflight(common::IoErr err) noexcept {
    if (!slots_) {
        return;
    }
    struct CancelAction {
        void *context = nullptr;
        InflightCancelFn cancel = nullptr;
        std::coroutine_handle<> waiter{};
    };

    auto actions = std::make_unique<CancelAction[]>(options_.max_inflight);
    std::size_t action_count = 0;
    for (std::size_t i = 0; i < options_.max_inflight; ++i) {
        InflightSlot &slot = slots_[i];
        if (!slot.active) {
            continue;
        }
        clear_query_id(slot.id, static_cast<std::uint16_t>(i));
        slot.completion_err = err;
        slot.response_size = 0;
        slot.need_tcp_fallback = false;
        slot.retry_server = false;
        slot.completed = true;
        FIBER_ASSERT(slot.cancel == nullptr || !slot.waiter);
        if (slot.cancel) {
            actions[action_count].context = slot.cancel_context;
            actions[action_count].cancel = slot.cancel;
            slot.cancel_context = nullptr;
            slot.cancel = nullptr;
            ++action_count;
        } else if (slot.waiter) {
            actions[action_count++].waiter = slot.waiter;
            slot.waiter = {};
        }
    }
    for (std::size_t i = 0; i < action_count; ++i) {
        if (actions[i].cancel) {
            actions[i].cancel(actions[i].context);
        } else if (actions[i].waiter) {
            actions[i].waiter.resume();
        }
    }
}

std::uint16_t DnsClient::allocate_slot() noexcept {
    if (free_head_ == kInvalidSlot) {
        return kInvalidSlot;
    }
    const std::uint16_t slot_index = free_head_;
    free_head_ = slots_[slot_index].next_free;
    slots_[slot_index].next_free = kInvalidSlot;
    return slot_index;
}

void DnsClient::release_slot(std::uint16_t slot_index) noexcept {
    FIBER_ASSERT(slot_index < options_.max_inflight);
    InflightSlot &slot = slots_[slot_index];
    slot.active = false;
    slot.completed = false;
    slot.need_tcp_fallback = false;
    slot.retry_server = false;
    slot.server_index = 0;
    slot.id = 0;
    slot.waiter = {};
    slot.request_size = 0;
    slot.response_dst = nullptr;
    slot.response_cap = 0;
    slot.response_size = 0;
    slot.completion_err = common::IoErr::None;
    slot.validator = {};
    slot.cancel_context = nullptr;
    slot.cancel = nullptr;
    slot.next_free = free_head_;
    free_head_ = slot_index;
}

common::IoResult<std::uint16_t> DnsClient::allocate_query_id(std::uint16_t start, std::uint16_t stride) noexcept {
    return detail::select_query_id(id_to_slot_.get(), kInvalidIdMapping, start, stride);
}

void DnsClient::clear_query_id(std::uint16_t id, std::uint16_t slot_index) noexcept {
    if (!id_to_slot_) {
        return;
    }
    if (id_to_slot_[id] == slot_index) {
        id_to_slot_[id] = kInvalidIdMapping;
    }
}

bool DnsClient::response_ready(std::uint16_t slot_index) const noexcept {
    FIBER_ASSERT(slot_index < options_.max_inflight);
    return slots_[slot_index].completed;
}

bool DnsClient::arm_waiter(std::uint16_t slot_index, std::coroutine_handle<> handle) noexcept {
    FIBER_ASSERT(slot_index < options_.max_inflight);
    InflightSlot &slot = slots_[slot_index];
    if (slot.completed) {
        return false;
    }
    FIBER_ASSERT(!slot.waiter);
    slot.waiter = handle;
    return true;
}

void DnsClient::cancel_waiter(std::uint16_t slot_index, std::coroutine_handle<> handle) noexcept {
    if (!slots_ || slot_index >= options_.max_inflight) {
        return;
    }
    InflightSlot &slot = slots_[slot_index];
    if (slot.waiter == handle) {
        slot.waiter = {};
    }
}

common::IoResult<std::size_t> DnsClient::wait_result(std::uint16_t slot_index) noexcept {
    FIBER_ASSERT(slot_index < options_.max_inflight);
    InflightSlot &slot = slots_[slot_index];
    slot.waiter = {};
    slot.completed = false;
    if (slot.completion_err != common::IoErr::None) {
        return std::unexpected(slot.completion_err);
    }
    return slot.response_size;
}

void DnsClient::arm_inflight_cancel(std::uint16_t slot_index, void *context, InflightCancelFn cancel) noexcept {
    FIBER_ASSERT(slot_index < options_.max_inflight);
    FIBER_ASSERT(context != nullptr);
    FIBER_ASSERT(cancel != nullptr);
    InflightSlot &slot = slots_[slot_index];
    FIBER_ASSERT(slot.active);
    FIBER_ASSERT(slot.cancel_context == nullptr);
    FIBER_ASSERT(slot.cancel == nullptr);
    slot.cancel_context = context;
    slot.cancel = cancel;
}

void DnsClient::disarm_inflight_cancel(std::uint16_t slot_index, void *context) noexcept {
    FIBER_ASSERT(slot_index < options_.max_inflight);
    InflightSlot &slot = slots_[slot_index];
    if (slot.cancel_context != context) {
        return;
    }
    slot.cancel_context = nullptr;
    slot.cancel = nullptr;
}

void DnsClient::complete_slot(std::uint16_t slot_index, common::IoErr err, std::size_t response_size,
                              bool need_tcp_fallback, bool retry_server) noexcept {
    FIBER_ASSERT(slot_index < options_.max_inflight);
    InflightSlot &slot = slots_[slot_index];
    slot.completion_err = err;
    slot.response_size = response_size;
    slot.need_tcp_fallback = need_tcp_fallback;
    slot.retry_server = retry_server;
    slot.completed = true;
    auto handle = slot.waiter;
    slot.waiter = {};
    if (handle) {
        handle.resume();
    }
}

void DnsClient::handle_udp_packet(const std::uint8_t *packet, std::size_t packet_len,
                                  const net::SocketAddress &peer) noexcept {
    if (closing_ || packet_len < kDnsHeaderSize) {
        return;
    }

    const std::uint16_t id = read_be16(packet);
    const std::uint16_t slot_index = id_to_slot_[id];
    if (slot_index == kInvalidIdMapping || slot_index >= options_.max_inflight) {
        return;
    }

    InflightSlot &slot = slots_[slot_index];
    if (!slot.active || slot.id != id || slot.server_index >= options_.nameservers.size() ||
        !socket_address_equal(peer, options_.nameservers[slot.server_index])) {
        return;
    }
    if (!detail::response_matches_query(slot.request_buf, slot.request_size, packet, packet_len,
                                        options_.enable_0x20)) {
        return;
    }
    if (packet_len > slot.response_cap) {
        complete_slot(slot_index, common::IoErr::NoMem, 0, false);
        return;
    }

    const std::uint16_t flags = read_be16(packet + 2);
    const bool truncated = (flags & 0x0200U) != 0;
    if (!truncated && validate_response(slot, packet, packet_len) == ResponseDisposition::RetryServer) {
        complete_slot(slot_index, common::IoErr::None, 0, false, true);
        return;
    }

    std::memcpy(slot.response_dst, packet, packet_len);
    complete_slot(slot_index, common::IoErr::None, packet_len, truncated && options_.enable_tcp_fallback);
}

DnsClient::ResponseDisposition DnsClient::validate_response(const InflightSlot &slot, const std::uint8_t *packet,
                                                            std::size_t packet_len) const noexcept {
    if (slot.validator.validate == nullptr) {
        return ResponseDisposition::Accept;
    }
    return slot.validator.validate(slot.validator.context, packet, packet_len);
}

bool DnsClient::should_retry_server_error(const std::uint8_t *packet, std::size_t packet_len) const noexcept {
    if (!options_.retry_server_errors || packet == nullptr || packet_len < kDnsHeaderSize) {
        return false;
    }
    switch (read_be16(packet + 2) & 0x000fU) {
        case 1: // FORMERR
        case 2: // SERVFAIL
        case 4: // NOTIMP
        case 5: // REFUSED
            return true;
        default:
            return false;
    }
}

bool DnsClient::has_open_udp_transport() const noexcept {
    return std::ranges::any_of(udp_transports_, [](const UdpTransport &transport) noexcept {
        return transport.socket && transport.socket->valid();
    });
}

DnsClient::UdpTransport &DnsClient::udp_transport(net::IpFamily family) noexcept {
    return udp_transports_[udp_transport_index(family)];
}

const DnsClient::UdpTransport &DnsClient::udp_transport(net::IpFamily family) const noexcept {
    return udp_transports_[udp_transport_index(family)];
}

} // namespace fiber::dns
