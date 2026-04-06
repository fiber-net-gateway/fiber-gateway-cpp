#include "DnsClient.h"

#include "DnsMessage.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <expected>
#include <limits>
#include <utility>

#include "../async/Spawn.h"
#include "../async/Timeout.h"
#include "../common/Assert.h"
#include "../net/TcpStream.h"
#include "../net/UdpSocket.h"

namespace fiber::dns {

namespace {

constexpr std::size_t kDnsHeaderSize = 12;

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

common::IoResult<std::size_t> consume_stream_read(fiber::common::IoResult<std::size_t> result) noexcept {
    if (!result) {
        return std::unexpected(result.error());
    }
    if (*result == 0) {
        return std::unexpected(common::IoErr::ConnReset);
    }
    return result;
}

} // namespace

DnsClient::ResponseAwaiter::ResponseAwaiter(DnsClient &client, std::uint16_t slot_index) noexcept :
    client_(&client), slot_index_(slot_index) {
}

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
    if (socket_ && socket_->valid()) {
        FIBER_ASSERT(loop_ != nullptr);
        FIBER_ASSERT(loop_->in_loop());
        close();
    }
}

bool DnsClient::init(event::EventLoop &loop, Options options) noexcept {
    release();
    if (options.server.ip().is_unspecified() || options.server.port() == 0 || options.max_inflight == 0 ||
        options.max_udp_packet_size < kDnsHeaderSize || options.attempts == 0) {
        return false;
    }

    if (is_generic_unspecified(options.bind_addr)) {
        options.bind_addr = options.server.family() == net::IpFamily::V4 ? net::SocketAddress::any_v4()
                                                                         : net::SocketAddress::any_v6();
    }
    if (options.bind_addr.family() != options.server.family()) {
        return false;
    }
    if (options.query_options.use_edns) {
        options.query_options.max_udp_payload_size = options.max_udp_packet_size;
    }

    options_ = options;
    loop_ = &loop;
    closing_ = false;
    next_id_ = 0;

    socket_ = std::make_unique<net::UdpSocket>(loop);
    if (!socket_) {
        release();
        return false;
    }
    net::UdpBindOptions bind_options{};
    bind_options.v6_only = options.server.family() == net::IpFamily::V6;
    auto bind_result = socket_->bind(options.bind_addr, bind_options);
    if (!bind_result) {
        release();
        return false;
    }
    if (!init_storage()) {
        release();
        return false;
    }

    async::spawn(loop, [this]() -> async::DetachedTask { co_await recv_loop(); });
    return true;
}

void DnsClient::close() noexcept {
    if (!loop_) {
        return;
    }
    FIBER_ASSERT(loop_->in_loop());
    if (closing_) {
        return;
    }
    closing_ = true;
    if (socket_ && socket_->valid()) {
        socket_->close();
    }
    cancel_all_inflight(common::IoErr::Canceled);
}

void DnsClient::release() noexcept {
    if (socket_ && socket_->valid()) {
        FIBER_ASSERT(loop_ != nullptr);
        FIBER_ASSERT(loop_->in_loop());
        close();
    }
    socket_.reset();
    recv_buffer_.reset();
    request_buffers_.reset();
    id_to_slot_.reset();
    slots_.reset();
    reset_state();
}

bool DnsClient::valid() const noexcept {
    return socket_ && socket_->valid();
}

event::EventLoop &DnsClient::loop() const noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    return *loop_;
}

const net::SocketAddress &DnsClient::server() const noexcept {
    return options_.server;
}

async::Task<common::IoResult<std::size_t>> DnsClient::query_raw(const QuestionSpec &question,
                                                                std::uint8_t *dst,
                                                                std::size_t cap) noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    if (!valid() || closing_) {
        co_return std::unexpected(common::IoErr::Canceled);
    }
    if ((dst == nullptr && cap != 0) || cap < kDnsHeaderSize) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    const std::uint16_t slot_index = allocate_slot();
    if (slot_index == kInvalidSlot) {
        co_return std::unexpected(common::IoErr::Busy);
    }

    InflightSlot &slot = slots_[slot_index];
    slot.active = true;
    slot.response_dst = dst;
    slot.response_cap = cap;

    common::IoErr final_err = common::IoErr::TimedOut;
    for (std::uint8_t attempt = 0; attempt < options_.attempts; ++attempt) {
        auto id_result = allocate_query_id();
        if (!id_result) {
            final_err = id_result.error();
            break;
        }

        slot.id = *id_result;
        slot.request_size = 0;
        slot.response_size = 0;
        slot.completion_err = common::IoErr::None;
        slot.completed = false;
        slot.need_tcp_fallback = false;
        slot.waiter = {};

        QueryOptions query_options = options_.query_options;
        query_options.id = slot.id;
        auto encoded = encode_query(query_options, question, slot.request_buf, options_.max_udp_packet_size);
        if (!encoded) {
            final_err = encoded.error();
            clear_query_id(slot.id, slot_index);
            break;
        }
        slot.request_size = *encoded;
        id_to_slot_[slot.id] = slot_index;

        auto send_result = co_await socket_->send_to(slot.request_buf, slot.request_size, options_.server);
        if (!send_result) {
            final_err = send_result.error();
            clear_query_id(slot.id, slot_index);
            break;
        }

        auto wait_result = co_await async::timeout_for(ResponseAwaiter(*this, slot_index), options_.timeout);
        clear_query_id(slot.id, slot_index);

        if (!wait_result) {
            final_err = wait_result.error();
            slot.waiter = {};
            slot.completed = false;
            slot.need_tcp_fallback = false;
            if (final_err == common::IoErr::TimedOut && !closing_ && attempt + 1 < options_.attempts) {
                continue;
            }
            break;
        }

        if (slot.need_tcp_fallback) {
            auto tcp_result = co_await query_tcp(slot);
            release_slot(slot_index);
            co_return tcp_result;
        }

        if (slot.completion_err != common::IoErr::None) {
            final_err = slot.completion_err;
            break;
        }

        const std::size_t response_size = slot.response_size;
        release_slot(slot_index);
        co_return response_size;
    }

    release_slot(slot_index);
    co_return std::unexpected(final_err);
}

async::Task<void> DnsClient::recv_loop() noexcept {
    while (socket_ && socket_->valid()) {
        auto recv_result = co_await socket_->recv_from(recv_buffer_.get(), options_.max_udp_packet_size);
        if (!recv_result) {
            if (recv_result.error() == common::IoErr::Canceled || recv_result.error() == common::IoErr::BadFd) {
                break;
            }
            continue;
        }
        handle_udp_packet(recv_buffer_.get(), recv_result->size, recv_result->peer);
    }
    co_return;
}

async::Task<common::IoResult<std::size_t>> DnsClient::query_tcp(const InflightSlot &slot) noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    auto connect_result = co_await async::timeout_for([this]() { return net::TcpStream::connect(*loop_, options_.server); },
                                                      options_.timeout);
    if (!connect_result) {
        co_return std::unexpected(connect_result.error());
    }

    net::TcpStream stream(std::move(*connect_result));
    std::array<std::uint8_t, 2> len_prefix{};
    write_be16(len_prefix.data(), static_cast<std::uint16_t>(slot.request_size));

    std::size_t prefix_written = 0;
    while (prefix_written < len_prefix.size()) {
        auto write_prefix = co_await async::timeout_for(
            [&]() { return stream.write(len_prefix.data() + prefix_written, len_prefix.size() - prefix_written); },
            options_.timeout);
        if (!write_prefix) {
            stream.close();
            co_return std::unexpected(write_prefix.error());
        }
        if (*write_prefix == 0) {
            stream.close();
            co_return std::unexpected(common::IoErr::ConnReset);
        }
        prefix_written += *write_prefix;
    }

    std::size_t written = 0;
    while (written < slot.request_size) {
        auto write_result = co_await async::timeout_for(
            [&]() { return stream.write(slot.request_buf + written, slot.request_size - written); }, options_.timeout);
        if (!write_result) {
            stream.close();
            co_return std::unexpected(write_result.error());
        }
        if (*write_result == 0) {
            stream.close();
            co_return std::unexpected(common::IoErr::ConnReset);
        }
        written += *write_result;
    }

    auto read_prefix = co_await async::timeout_for([&]() { return stream.read(len_prefix.data(), len_prefix.size()); },
                                                   options_.timeout);
    auto prefix_result = consume_stream_read(read_prefix);
    if (!prefix_result) {
        stream.close();
        co_return std::unexpected(prefix_result.error());
    }
    std::size_t prefix_read = *prefix_result;
    while (prefix_read < len_prefix.size()) {
        auto more = co_await async::timeout_for(
            [&]() { return stream.read(len_prefix.data() + prefix_read, len_prefix.size() - prefix_read); },
            options_.timeout);
        auto more_result = consume_stream_read(more);
        if (!more_result) {
            stream.close();
            co_return std::unexpected(more_result.error());
        }
        prefix_read += *more_result;
    }

    const std::size_t response_len = read_be16(len_prefix.data());
    if (response_len > slot.response_cap) {
        stream.close();
        co_return std::unexpected(common::IoErr::NoMem);
    }

    std::size_t total_read = 0;
    while (total_read < response_len) {
        auto read_result = co_await async::timeout_for(
            [&]() { return stream.read(slot.response_dst + total_read, response_len - total_read); }, options_.timeout);
        auto payload_result = consume_stream_read(read_result);
        if (!payload_result) {
            stream.close();
            co_return std::unexpected(payload_result.error());
        }
        total_read += *payload_result;
    }

    stream.close();
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

void DnsClient::reset_state() noexcept {
    loop_ = nullptr;
    options_ = {};
    free_head_ = kInvalidSlot;
    next_id_ = 0;
    closing_ = false;
}

void DnsClient::cancel_all_inflight(common::IoErr err) noexcept {
    if (!slots_) {
        return;
    }
    auto resumes = std::make_unique<std::coroutine_handle<>[]>(options_.max_inflight);
    std::size_t resume_count = 0;
    for (std::size_t i = 0; i < options_.max_inflight; ++i) {
        InflightSlot &slot = slots_[i];
        if (!slot.active) {
            continue;
        }
        clear_query_id(slot.id, static_cast<std::uint16_t>(i));
        slot.completion_err = err;
        slot.response_size = 0;
        slot.need_tcp_fallback = false;
        slot.completed = true;
        if (slot.waiter) {
            resumes[resume_count++] = slot.waiter;
            slot.waiter = {};
        }
    }
    for (std::size_t i = 0; i < resume_count; ++i) {
        if (resumes[i]) {
            resumes[i].resume();
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
    slot.id = 0;
    slot.waiter = {};
    slot.request_size = 0;
    slot.response_dst = nullptr;
    slot.response_cap = 0;
    slot.response_size = 0;
    slot.completion_err = common::IoErr::None;
    slot.next_free = free_head_;
    free_head_ = slot_index;
}

common::IoResult<std::uint16_t> DnsClient::allocate_query_id() noexcept {
    for (std::size_t i = 0; i <= std::numeric_limits<std::uint16_t>::max(); ++i) {
        const std::uint16_t candidate = next_id_++;
        if (id_to_slot_[candidate] == kInvalidIdMapping) {
            return candidate;
        }
    }
    return std::unexpected(common::IoErr::Busy);
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

void DnsClient::complete_slot(std::uint16_t slot_index,
                              common::IoErr err,
                              std::size_t response_size,
                              bool need_tcp_fallback) noexcept {
    FIBER_ASSERT(slot_index < options_.max_inflight);
    InflightSlot &slot = slots_[slot_index];
    slot.completion_err = err;
    slot.response_size = response_size;
    slot.need_tcp_fallback = need_tcp_fallback;
    slot.completed = true;
    auto handle = slot.waiter;
    slot.waiter = {};
    if (handle) {
        handle.resume();
    }
}

void DnsClient::handle_udp_packet(const std::uint8_t *packet,
                                  std::size_t packet_len,
                                  const net::SocketAddress &peer) noexcept {
    if (closing_ || packet_len < kDnsHeaderSize || !socket_address_equal(peer, options_.server)) {
        return;
    }

    const std::uint16_t id = read_be16(packet);
    const std::uint16_t flags = read_be16(packet + 2);
    if ((flags & 0x8000U) == 0) {
        return;
    }
    const std::uint16_t slot_index = id_to_slot_[id];
    if (slot_index == kInvalidIdMapping || slot_index >= options_.max_inflight) {
        return;
    }

    InflightSlot &slot = slots_[slot_index];
    if (!slot.active || slot.id != id) {
        return;
    }
    if (packet_len > slot.response_cap) {
        complete_slot(slot_index, common::IoErr::NoMem, 0, false);
        return;
    }

    std::memcpy(slot.response_dst, packet, packet_len);
    const bool truncated = (flags & 0x0200U) != 0;
    complete_slot(slot_index, common::IoErr::None, packet_len, truncated && options_.enable_tcp_fallback);
}

} // namespace fiber::dns
