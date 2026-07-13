#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <string>
#include <vector>

#include "async/Sleep.h"
#include "async/Spawn.h"
#include "async/Timeout.h"
#include "async/WaitGroup.h"
#include "common/IoError.h"
#include "dns/DnsCache.h"
#include "dns/DnsResolverLocal.h"
#include "event/EventLoopGroup.h"
#include "net/UdpSocket.h"

namespace {

using namespace std::chrono_literals;
using fiber::async::DetachedTask;
using fiber::common::IoErr;
using fiber::dns::DnsClient;
using fiber::dns::DnsResolverLocal;
using fiber::dns::QuestionSpec;
using fiber::dns::RecordClass;
using fiber::dns::RecordType;
using fiber::dns::ResolveResult;
using fiber::dns::ResolveStatus;

struct ResolveOutcome {
    IoErr err = IoErr::Unknown;
    ResolveStatus status = ResolveStatus::ServerFailure;
    std::string canonical;
    std::vector<std::string> records;
};

struct ServerOutcome {
    IoErr err = IoErr::Unknown;
    std::size_t recv_count = 0;
};

struct CacheLookupOutcome {
    IoErr err = IoErr::Unknown;
    bool found = false;
};

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

std::vector<std::uint8_t> make_a_response(std::uint16_t id, std::string_view qname, std::array<std::uint8_t, 4> addr,
                                          std::string_view answer_owner = {}, std::uint32_t ttl = 60) {
    std::vector<std::uint8_t> packet;
    push_be16(packet, id);
    push_be16(packet, 0x8180U);
    push_be16(packet, 1);
    push_be16(packet, 1);
    push_be16(packet, 0);
    push_be16(packet, 0);

    auto qname_wire = encode_dns_name(qname);
    packet.insert(packet.end(), qname_wire.begin(), qname_wire.end());
    push_be16(packet, static_cast<std::uint16_t>(RecordType::A));
    push_be16(packet, static_cast<std::uint16_t>(RecordClass::IN));

    if (answer_owner.empty() || answer_owner == qname) {
        push_be16(packet, 0xc00cU);
    } else {
        auto owner_wire = encode_dns_name(answer_owner);
        packet.insert(packet.end(), owner_wire.begin(), owner_wire.end());
    }
    push_be16(packet, static_cast<std::uint16_t>(RecordType::A));
    push_be16(packet, static_cast<std::uint16_t>(RecordClass::IN));
    push_be32(packet, ttl);
    push_be16(packet, 4);
    packet.insert(packet.end(), addr.begin(), addr.end());
    return packet;
}

std::vector<std::uint8_t> make_conflicting_cname_response(std::uint16_t id, std::string_view qname,
                                                          std::string_view first_target,
                                                          std::string_view second_target) {
    std::vector<std::uint8_t> packet;
    push_be16(packet, id);
    push_be16(packet, 0x8180U);
    push_be16(packet, 1);
    push_be16(packet, 2);
    push_be16(packet, 0);
    push_be16(packet, 0);

    auto qname_wire = encode_dns_name(qname);
    packet.insert(packet.end(), qname_wire.begin(), qname_wire.end());
    push_be16(packet, static_cast<std::uint16_t>(RecordType::A));
    push_be16(packet, static_cast<std::uint16_t>(RecordClass::IN));

    const auto append_cname = [&](std::string_view target) {
        auto target_wire = encode_dns_name(target);
        push_be16(packet, 0xc00cU);
        push_be16(packet, static_cast<std::uint16_t>(RecordType::CNAME));
        push_be16(packet, static_cast<std::uint16_t>(RecordClass::IN));
        push_be32(packet, 60);
        push_be16(packet, static_cast<std::uint16_t>(target_wire.size()));
        packet.insert(packet.end(), target_wire.begin(), target_wire.end());
    };
    append_cname(first_target);
    append_cname(second_target);
    return packet;
}

std::vector<std::uint8_t> make_cname_a_response(std::uint16_t id, std::string_view qname, std::string_view target,
                                                std::array<std::uint8_t, 4> addr, std::string_view unrelated_owner = {},
                                                std::array<std::uint8_t, 4> unrelated_addr = {}) {
    std::vector<std::uint8_t> packet;
    push_be16(packet, id);
    push_be16(packet, 0x8180U);
    push_be16(packet, 1);
    push_be16(packet, unrelated_owner.empty() ? 2 : 3);
    push_be16(packet, 0);
    push_be16(packet, 0);

    auto qname_wire = encode_dns_name(qname);
    auto target_wire = encode_dns_name(target);
    packet.insert(packet.end(), qname_wire.begin(), qname_wire.end());
    push_be16(packet, static_cast<std::uint16_t>(RecordType::A));
    push_be16(packet, static_cast<std::uint16_t>(RecordClass::IN));

    push_be16(packet, 0xc00cU);
    push_be16(packet, static_cast<std::uint16_t>(RecordType::CNAME));
    push_be16(packet, static_cast<std::uint16_t>(RecordClass::IN));
    push_be32(packet, 120);
    push_be16(packet, static_cast<std::uint16_t>(target_wire.size()));
    packet.insert(packet.end(), target_wire.begin(), target_wire.end());

    packet.insert(packet.end(), target_wire.begin(), target_wire.end());
    push_be16(packet, static_cast<std::uint16_t>(RecordType::A));
    push_be16(packet, static_cast<std::uint16_t>(RecordClass::IN));
    push_be32(packet, 60);
    push_be16(packet, 4);
    packet.insert(packet.end(), addr.begin(), addr.end());

    if (!unrelated_owner.empty()) {
        auto unrelated_wire = encode_dns_name(unrelated_owner);
        packet.insert(packet.end(), unrelated_wire.begin(), unrelated_wire.end());
        push_be16(packet, static_cast<std::uint16_t>(RecordType::A));
        push_be16(packet, static_cast<std::uint16_t>(RecordClass::IN));
        push_be32(packet, 60);
        push_be16(packet, 4);
        packet.insert(packet.end(), unrelated_addr.begin(), unrelated_addr.end());
    }
    return packet;
}

DetachedTask run_single_response_server(fiber::event::EventLoop *loop, std::promise<std::uint16_t> *port_promise,
                                        std::promise<ServerOutcome> *outcome_promise,
                                        std::vector<std::uint8_t> response, std::chrono::milliseconds delay = 0ms,
                                        std::chrono::milliseconds observe_after_response = 0ms) {
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
    auto recv_result =
            co_await fiber::async::timeout_for([&]() { return socket.recv_from(buf.data(), buf.size()); }, 2s);
    if (!recv_result) {
        outcome.err = recv_result.error();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }
    ++outcome.recv_count;
    response[0] = buf[0];
    response[1] = buf[1];
    if (!echo_question_name(buf.data(), recv_result->size, response)) {
        outcome.err = IoErr::Invalid;
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }

    if (delay > 0ms) {
        co_await fiber::async::sleep(delay);
    }

    auto send_result = co_await fiber::async::timeout_for(
            [&]() { return socket.send_to(response.data(), response.size(), recv_result->peer); }, 2s);
    outcome.err = send_result ? IoErr::None : send_result.error();
    if (send_result && observe_after_response > 0ms) {
        auto extra = co_await fiber::async::timeout_for([&]() { return socket.recv_from(buf.data(), buf.size()); },
                                                        observe_after_response);
        if (extra) {
            ++outcome.recv_count;
        } else if (extra.error() != IoErr::TimedOut) {
            outcome.err = extra.error();
        }
    }
    socket.close();
    outcome_promise->set_value(std::move(outcome));
}

DetachedTask run_cache_hit_resolve(fiber::event::EventLoop *loop, fiber::dns::SharedDnsCache *cache,
                                   std::promise<ResolveOutcome> *promise) {
    ResolveOutcome outcome;
    DnsResolverLocal resolver;
    DnsClient::Options client_options{};
    client_options.server = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 65053);
    DnsResolverLocal::Options resolver_options{};
    if (!resolver.init(*loop, *cache, client_options, resolver_options)) {
        outcome.err = IoErr::Invalid;
        promise->set_value(std::move(outcome));
        co_return;
    }

    std::array<fiber::net::IpAddress, 1> records{fiber::net::IpAddress::v4({10, 0, 0, 1})};
    auto now = loop->now();
    auto cache_err = co_await cache->upsert_a("cache.example", static_cast<std::uint16_t>(RecordClass::IN),
                                              records.data(), 1, now + 30s);
    if (cache_err != IoErr::None) {
        outcome.err = cache_err;
        resolver.release();
        promise->set_value(std::move(outcome));
        co_return;
    }

    ResolveResult result;
    if (!result.init()) {
        outcome.err = IoErr::NoMem;
        resolver.release();
        promise->set_value(std::move(outcome));
        co_return;
    }

    QuestionSpec question;
    question.name = "CACHE.example.";
    question.type = static_cast<std::uint16_t>(RecordType::A);
    question.dns_class = static_cast<std::uint16_t>(RecordClass::IN);
    auto resolve_result = co_await resolver.resolve(question, result);
    outcome.err = resolve_result ? IoErr::None : resolve_result.error();
    if (resolve_result) {
        outcome.status = *resolve_result;
        outcome.canonical = std::string(result.canonical_name());
        for (std::uint16_t i = 0; i < result.record_count(); ++i) {
            outcome.records.push_back(result.records()[i].to_string());
        }
    }
    resolver.release();
    promise->set_value(std::move(outcome));
}

DetachedTask run_double_resolve(fiber::event::EventLoop *loop, fiber::dns::SharedDnsCache *cache, std::uint16_t port,
                                std::promise<ResolveOutcome> *promise) {
    ResolveOutcome outcome;
    DnsResolverLocal resolver;
    DnsClient::Options client_options{};
    client_options.server = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);
    client_options.timeout = 200ms;
    client_options.attempts = 1;
    DnsResolverLocal::Options resolver_options{};
    if (!resolver.init(*loop, *cache, client_options, resolver_options)) {
        outcome.err = IoErr::Invalid;
        promise->set_value(std::move(outcome));
        co_return;
    }

    ResolveResult first;
    ResolveResult second;
    if (!first.init() || !second.init()) {
        outcome.err = IoErr::NoMem;
        resolver.release();
        promise->set_value(std::move(outcome));
        co_return;
    }

    QuestionSpec question;
    question.name = "www.example.com";
    question.type = static_cast<std::uint16_t>(RecordType::A);
    question.dns_class = static_cast<std::uint16_t>(RecordClass::IN);

    auto first_result = co_await resolver.resolve(question, first);
    if (!first_result) {
        outcome.err = first_result.error();
        resolver.release();
        promise->set_value(std::move(outcome));
        co_return;
    }

    auto second_result = co_await resolver.resolve(question, second);
    outcome.err = second_result ? IoErr::None : second_result.error();
    if (second_result) {
        outcome.status = *second_result;
        outcome.canonical = std::string(second.canonical_name());
        for (std::uint16_t i = 0; i < second.record_count(); ++i) {
            outcome.records.push_back(second.records()[i].to_string());
        }
    }
    resolver.release();
    promise->set_value(std::move(outcome));
}

DetachedTask run_single_resolve(fiber::event::EventLoop *loop, fiber::dns::SharedDnsCache *cache, std::uint16_t port,
                                std::string_view qname, DnsResolverLocal::Options resolver_options,
                                std::promise<ResolveOutcome> *promise) {
    ResolveOutcome outcome;
    DnsResolverLocal resolver;
    DnsClient::Options client_options{};
    client_options.server = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);
    client_options.timeout = 200ms;
    client_options.attempts = 1;
    if (!resolver.init(*loop, *cache, client_options, resolver_options)) {
        outcome.err = IoErr::Invalid;
        promise->set_value(std::move(outcome));
        co_return;
    }

    ResolveResult result;
    if (!result.init()) {
        outcome.err = IoErr::NoMem;
        resolver.release();
        promise->set_value(std::move(outcome));
        co_return;
    }

    QuestionSpec question;
    question.name = qname;
    question.type = static_cast<std::uint16_t>(RecordType::A);
    question.dns_class = static_cast<std::uint16_t>(RecordClass::IN);
    auto resolve_result = co_await resolver.resolve(question, result);
    outcome.err = resolve_result ? IoErr::None : resolve_result.error();
    if (resolve_result) {
        outcome.status = *resolve_result;
        outcome.canonical = std::string(result.canonical_name());
        for (std::uint16_t i = 0; i < result.record_count(); ++i) {
            outcome.records.push_back(result.records()[i].to_string());
        }
    }
    resolver.release();
    promise->set_value(std::move(outcome));
}

DetachedTask run_singleflight_resolve(fiber::event::EventLoop *loop, fiber::dns::SharedDnsCache *cache,
                                      std::uint16_t port, std::promise<ResolveOutcome> *first_promise,
                                      std::promise<ResolveOutcome> *second_promise,
                                      std::string_view qname = "singleflight.example") {
    DnsResolverLocal resolver;
    DnsClient::Options client_options{};
    client_options.server = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);
    client_options.timeout = 400ms;
    client_options.attempts = 1;
    DnsResolverLocal::Options resolver_options{};
    if (!resolver.init(*loop, *cache, client_options, resolver_options)) {
        ResolveOutcome first_outcome;
        first_outcome.err = IoErr::Invalid;
        ResolveOutcome second_outcome = first_outcome;
        first_promise->set_value(std::move(first_outcome));
        second_promise->set_value(std::move(second_outcome));
        co_return;
    }

    fiber::async::WaitGroup pending;
    pending.add(2);
    auto run_one = [&](std::promise<ResolveOutcome> *promise) -> DetachedTask {
        ResolveOutcome outcome;
        ResolveResult result;
        if (!result.init()) {
            outcome.err = IoErr::NoMem;
            promise->set_value(std::move(outcome));
            pending.done();
            co_return;
        }

        QuestionSpec question;
        question.name = qname;
        question.type = static_cast<std::uint16_t>(RecordType::A);
        question.dns_class = static_cast<std::uint16_t>(RecordClass::IN);
        auto resolve_result = co_await resolver.resolve(question, result);
        outcome.err = resolve_result ? IoErr::None : resolve_result.error();
        if (resolve_result) {
            outcome.status = *resolve_result;
            outcome.canonical = std::string(result.canonical_name());
            for (std::uint16_t i = 0; i < result.record_count(); ++i) {
                outcome.records.push_back(result.records()[i].to_string());
            }
        }
        promise->set_value(std::move(outcome));
        pending.done();
    };

    fiber::async::spawn(*loop, [&]() { return run_one(first_promise); });
    fiber::async::spawn(*loop, [&]() { return run_one(second_promise); });
    co_await pending.join();
    resolver.release();
    co_return;
}

DetachedTask run_cache_lookup(fiber::event::EventLoop *loop, fiber::dns::SharedDnsCache *cache, std::string_view qname,
                              std::promise<CacheLookupOutcome> *promise) {
    CacheLookupOutcome outcome;
    fiber::dns::NameSnapshot snapshot;
    if (!snapshot.init()) {
        outcome.err = IoErr::NoMem;
        promise->set_value(outcome);
        co_return;
    }

    outcome.err =
            co_await cache->lookup_name(qname, static_cast<std::uint16_t>(RecordClass::IN), loop->now(), snapshot);
    outcome.found = outcome.err == IoErr::None && snapshot.found();
    promise->set_value(outcome);
}

TEST(DnsResolverLocalTest, ResolvesFromCacheWithoutUpstreamQuery) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::dns::SharedDnsCache cache;
    ASSERT_TRUE(cache.init());

    std::promise<ResolveOutcome> promise;
    fiber::async::spawn(group.at(0), [&]() { return run_cache_hit_resolve(&group.at(0), &cache, &promise); });

    const auto outcome = promise.get_future().get();
    group.stop();
    group.join();
    cache.release();

    ASSERT_EQ(outcome.err, IoErr::None);
    ASSERT_EQ(outcome.status, ResolveStatus::Success);
    ASSERT_EQ(outcome.canonical, "cache.example");
    ASSERT_EQ(outcome.records.size(), 1u);
    EXPECT_EQ(outcome.records[0], "10.0.0.1");
}

TEST(DnsResolverLocalTest, ResolvesCnameAndUsesCacheOnSecondLookup) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::dns::SharedDnsCache cache;
    ASSERT_TRUE(cache.init());

    std::promise<std::uint16_t> port_promise;
    std::promise<ServerOutcome> server_promise;
    std::promise<ResolveOutcome> resolve_promise;

    auto response = make_cname_a_response(0, "www.example.com", "edge.example.net", {7, 7, 7, 7});
    fiber::async::spawn(group.at(0), [&]() {
        return run_single_response_server(&group.at(0), &port_promise, &server_promise, response);
    });
    const auto port = port_promise.get_future().get();
    ASSERT_NE(port, 0);

    fiber::async::spawn(group.at(0),
                        [&]() { return run_double_resolve(&group.at(0), &cache, port, &resolve_promise); });

    const auto outcome = resolve_promise.get_future().get();
    const auto server = server_promise.get_future().get();
    group.stop();
    group.join();
    cache.release();

    ASSERT_EQ(server.err, IoErr::None);
    ASSERT_EQ(server.recv_count, 1u);
    ASSERT_EQ(outcome.err, IoErr::None);
    ASSERT_EQ(outcome.status, ResolveStatus::Success);
    ASSERT_EQ(outcome.canonical, "edge.example.net");
    ASSERT_EQ(outcome.records.size(), 1u);
    EXPECT_EQ(outcome.records[0], "7.7.7.7");
}

TEST(DnsResolverLocalTest, CachesOnlyReachableCnameAnswers) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::dns::SharedDnsCache cache;
    ASSERT_TRUE(cache.init());

    std::promise<std::uint16_t> port_promise;
    std::promise<ServerOutcome> server_promise;
    std::promise<ResolveOutcome> resolve_promise;

    auto response = make_cname_a_response(0, "www.example.com", "edge.example.net", {7, 7, 7, 7}, "unrelated.example",
                                          {6, 6, 6, 6});
    fiber::async::spawn(group.at(0), [&]() {
        return run_single_response_server(&group.at(0), &port_promise, &server_promise, response);
    });
    const auto port = port_promise.get_future().get();
    ASSERT_NE(port, 0);

    fiber::async::spawn(group.at(0),
                        [&]() { return run_double_resolve(&group.at(0), &cache, port, &resolve_promise); });

    const auto outcome = resolve_promise.get_future().get();
    const auto server = server_promise.get_future().get();
    std::promise<CacheLookupOutcome> cache_promise;
    fiber::async::spawn(group.at(0),
                        [&]() { return run_cache_lookup(&group.at(0), &cache, "unrelated.example", &cache_promise); });
    const auto cache_outcome = cache_promise.get_future().get();
    group.stop();
    group.join();
    cache.release();

    ASSERT_EQ(server.err, IoErr::None);
    ASSERT_EQ(server.recv_count, 1u);
    ASSERT_EQ(outcome.err, IoErr::None);
    ASSERT_EQ(outcome.status, ResolveStatus::Success);
    ASSERT_EQ(outcome.records.size(), 1u);
    EXPECT_EQ(outcome.records[0], "7.7.7.7");
    ASSERT_EQ(cache_outcome.err, IoErr::None);
    EXPECT_FALSE(cache_outcome.found);
}

TEST(DnsResolverLocalTest, RejectsUnrelatedAnswerWithoutRetryingOrOrphaningWaiters) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::dns::SharedDnsCache cache;
    ASSERT_TRUE(cache.init());

    std::promise<std::uint16_t> port_promise;
    std::promise<ServerOutcome> server_promise;
    std::promise<ResolveOutcome> first_promise;
    std::promise<ResolveOutcome> second_promise;

    auto response = make_a_response(0, "poison.example", {6, 6, 6, 6}, "unrelated.example");
    fiber::async::spawn(group.at(0), [&]() {
        return run_single_response_server(&group.at(0), &port_promise, &server_promise, response, 50ms);
    });
    const auto port = port_promise.get_future().get();
    ASSERT_NE(port, 0);

    auto first_future = first_promise.get_future();
    auto second_future = second_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_singleflight_resolve(&group.at(0), &cache, port, &first_promise, &second_promise, "poison.example");
    });

    const bool first_ready = first_future.wait_for(2s) == std::future_status::ready;
    const bool second_ready = second_future.wait_for(2s) == std::future_status::ready;
    if (!first_ready || !second_ready) {
        group.stop();
        group.join();
        cache.release();
        FAIL() << "concurrent malformed-response lookups did not both complete";
    }

    const auto first = first_future.get();
    const auto second = second_future.get();
    const auto server = server_promise.get_future().get();
    std::promise<CacheLookupOutcome> cache_promise;
    fiber::async::spawn(group.at(0),
                        [&]() { return run_cache_lookup(&group.at(0), &cache, "unrelated.example", &cache_promise); });
    const auto cache_outcome = cache_promise.get_future().get();
    group.stop();
    group.join();
    cache.release();

    ASSERT_EQ(server.err, IoErr::None);
    ASSERT_EQ(server.recv_count, 1u);
    EXPECT_EQ(first.err, IoErr::Invalid);
    EXPECT_EQ(second.err, IoErr::Invalid);
    ASSERT_EQ(cache_outcome.err, IoErr::None);
    EXPECT_FALSE(cache_outcome.found);
}

TEST(DnsResolverLocalTest, RejectsConflictingCnameTargetsBeforeCaching) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::dns::SharedDnsCache cache;
    ASSERT_TRUE(cache.init());

    std::promise<std::uint16_t> port_promise;
    std::promise<ServerOutcome> server_promise;
    std::promise<ResolveOutcome> resolve_promise;
    auto response = make_conflicting_cname_response(0, "conflict.example", "first.example", "second.example");
    fiber::async::spawn(group.at(0), [&]() {
        return run_single_response_server(&group.at(0), &port_promise, &server_promise, response);
    });
    const auto port = port_promise.get_future().get();
    ASSERT_NE(port, 0);

    fiber::async::spawn(group.at(0), [&]() {
        return run_single_resolve(&group.at(0), &cache, port, "conflict.example", {}, &resolve_promise);
    });
    const auto outcome = resolve_promise.get_future().get();
    const auto server = server_promise.get_future().get();
    std::promise<CacheLookupOutcome> cache_promise;
    fiber::async::spawn(group.at(0),
                        [&]() { return run_cache_lookup(&group.at(0), &cache, "conflict.example", &cache_promise); });
    const auto cache_outcome = cache_promise.get_future().get();
    group.stop();
    group.join();
    cache.release();

    ASSERT_EQ(server.err, IoErr::None);
    ASSERT_EQ(server.recv_count, 1u);
    EXPECT_EQ(outcome.err, IoErr::Invalid);
    ASSERT_EQ(cache_outcome.err, IoErr::None);
    EXPECT_FALSE(cache_outcome.found);
}

TEST(DnsResolverLocalTest, DoesNotRequeryWhenExpectedCacheEntryIsMissing) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::dns::SharedDnsCache cache;
    ASSERT_TRUE(cache.init());

    std::promise<std::uint16_t> port_promise;
    std::promise<ServerOutcome> server_promise;
    std::promise<ResolveOutcome> resolve_promise;
    auto response = make_a_response(0, "expired.example", {1, 2, 3, 4}, {}, 0);
    fiber::async::spawn(group.at(0), [&]() {
        return run_single_response_server(&group.at(0), &port_promise, &server_promise, response, 0ms, 300ms);
    });
    const auto port = port_promise.get_future().get();
    ASSERT_NE(port, 0);

    DnsResolverLocal::Options resolver_options{};
    resolver_options.min_positive_ttl = 0s;
    fiber::async::spawn(group.at(0), [&]() {
        return run_single_resolve(&group.at(0), &cache, port, "expired.example", resolver_options, &resolve_promise);
    });
    const auto outcome = resolve_promise.get_future().get();
    const auto server = server_promise.get_future().get();
    group.stop();
    group.join();
    cache.release();

    ASSERT_EQ(server.err, IoErr::None);
    EXPECT_EQ(server.recv_count, 1u);
    EXPECT_EQ(outcome.err, IoErr::Invalid);
}

TEST(DnsResolverLocalTest, ConcurrentLookupsShareSingleUpstreamQuery) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::dns::SharedDnsCache cache;
    ASSERT_TRUE(cache.init());

    std::promise<std::uint16_t> port_promise;
    std::promise<ServerOutcome> server_promise;
    std::promise<ResolveOutcome> first_promise;
    std::promise<ResolveOutcome> second_promise;

    auto response = make_a_response(0, "singleflight.example", {9, 8, 7, 6});
    fiber::async::spawn(group.at(0), [&]() {
        return run_single_response_server(&group.at(0), &port_promise, &server_promise, response, 50ms);
    });
    const auto port = port_promise.get_future().get();
    ASSERT_NE(port, 0);

    fiber::async::spawn(group.at(0), [&]() {
        return run_singleflight_resolve(&group.at(0), &cache, port, &first_promise, &second_promise);
    });

    const auto first = first_promise.get_future().get();
    const auto second = second_promise.get_future().get();
    const auto server = server_promise.get_future().get();
    group.stop();
    group.join();
    cache.release();

    ASSERT_EQ(server.err, IoErr::None);
    ASSERT_EQ(server.recv_count, 1u);
    ASSERT_EQ(first.err, IoErr::None);
    ASSERT_EQ(second.err, IoErr::None);
    ASSERT_EQ(first.status, ResolveStatus::Success);
    ASSERT_EQ(second.status, ResolveStatus::Success);
    ASSERT_EQ(first.records.size(), 1u);
    ASSERT_EQ(second.records.size(), 1u);
    EXPECT_EQ(first.records[0], "9.8.7.6");
    EXPECT_EQ(second.records[0], "9.8.7.6");
}

} // namespace
