#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <string>
#include <vector>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/Timeout.h>
#include <fiber/common/IoError.h>
#include <fiber/dns/DnsCache2.h>
#include <fiber/dns/DnsResolver.h>
#include <fiber/dns/DnsResolverLocal.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/net/UdpSocket.h>

namespace {

using namespace std::chrono_literals;
using fiber::async::DetachedTask;
using fiber::common::IoErr;
using fiber::dns::AddressPolicy;
using fiber::dns::AddressResolver;
using fiber::dns::AddressResolveResult;
using fiber::dns::DnsClient;
using fiber::dns::DnsResolver;
using fiber::dns::DnsResolverLocal;
using fiber::dns::RecordClass;
using fiber::dns::RecordType;
using fiber::dns::ResolveStatus;

struct ServerOutcome {
    IoErr err = IoErr::Unknown;
    std::size_t recv_count = 0;
};

struct AddressOutcome {
    IoErr err = IoErr::Unknown;
    ResolveStatus status = ResolveStatus::ServerFailure;
    std::string canonical;
    std::vector<std::string> records;
};

struct DualAddressOutcome {
    AddressOutcome first{};
    AddressOutcome second{};
};

struct EndpointOutcome {
    IoErr err = IoErr::Unknown;
    ResolveStatus status = ResolveStatus::ServerFailure;
    std::string canonical;
    std::vector<std::string> endpoints;
};

void shutdown_cache(fiber::event::EventLoop &loop, fiber::dns::SharedDnsCache2 &cache) {
    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(loop, [&]() -> DetachedTask {
        cache.shutdown();
        done.set_value();
        co_return;
    });
    future.get();
}

fiber::common::IoResult<std::uint16_t> resolve_port(int fd) {
    sockaddr_storage bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    fiber::net::SocketAddress local;
    if (!fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<sockaddr *>(&bound), len, local)) {
        return std::unexpected(IoErr::NotSupported);
    }
    return local.port();
}

std::vector<std::uint8_t> encode_dns_name(std::string_view name) {
    std::vector<std::uint8_t> out;
    std::size_t start = 0;
    while (start < name.size()) {
        std::size_t dot = name.find('.', start);
        if (dot == std::string_view::npos) {
            dot = name.size();
        }
        const std::size_t label_len = dot - start;
        out.push_back(static_cast<std::uint8_t>(label_len));
        for (std::size_t i = start; i < dot; ++i) {
            out.push_back(static_cast<std::uint8_t>(name[i]));
        }
        start = dot + 1;
    }
    out.push_back(0);
    return out;
}

void push_be16(std::vector<std::uint8_t> &out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
    out.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void push_be32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

std::uint16_t read_be16(const std::uint8_t *data) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8U) | data[1]);
}

bool echo_question_name(const std::uint8_t *query, std::size_t query_len, std::vector<std::uint8_t> &response) {
    const auto name_end = [](const std::uint8_t *packet, std::size_t packet_len) -> std::size_t {
        if (packet == nullptr || packet_len < 12) {
            return 0;
        }
        std::size_t offset = 12;
        while (offset < packet_len) {
            const std::uint8_t label_len = packet[offset++];
            if (label_len == 0) {
                return offset;
            }
            if (label_len > 63 || static_cast<std::size_t>(label_len) > packet_len - offset) {
                return 0;
            }
            offset += label_len;
        }
        return 0;
    };

    const std::size_t query_name_end = name_end(query, query_len);
    const std::size_t response_name_end = name_end(response.data(), response.size());
    if (query_name_end == 0 || response_name_end == 0 || query_name_end != response_name_end) {
        return false;
    }
    std::memcpy(response.data() + 12, query + 12, query_name_end - 12);
    return true;
}

fiber::common::IoResult<std::uint16_t> parse_question_type(const std::uint8_t *packet, std::size_t packet_len) {
    if (packet_len < 12) {
        return std::unexpected(IoErr::Invalid);
    }
    std::size_t offset = 12;
    while (offset < packet_len) {
        const std::uint8_t label_len = packet[offset++];
        if (label_len == 0) {
            break;
        }
        if (offset + label_len > packet_len) {
            return std::unexpected(IoErr::Invalid);
        }
        offset += label_len;
    }
    if (offset + 4 > packet_len) {
        return std::unexpected(IoErr::Invalid);
    }
    return read_be16(packet + offset);
}

std::vector<std::uint8_t> make_multi_address_response(std::uint16_t id, std::string_view qname, std::uint16_t qtype,
                                                      const std::uint8_t *const *rdata_records,
                                                      const std::uint16_t *rdata_lengths, std::size_t record_count,
                                                      std::uint32_t ttl) {
    std::vector<std::uint8_t> packet;
    push_be16(packet, id);
    push_be16(packet, 0x8180U);
    push_be16(packet, 1);
    push_be16(packet, static_cast<std::uint16_t>(record_count));
    push_be16(packet, 0);
    push_be16(packet, 0);

    auto qname_wire = encode_dns_name(qname);
    packet.insert(packet.end(), qname_wire.begin(), qname_wire.end());
    push_be16(packet, qtype);
    push_be16(packet, static_cast<std::uint16_t>(RecordClass::IN));

    for (std::size_t i = 0; i < record_count; ++i) {
        push_be16(packet, 0xc00cU);
        push_be16(packet, qtype);
        push_be16(packet, static_cast<std::uint16_t>(RecordClass::IN));
        push_be32(packet, ttl);
        push_be16(packet, rdata_lengths[i]);
        packet.insert(packet.end(), rdata_records[i], rdata_records[i] + rdata_lengths[i]);
    }
    return packet;
}

std::vector<std::uint8_t> make_address_response(std::uint16_t id, std::string_view qname, std::uint16_t qtype,
                                                const std::uint8_t *rdata, std::uint16_t rdata_len, std::uint32_t ttl) {
    return make_multi_address_response(id, qname, qtype, &rdata, &rdata_len, 1, ttl);
}

std::vector<std::uint8_t> make_empty_response(std::uint16_t id, std::string_view qname, std::uint16_t qtype) {
    std::vector<std::uint8_t> packet;
    push_be16(packet, id);
    push_be16(packet, 0x8180U);
    push_be16(packet, 1);
    push_be16(packet, 0);
    push_be16(packet, 0);
    push_be16(packet, 0);

    auto qname_wire = encode_dns_name(qname);
    packet.insert(packet.end(), qname_wire.begin(), qname_wire.end());
    push_be16(packet, qtype);
    push_be16(packet, static_cast<std::uint16_t>(RecordClass::IN));
    return packet;
}

DetachedTask run_dual_stack_server(fiber::event::EventLoop *loop, std::promise<std::uint16_t> *port_promise,
                                   std::promise<ServerOutcome> *outcome_promise, std::string qname,
                                   std::array<std::uint8_t, 4> v4_addr, std::array<std::uint8_t, 16> v6_addr,
                                   bool return_aaaa_answer, std::size_t expected_queries = 2) {
    ServerOutcome outcome;
    fiber::net::UdpSocket socket(*loop);
    auto bind_result = socket.bind(fiber::net::SocketAddress::any_v4(), {});
    if (!bind_result) {
        port_promise->set_value(0);
        outcome.err = bind_result.error();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }

    auto port_result = resolve_port(socket.fd());
    port_promise->set_value(port_result ? *port_result : 0);
    if (!port_result) {
        outcome.err = port_result.error();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }

    std::array<std::uint8_t, 512> buf{};
    for (std::size_t i = 0; i < expected_queries; ++i) {
        auto recv_result = co_await socket.recv_from(buf.data(), buf.size(), 2s);
        if (!recv_result) {
            outcome.err = recv_result.error();
            outcome_promise->set_value(std::move(outcome));
            co_return;
        }
        ++outcome.recv_count;

        auto type_result = parse_question_type(buf.data(), recv_result->size);
        if (!type_result) {
            outcome.err = type_result.error();
            outcome_promise->set_value(std::move(outcome));
            co_return;
        }

        std::vector<std::uint8_t> response;
        if (*type_result == static_cast<std::uint16_t>(RecordType::A)) {
            response = make_address_response(read_be16(buf.data()), qname, *type_result, v4_addr.data(), v4_addr.size(),
                                             60);
        } else if (*type_result == static_cast<std::uint16_t>(RecordType::AAAA) && return_aaaa_answer) {
            response = make_address_response(read_be16(buf.data()), qname, *type_result, v6_addr.data(), v6_addr.size(),
                                             120);
        } else {
            response = make_empty_response(read_be16(buf.data()), qname, *type_result);
        }
        if (!echo_question_name(buf.data(), recv_result->size, response)) {
            outcome.err = IoErr::Invalid;
            outcome_promise->set_value(std::move(outcome));
            co_return;
        }

        auto send_result = co_await socket.send_to(response.data(), response.size(), recv_result->peer, 2s);
        if (!send_result) {
            outcome.err = send_result.error();
            outcome_promise->set_value(std::move(outcome));
            co_return;
        }
    }

    outcome.err = IoErr::None;
    socket.close();
    outcome_promise->set_value(std::move(outcome));
}

DetachedTask run_policy_resolve(fiber::event::EventLoop *loop, fiber::dns::SharedDnsCache2 *cache, std::uint16_t port,
                                AddressPolicy policy, std::string_view host, std::promise<AddressOutcome> *promise) {
    AddressOutcome outcome;
    DnsResolverLocal local;
    DnsClient::Options client_options{};
    client_options.server = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);
    client_options.timeout = 200ms;
    client_options.attempts = 1;
    DnsResolver dns_resolver;
    if (!local.init(*loop, *cache, client_options, {})) {
        outcome.err = IoErr::Invalid;
        promise->set_value(std::move(outcome));
        co_return;
    }
    if (!dns_resolver.init(local, {})) {
        outcome.err = IoErr::Invalid;
        local.release();
        promise->set_value(std::move(outcome));
        co_return;
    }

    AddressResolveResult result;
    if (!result.init()) {
        outcome.err = IoErr::NoMem;
        dns_resolver.release();
        local.release();
        promise->set_value(std::move(outcome));
        co_return;
    }

    auto resolved = co_await dns_resolver.resolve_host(host, policy, result);
    outcome.err = resolved ? IoErr::None : resolved.error();
    if (resolved) {
        outcome.status = *resolved;
        outcome.canonical = std::string(result.canonical_name());
        for (std::uint16_t i = 0; i < result.record_count(); ++i) {
            outcome.records.push_back(result.records()[i].to_string());
        }
    }
    dns_resolver.release();
    local.release();
    promise->set_value(std::move(outcome));
}

DetachedTask run_dual_policy_resolve(fiber::event::EventLoop *loop, fiber::dns::SharedDnsCache2 *cache,
                                     std::uint16_t port, std::string_view host,
                                     std::promise<DualAddressOutcome> *promise) {
    DualAddressOutcome dual_outcome;
    DnsResolverLocal local;
    DnsClient::Options client_options{};
    client_options.server = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);
    client_options.timeout = 200ms;
    client_options.attempts = 1;
    DnsResolver dns_resolver;
    if (!local.init(*loop, *cache, client_options, {})) {
        dual_outcome.first.err = IoErr::Invalid;
        dual_outcome.second.err = IoErr::Invalid;
        promise->set_value(std::move(dual_outcome));
        co_return;
    }
    if (!dns_resolver.init(local, {})) {
        dual_outcome.first.err = IoErr::Invalid;
        dual_outcome.second.err = IoErr::Invalid;
        local.release();
        promise->set_value(std::move(dual_outcome));
        co_return;
    }

    AddressResolveResult first_result;
    AddressResolveResult second_result;
    if (!first_result.init() || !second_result.init()) {
        dual_outcome.first.err = IoErr::NoMem;
        dual_outcome.second.err = IoErr::NoMem;
        dns_resolver.release();
        local.release();
        promise->set_value(std::move(dual_outcome));
        co_return;
    }

    auto first = co_await dns_resolver.resolve_host(host, AddressPolicy::V6First, first_result);
    dual_outcome.first.err = first ? IoErr::None : first.error();
    if (first) {
        dual_outcome.first.status = *first;
        dual_outcome.first.canonical = std::string(first_result.canonical_name());
        for (std::uint16_t i = 0; i < first_result.record_count(); ++i) {
            dual_outcome.first.records.push_back(first_result.records()[i].to_string());
        }
    }

    auto second = co_await dns_resolver.resolve_host(host, AddressPolicy::V4First, second_result);
    dual_outcome.second.err = second ? IoErr::None : second.error();
    if (second) {
        dual_outcome.second.status = *second;
        dual_outcome.second.canonical = std::string(second_result.canonical_name());
        for (std::uint16_t i = 0; i < second_result.record_count(); ++i) {
            dual_outcome.second.records.push_back(second_result.records()[i].to_string());
        }
    }

    dns_resolver.release();
    local.release();
    promise->set_value(std::move(dual_outcome));
}

DetachedTask run_literal_endpoint_resolve(fiber::event::EventLoop *loop, fiber::dns::SharedDnsCache2 *cache,
                                          std::promise<EndpointOutcome> *promise) {
    EndpointOutcome outcome;
    DnsResolverLocal local;
    DnsClient::Options client_options{};
    client_options.server = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 65053);
    DnsResolver dns_resolver;
    AddressResolver address_resolver;
    if (!local.init(*loop, *cache, client_options, {})) {
        outcome.err = IoErr::Invalid;
        promise->set_value(std::move(outcome));
        co_return;
    }
    if (!dns_resolver.init(local, {})) {
        outcome.err = IoErr::Invalid;
        local.release();
        promise->set_value(std::move(outcome));
        co_return;
    }
    if (!address_resolver.init(dns_resolver, {})) {
        outcome.err = IoErr::Invalid;
        dns_resolver.release();
        local.release();
        promise->set_value(std::move(outcome));
        co_return;
    }

    fiber::dns::EndpointResolveResult result;
    if (!result.init()) {
        outcome.err = IoErr::NoMem;
        address_resolver.release();
        dns_resolver.release();
        local.release();
        promise->set_value(std::move(outcome));
        co_return;
    }

    auto resolved = co_await address_resolver.resolve("127.0.0.1", 8443, result);
    outcome.err = resolved ? IoErr::None : resolved.error();
    if (resolved) {
        outcome.status = *resolved;
        outcome.canonical = std::string(result.canonical_name());
        for (std::uint16_t i = 0; i < result.record_count(); ++i) {
            outcome.endpoints.push_back(result.records()[i].to_string());
        }
    }

    address_resolver.release();
    dns_resolver.release();
    local.release();
    promise->set_value(std::move(outcome));
}

DetachedTask run_multi_v4_server(fiber::event::EventLoop *loop, std::promise<std::uint16_t> *port_promise,
                                 std::promise<ServerOutcome> *outcome_promise, std::string qname,
                                 std::vector<std::array<std::uint8_t, 4>> v4_addrs) {
    ServerOutcome outcome;
    fiber::net::UdpSocket socket(*loop);
    auto bind_result = socket.bind(fiber::net::SocketAddress::any_v4(), {});
    if (!bind_result) {
        port_promise->set_value(0);
        outcome.err = bind_result.error();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }

    auto port_result = resolve_port(socket.fd());
    port_promise->set_value(port_result ? *port_result : 0);
    if (!port_result) {
        outcome.err = port_result.error();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }

    std::array<std::uint8_t, 512> buf{};
    auto recv_result = co_await socket.recv_from(buf.data(), buf.size(), 2s);
    if (!recv_result) {
        outcome.err = recv_result.error();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }
    ++outcome.recv_count;

    auto type_result = parse_question_type(buf.data(), recv_result->size);
    if (!type_result) {
        outcome.err = type_result.error();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }

    std::vector<std::uint8_t> response;
    if (*type_result == static_cast<std::uint16_t>(RecordType::A)) {
        std::vector<const std::uint8_t *> record_ptrs;
        std::vector<std::uint16_t> record_lengths;
        record_ptrs.reserve(v4_addrs.size());
        record_lengths.reserve(v4_addrs.size());
        for (const auto &addr: v4_addrs) {
            record_ptrs.push_back(addr.data());
            record_lengths.push_back(static_cast<std::uint16_t>(addr.size()));
        }
        response = make_multi_address_response(read_be16(buf.data()), qname, *type_result, record_ptrs.data(),
                                               record_lengths.data(), record_ptrs.size(), 60);
    } else {
        response = make_empty_response(read_be16(buf.data()), qname, *type_result);
    }
    if (!echo_question_name(buf.data(), recv_result->size, response)) {
        outcome.err = IoErr::Invalid;
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }

    auto send_result = co_await socket.send_to(response.data(), response.size(), recv_result->peer, 2s);
    if (!send_result) {
        outcome.err = send_result.error();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }

    outcome.err = IoErr::None;
    socket.close();
    outcome_promise->set_value(std::move(outcome));
}

} // namespace

TEST(DnsResolverTest, ResolvesDualStackInPreferredOrder) {
    fiber::event::EventLoopGroup group(2);
    group.start();

    fiber::dns::SharedDnsCache2 cache;
    ASSERT_TRUE(cache.init(group.at(0)));

    std::promise<std::uint16_t> port_promise;
    std::promise<ServerOutcome> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();

    std::array<std::uint8_t, 16> v6_addr{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    fiber::async::spawn(group.at(1), [&]() {
        return run_dual_stack_server(&group.at(1), &port_promise, &server_promise, "dual.example", {1, 1, 1, 1},
                                     v6_addr, true);
    });

    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<DualAddressOutcome> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_dual_policy_resolve(&group.at(0), &cache, port, "dual.example", &promise);
    });

    const DualAddressOutcome outcomes = future.get();
    const ServerOutcome server_outcome = server_future.get();

    EXPECT_EQ(outcomes.first.err, IoErr::None);
    EXPECT_EQ(outcomes.first.status, ResolveStatus::Success);
    ASSERT_EQ(outcomes.first.records.size(), 2u);
    EXPECT_EQ(outcomes.first.records[0], "2001:db8::1");
    EXPECT_EQ(outcomes.first.records[1], "1.1.1.1");

    EXPECT_EQ(outcomes.second.err, IoErr::None);
    EXPECT_EQ(outcomes.second.status, ResolveStatus::Success);
    ASSERT_EQ(outcomes.second.records.size(), 2u);
    EXPECT_EQ(outcomes.second.records[0], "1.1.1.1");
    EXPECT_EQ(outcomes.second.records[1], "2001:db8::1");

    EXPECT_EQ(server_outcome.err, IoErr::None);
    EXPECT_EQ(server_outcome.recv_count, 2u);

    shutdown_cache(group.at(0), cache);
    group.stop();
    group.join();
}

TEST(DnsResolverTest, ReturnsSuccessWhenOneFamilyHasNoData) {
    fiber::event::EventLoopGroup group(2);
    group.start();

    fiber::dns::SharedDnsCache2 cache;
    ASSERT_TRUE(cache.init(group.at(0)));

    std::promise<std::uint16_t> port_promise;
    std::promise<ServerOutcome> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();

    std::array<std::uint8_t, 16> v6_addr{};
    fiber::async::spawn(group.at(1), [&]() {
        return run_dual_stack_server(&group.at(1), &port_promise, &server_promise, "dual.example", {9, 9, 9, 9},
                                     v6_addr, false);
    });

    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<AddressOutcome> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_policy_resolve(&group.at(0), &cache, port, AddressPolicy::V6First, "dual.example", &promise);
    });

    const AddressOutcome outcome = future.get();
    const ServerOutcome server_outcome = server_future.get();

    EXPECT_EQ(outcome.err, IoErr::None);
    EXPECT_EQ(outcome.status, ResolveStatus::Success);
    ASSERT_EQ(outcome.records.size(), 1u);
    EXPECT_EQ(outcome.records[0], "9.9.9.9");
    EXPECT_EQ(server_outcome.err, IoErr::None);
    EXPECT_EQ(server_outcome.recv_count, 2u);

    shutdown_cache(group.at(0), cache);
    group.stop();
    group.join();
}

TEST(DnsResolverTest, ResolvesMoreThanEightRecordsInSingleFamily) {
    fiber::event::EventLoopGroup group(2);
    group.start();

    fiber::dns::SharedDnsCache2 cache;
    ASSERT_TRUE(cache.init(group.at(0)));

    std::promise<std::uint16_t> port_promise;
    std::promise<ServerOutcome> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();

    std::vector<std::array<std::uint8_t, 4>> v4_addrs;
    for (std::uint8_t i = 1; i <= 9; ++i) {
        v4_addrs.push_back({192, 0, 2, i});
    }
    fiber::async::spawn(group.at(1), [&]() {
        return run_multi_v4_server(&group.at(1), &port_promise, &server_promise, "many.example", std::move(v4_addrs));
    });

    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<AddressOutcome> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_policy_resolve(&group.at(0), &cache, port, AddressPolicy::V4Only, "many.example", &promise);
    });

    const AddressOutcome outcome = future.get();
    const ServerOutcome server_outcome = server_future.get();

    EXPECT_EQ(outcome.err, IoErr::None);
    EXPECT_EQ(outcome.status, ResolveStatus::Success);
    ASSERT_EQ(outcome.records.size(), 9u);
    EXPECT_EQ(outcome.records.front(), "192.0.2.1");
    EXPECT_EQ(outcome.records.back(), "192.0.2.9");
    EXPECT_EQ(server_outcome.err, IoErr::None);
    EXPECT_EQ(server_outcome.recv_count, 1u);

    shutdown_cache(group.at(0), cache);
    group.stop();
    group.join();
}

TEST(DnsResolverTest, LiteralBypassAndEndpointPortMapping) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::dns::SharedDnsCache2 cache;
    ASSERT_TRUE(cache.init(group.at(0)));

    std::promise<EndpointOutcome> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return run_literal_endpoint_resolve(&group.at(0), &cache, &promise); });

    const EndpointOutcome outcome = future.get();
    EXPECT_EQ(outcome.err, IoErr::None);
    EXPECT_EQ(outcome.status, ResolveStatus::Success);
    EXPECT_EQ(outcome.canonical, "127.0.0.1");
    ASSERT_EQ(outcome.endpoints.size(), 1u);
    EXPECT_EQ(outcome.endpoints[0], "127.0.0.1:8443");

    shutdown_cache(group.at(0), cache);
    group.stop();
    group.join();
}
