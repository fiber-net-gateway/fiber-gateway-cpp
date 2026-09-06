#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/Yield.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/Http2PooledExchange.h>
#include <fiber/http/LocalHttp2ConnectionPoolSet.h>
#include <fiber/net/TrustStore.h>

namespace {
using namespace fiber;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;
using async::DetachedTask;
using async::Task;
using common::IoErr;
using Key = http::HttpConnectionGroupKey;
using Lease = http::LocalHttp2ConnectionPoolSet::Lease;
constexpr std::size_t kErrorCount = static_cast<std::size_t>(IoErr::Unknown) + 1;

struct Options {
    std::size_t loops = 2, concurrency = 32, keys = 1, rounds = 100;
    std::uint64_t duration_ms = 10000, warmup_ms = 1000, timeout_ms = 2000, acquire_ms = 1000;
    std::uint64_t rate = 0, clear_ms = 0, cancel_percent = 0, read_delay_ms = 0, hold_ms = 0;
    std::uint64_t seed = 1, dial_delay_ms = 0;
    std::uint64_t cancel_until_ms = 0;
    std::uint16_t port = 18082;
    std::string host = "127.0.0.1", path = "/small.bin", ca_file;
    bool lifecycle = false, fifo = false, tls = false, bad_trust = false, acquire_only = false;
    bool allow_errors = false, mixed = false;
    http::Http2ConnectionPoolCore::Options pool{};
};

bool parse(int argc, char **argv, Options &o) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view name(argv[i]);
        if (name == "--help") {
            std::puts("http2_pool_benchmark [--scenario load|lifecycle|fifo] [--host IP] [--port N]\n"
                      "  --loops N --concurrency N (per loop) --keys N --duration-ms N --warmup-ms N\n"
                      "  --connections N (per key) --total-connections N (per loop) --streams N\n"
                      "  --pre-settings N --dials N --idle N --idle-ms N --lifetime N\n"
                      "  --timeout-ms N --acquire-ms N --dial-delay-ms N --hold-ms N\n"
                      "  --path /small.bin|/medium.bin|/large.bin|/slow.bin --mixed\n"
                      "  --cancel-percent N --cancel-until-ms N --read-delay-ms N --rate N (whole process, bounded "
                      "lanes)\n"
                      "  --clear-ms N --rounds N --seed N --tls --ca-file PATH --bad-trust\n"
                      "  --acquire-only --allow-errors\n"
                      "JSONL is written to stdout; diagnostics go to stderr. Body verification is always 100%.");
            std::exit(0);
        }
        if (name == "--tls") {
            o.tls = true;
            continue;
        }
        if (name == "--bad-trust") {
            o.bad_trust = true;
            continue;
        }
        if (name == "--acquire-only") {
            o.acquire_only = true;
            continue;
        }
        if (name == "--allow-errors") {
            o.allow_errors = true;
            continue;
        }
        if (name == "--mixed") {
            o.mixed = true;
            continue;
        }
        if (++i == argc)
            return false;
        const std::string_view value(argv[i]);
        if (name == "--host") {
            o.host = value;
            continue;
        }
        if (name == "--path") {
            o.path = value;
            continue;
        }
        if (name == "--ca-file") {
            o.ca_file = value;
            continue;
        }
        if (name == "--scenario") {
            if (value != "load" && value != "lifecycle" && value != "fifo")
                return false;
            o.lifecycle = value == "lifecycle";
            o.fifo = value == "fifo";
            continue;
        }
        std::uint64_t n = 0;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), n);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || n > 1000000000)
            return false;
        if (name == "--loops")
            o.loops = n;
        else if (name == "--concurrency")
            o.concurrency = n;
        else if (name == "--keys")
            o.keys = n;
        else if (name == "--rounds")
            o.rounds = n;
        else if (name == "--duration-ms")
            o.duration_ms = n;
        else if (name == "--warmup-ms")
            o.warmup_ms = n;
        else if (name == "--timeout-ms")
            o.timeout_ms = n;
        else if (name == "--acquire-ms")
            o.acquire_ms = n;
        else if (name == "--rate")
            o.rate = n;
        else if (name == "--clear-ms")
            o.clear_ms = n;
        else if (name == "--cancel-percent")
            o.cancel_percent = n;
        else if (name == "--cancel-until-ms")
            o.cancel_until_ms = n;
        else if (name == "--read-delay-ms")
            o.read_delay_ms = n;
        else if (name == "--hold-ms")
            o.hold_ms = n;
        else if (name == "--dial-delay-ms")
            o.dial_delay_ms = n;
        else if (name == "--seed")
            o.seed = n;
        else if (name == "--port") {
            if (!n || n > 65535)
                return false;
            o.port = n;
        } else if (name == "--connections")
            o.pool.max_connections_per_group = n;
        else if (name == "--total-connections")
            o.pool.max_connections_total = n;
        else if (name == "--streams")
            o.pool.max_streams_per_connection = n;
        else if (name == "--pre-settings")
            o.pool.pre_settings_max_streams = n;
        else if (name == "--dials")
            o.pool.max_concurrent_dials_per_group = n;
        else if (name == "--idle")
            o.pool.max_idle_total = n;
        else if (name == "--idle-ms")
            o.pool.idle_timeout = Ms(n);
        else if (name == "--lifetime")
            o.pool.max_streams_lifetime = n;
        else
            return false;
    }
    if (o.lifecycle || o.fifo) {
        o.pool.max_connections_per_group = 1;
        o.pool.max_streams_per_connection = 1;
        o.pool.max_connections_total = 2;
        o.pool.max_idle_total = 2;
        o.pool.max_concurrent_dials_per_group = 1;
        o.pool.max_streams_lifetime = 0;
        o.pool.idle_timeout = 60s;
        o.keys = 2;
    }
    return o.loops > 0 && o.loops <= 64 && o.concurrency > 0 && o.loops * o.concurrency <= 65536 && o.keys > 0 &&
           o.keys <= 65536 && o.rounds > 0 && o.rounds <= 100000 && o.duration_ms > 0 && o.timeout_ms > 0 &&
           o.acquire_ms > 0 && o.cancel_percent <= 100 && o.pool.max_connections_total > 0 &&
           o.pool.max_connections_total <= 65536 && o.pool.max_connections_per_group > 0 &&
           o.pool.max_concurrent_dials_per_group > 0 && o.pool.max_idle_total <= o.pool.max_connections_total &&
           o.pool.idle_timeout > 0ms &&
           (o.path == "/small.bin" || o.path == "/medium.bin" || o.path == "/large.bin" || o.path == "/slow.bin");
}

// Fixed logarithmic histogram: 32 buckets/octave, upper-bound percentiles in microseconds.
struct Histogram {
    std::array<std::uint64_t, 1024> buckets{};
    std::uint64_t count = 0, max_us = 0;
    void add(Clock::duration elapsed) noexcept {
        auto us = static_cast<std::uint64_t>(
                std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()));
        max_us = std::max(max_us, us);
        ++count;
        std::size_t exponent = 0;
        auto mantissa = us;
        while (mantissa >= 64 && exponent < 30) {
            mantissa >>= 1;
            ++exponent;
        }
        const auto index = exponent ? (exponent + 1) * 32 + mantissa - 32 : mantissa;
        ++buckets[std::min(index, buckets.size() - 1)];
    }
    std::uint64_t percentile(unsigned permille) const noexcept {
        const auto target = (count * permille + 999) / 1000;
        if (!target)
            return 0;
        std::uint64_t sum = 0;
        for (std::size_t i = 0; i < buckets.size(); ++i) {
            sum += buckets[i];
            if (sum >= target) {
                if (i < 64)
                    return i;
                const auto shift = i / 32 - 1;
                return ((32 + i % 32 + 1) << shift) - 1;
            }
        }
        return max_us;
    }
    void merge(const Histogram &h) noexcept {
        count += h.count;
        max_us = std::max(max_us, h.max_us);
        for (std::size_t i = 0; i < buckets.size(); ++i)
            buckets[i] += h.buckets[i];
    }
};

struct Stats {
    std::uint64_t started = 0, success = 0, errors = 0, canceled = 0, rejected = 0;
    std::uint64_t acquired = 0, released = 0, fast_hits = 0, bytes = 0;
    std::uint64_t dials = 0, dial_ok = 0, dial_error = 0, dial_canceled = 0, dial_failed_callbacks = 0;
    std::uint64_t closed = 0, goaway = 0, violations = 0, max_connections = 0, max_group = 0;
    std::uint64_t max_dials = 0, max_streams = 0, measured_success = 0;
    std::array<std::uint64_t, kErrorCount> error_codes{};
    std::array<std::uint64_t, kErrorCount> close_codes{};
    Histogram latency, acquire_latency, scheduled_latency, successful_latency;
};

struct Benchmark;
struct Shard;

struct GroupState {
    std::size_t total = 0, ready = 0, dials = 0;
};

struct ConnectionState {
    http::Http2ClientConnection *connection = nullptr;
    Shard *owner = nullptr;
    std::uint64_t key_id = 0, generation = 0;
    std::size_t leases = 0;
    bool closed = true, saw_goaway = false;
    http::Http2CloseGate::ObserverHook hook{};
};

struct Shard {
    Shard(Benchmark &bench, std::size_t index, const Options &o);
    Benchmark &bench;
    std::size_t index;
    Stats stats;
    std::vector<Key> keys; // Setup-only storage; never resized after publication.
    std::unique_ptr<GroupState[]> groups;
    std::unique_ptr<ConnectionState[]> connections;
    std::size_t live_requests = 0, live_lanes = 0, pending = 0;
    std::size_t fifo_started = 0, fifo_done = 0, fifo_next = 0, fifo_timed_out = 0;
    std::uint64_t sequence = 0;
    static Task<common::IoResult<void>> dial(void *, http::Http2ClientConnection &, const Key &) noexcept;
    static void count_changed(void *, const Key &, std::size_t, std::size_t) noexcept;
    static void dial_failed(void *, const Key &, IoErr, std::size_t, Ms) noexcept;
    static void on_closed(void *, http::Http2Connection &, IoErr) noexcept;
    void check(bool condition, const char *message) noexcept;
    IoErr request_error(IoErr error, const char *phase, std::uint64_t id, std::uint64_t generation = 0,
                        std::uint32_t stream = 0) noexcept;
    ConnectionState &connection_state(http::Http2ClientConnection &connection) noexcept;
    void emit(const char *kind) noexcept;
    void check_drained() noexcept;
    Task<IoErr> request(mem::BufPool &, std::size_t key_id, std::uint64_t id, std::uint64_t random,
                        bool holder = false) noexcept;
    DetachedTask lane(std::size_t lane_index) noexcept;
    DetachedTask monitor() noexcept;
    DetachedTask holder() noexcept;
    DetachedTask pending_request(bool blocked_dial) noexcept;
    DetachedTask probe(bool shutdown) noexcept;
    DetachedTask fifo_round() noexcept;
};

struct Benchmark {
    explicit Benchmark(Options options) :
        options(std::move(options)), group(this->options.loops), pool(group, this->options.pool),
        address(net::IpAddress::loopback_v4(), this->options.port) {
        net::IpAddress ip;
        if (!net::IpAddress::parse(this->options.host, ip)) {
            std::fputs("--host must be an IP literal\n", stderr);
            std::exit(2);
        }
        address = net::SocketAddress(ip, this->options.port);
        tls.server_name = "localhost";
        tls.verify_name = "localhost";
        tls.handshake_timeout = Ms(this->options.timeout_ms);
        tls.security.verify_peer = true;
        if (this->options.tls && !this->options.bad_trust) {
            auto result = net::TrustStore::create(this->options.ca_file.empty()
                                                          ? net::TrustStoreOptions::system()
                                                          : net::TrustStoreOptions::from_file(this->options.ca_file));
            if (!result) {
                std::fputs("cannot load TLS trust store\n", stderr);
                std::exit(2);
            }
            trust = std::move(*result);
            tls.security.trust_store = trust.get();
        }
        for (std::size_t i = 0; i < this->options.loops; ++i)
            shards.push_back(std::make_unique<Shard>(*this, i, this->options));
    }
    Options options;
    event::EventLoopGroup group;
    http::LocalHttp2ConnectionPoolSet pool;
    net::SocketAddress address;
    std::unique_ptr<net::TrustStore> trust;
    http::HttpClientTlsOptions tls;
    std::vector<std::unique_ptr<Shard>> shards;
    std::mutex output_mutex;
    Clock::time_point start{}, measure_start{}, finish{};
    std::atomic<bool> paused{false}, stopping{false}, done{false}, release_holders{false};
    std::atomic<unsigned> initialized{0}, held_ready{0}, pending_entered{0}, blocked_entered{0};
    std::atomic<unsigned> tasks_done{0}, admin_started{0}, admin_done{0}, probes_done{0};
    std::atomic<std::uint64_t> violations{0};
    std::uint64_t clears = 0, lifecycle_rounds = 0;
    Histogram admin_latency;

    DetachedTask control() noexcept;
    DetachedTask admin(bool shutdown) noexcept {
        ++admin_started;
        const auto began = event::EventLoop::current().now();
        if (shutdown)
            co_await pool.shutdown_async();
        else
            co_await pool.clear_async();
        {
            std::lock_guard lock(output_mutex);
            admin_latency.add(event::EventLoop::current().now() - began);
        }
        ++admin_done;
    }
    Task<void> lifecycle() noexcept;
    Task<void> fifo() noexcept;
    bool summary() noexcept;
};

Shard::Shard(Benchmark &b, std::size_t i, const Options &o) :
    bench(b), index(i), groups(std::make_unique<GroupState[]>(o.keys)),
    connections(std::make_unique<ConnectionState[]>(o.pool.max_connections_total)) {
    keys.reserve(o.keys);
    for (std::size_t k = 0; k < o.keys; ++k)
        keys.push_back(Key::from_ip(b.address.ip(), o.port, o.tls ? Key::Scheme::Https : Key::Scheme::Http,
                                    http::HttpConnectionPoolAffinity(k + 1)));
}

void Shard::check(bool condition, const char *message) noexcept {
    if (condition)
        return;
    ++stats.violations;
    ++bench.violations;
    if (stats.violations <= 10)
        std::fprintf(stderr, "loop=%zu invariant=%s\n", index, message);
}

void Shard::count_changed(void *ctx, const Key &key, std::size_t total, std::size_t ready) noexcept {
    auto &s = *static_cast<Shard *>(ctx);
    s.check(&event::EventLoop::current() == &s.bench.group.at(s.index), "callback loop ownership");
    auto &g = s.groups[key.pool_affinity().value() - 1];
    g.total = total;
    g.ready = ready;
    const auto &o = s.bench.options.pool;
    const auto all = s.bench.pool.connection_total();
    s.check(total <= o.max_connections_per_group && all <= o.max_connections_total, "connection limit");
    s.check(ready <= total, "ready <= total");
    s.stats.max_connections = std::max<std::uint64_t>(s.stats.max_connections, all);
    s.stats.max_group = std::max<std::uint64_t>(s.stats.max_group, total);
}

void Shard::dial_failed(void *ctx, const Key &, IoErr, std::size_t count, Ms retry) noexcept {
    auto &s = *static_cast<Shard *>(ctx);
    ++s.stats.dial_failed_callbacks;
    Ms expected = s.bench.options.pool.dial_retry_backoff;
    for (std::size_t i = 1; i < count && expected < s.bench.options.pool.max_dial_retry_backoff; ++i)
        expected = std::min(expected * 2, s.bench.options.pool.max_dial_retry_backoff);
    s.check(retry == expected, "exponential dial backoff");
}

void Shard::on_closed(void *ctx, http::Http2Connection &connection, IoErr reason) noexcept {
    auto &r = *static_cast<ConnectionState *>(ctx);
    r.closed = true;
    ++r.owner->stats.closed;
    ++r.owner->stats.close_codes[static_cast<std::size_t>(reason)];
    if (connection.peer_goaway_received() && !r.saw_goaway) {
        r.saw_goaway = true;
        ++r.owner->stats.goaway;
    }
}

IoErr Shard::request_error(IoErr error, const char *phase, std::uint64_t id, std::uint64_t generation,
                           std::uint32_t stream) noexcept {
    if (stats.errors < 8) {
        const auto name = common::io_err_name(error);
        std::fprintf(stderr, "loop=%zu request=%llu connection=%llu stream=%u phase=%s error=%.*s\n", index,
                     static_cast<unsigned long long>(id), static_cast<unsigned long long>(generation), stream, phase,
                     static_cast<int>(name.size()), name.data());
    }
    return error;
}

ConnectionState &Shard::connection_state(http::Http2ClientConnection &connection) noexcept {
    for (std::size_t i = 0; i < bench.options.pool.max_connections_total; ++i) {
        if (connections[i].connection == &connection && !connections[i].closed)
            return connections[i];
    }
    // A live Lease may outlive a transport close.
    for (std::size_t i = 0; i < bench.options.pool.max_connections_total; ++i) {
        if (connections[i].connection == &connection)
            return connections[i];
    }
    FIBER_PANIC("benchmark connection not registered");
}

Task<common::IoResult<void>> Shard::dial(void *ctx, http::Http2ClientConnection &connection, const Key &key) noexcept {
    auto &s = *static_cast<Shard *>(ctx);
    auto &b = s.bench;
    const auto id = key.pool_affinity().value() - 1;
    s.check(&connection.loop() == &b.group.at(s.index), "connector loop ownership");
    auto &g = s.groups[id];
    ++g.dials;
    ++s.stats.dials;
    s.stats.max_dials = std::max<std::uint64_t>(s.stats.max_dials, g.dials);
    s.check(g.dials <= b.options.pool.max_concurrent_dials_per_group, "parallel dial limit");
    struct Guard {
        Shard &s;
        GroupState &g;
        bool completed = false;
        ~Guard() {
            --g.dials;
            if (!completed)
                ++s.stats.dial_canceled;
        }
    } guard{s, g};
    if (b.options.lifecycle && id == 1) {
        ++b.blocked_entered;
        co_await async::sleep(60s);
    }
    if (b.options.dial_delay_ms)
        co_await async::sleep(Ms(b.options.dial_delay_ms));
    auto result = b.options.tls ? co_await connection.connect(b.address, 1s, b.tls)
                                : co_await connection.connect(b.address, 1s);
    guard.completed = true;
    if (!result) {
        ++s.stats.dial_error;
        co_return result;
    }
    ++s.stats.dial_ok;
    ConnectionState *record = nullptr;
    for (std::size_t i = 0; i < b.options.pool.max_connections_total; ++i) {
        auto &candidate = s.connections[i];
        if (candidate.closed && candidate.leases == 0) {
            record = &candidate;
            break;
        }
    }
    FIBER_ASSERT(record != nullptr);
    // The closed gate may still exist until the pool's deferred destruction.
    if (record->hook.linked)
        record->hook.gate->remove_observer(record->hook);
    record->connection = &connection;
    record->owner = &s;
    record->key_id = id;
    record->generation = s.stats.dial_ok;
    record->closed = false;
    record->saw_goaway = false;
    connection.close_gate().add_observer(record->hook, &Shard::on_closed, record);
    co_return result;
}

Ms remaining(Clock::time_point deadline) noexcept {
    const auto now = event::EventLoop::current().now();
    return deadline <= now ? 0ms : std::chrono::ceil<Ms>(deadline - now);
}

Task<IoErr> Shard::request(mem::BufPool &buffers, std::size_t key_id, std::uint64_t id, std::uint64_t random,
                           bool hold) noexcept {
    auto &b = bench;
    const auto &o = b.options;
    auto &loop = event::EventLoop::current();
    const auto began = loop.now();
    const auto deadline = began + Ms(o.timeout_ms);
    ++pending;
    std::optional<Lease> lease;
    if (!o.acquire_only) {
        lease = b.pool.try_acquire(keys[key_id]);
        if (lease)
            ++stats.fast_hits;
    }
    if (!lease) {
        auto acquired = co_await b.pool.acquire(keys[key_id], {&Shard::dial, this},
                                                std::min(Ms(o.acquire_ms), remaining(deadline)));
        if (!acquired) {
            --pending;
            stats.acquire_latency.add(loop.now() - began);
            co_return request_error(acquired.error(), "acquire", id);
        }
        lease.emplace(std::move(*acquired));
    }
    --pending;
    stats.acquire_latency.add(loop.now() - began);
    ++stats.acquired;
    auto &connection = lease->connection();
    auto &record = connection_state(connection);
    check(record.key_id == key_id && &connection.loop() == &b.group.at(index), "lease key/loop identity");
    ++record.leases;
    // Declared before exchange: the slot is released before this observer records its release.
    struct LeaseCountGuard {
        Shard &s;
        ConnectionState &record;
        ~LeaseCountGuard() {
            --record.leases;
            ++s.stats.released;
        }
    } lease_guard{*this, record};
    http::Http2PooledExchange exchange(std::move(*lease), buffers);
    if (o.hold_ms)
        co_await async::sleep(std::min(Ms(o.hold_ms), remaining(deadline)));
    char request_id[80];
    const int id_size =
            std::snprintf(request_id, sizeof(request_id), "%llu-%zu-%llu", static_cast<unsigned long long>(o.seed),
                          index, static_cast<unsigned long long>(id));
    http::HttpHeaders headers(buffers);
    if (!headers.add_view("x-bench-id", {request_id, static_cast<std::size_t>(id_size)}))
        co_return IoErr::NoMem;
    std::string_view path = hold ? "/slow.bin" : std::string_view(o.path);
    if (o.mixed && !hold)
        path = random % 100 == 0 ? "/slow.bin" : random % 100 < 10 ? "/medium.bin" : "/small.bin";
    auto sent = co_await exchange->send_header({.method = http::HttpMethod::Get,
                                                .scheme = o.tls ? "https" : "http",
                                                .authority = "localhost",
                                                .path = path,
                                                .headers = &headers},
                                               true, remaining(deadline));
    if (!sent)
        co_return request_error(sent.error(), "send_header", id, record.generation, exchange->stream_id());
    auto &h2 = connection.http2();
    stats.max_streams = std::max<std::uint64_t>(stats.max_streams, h2.local_active_stream_count());
    if (h2.peer_settings_received())
        check(h2.local_active_stream_count() <= h2.peer_max_concurrent_streams(), "peer stream limit");
    auto head = co_await exchange->read_header(remaining(deadline));
    if (!head)
        co_return request_error(head.error(), "read_header", id, record.generation, exchange->stream_id());
    if (!*head) {
        check(false, "missing response header");
        co_return IoErr::Invalid;
    }
    char port[8];
    const auto port_len = std::snprintf(port, sizeof(port), "%u", o.port);
    const bool valid = (*head)->status_code == 200 &&
                       (*head)->headers.get("x-bench-id") == std::string_view(request_id, id_size) &&
                       (*head)->headers.get("x-backend-port") == std::string_view(port, port_len) &&
                       (*head)->headers.get("x-backend-protocol") == "HTTP/2.0";
    check(valid, "status/request ID/backend/protocol");
    if (!valid)
        co_return IoErr::Invalid;
    if (hold) {
        ++b.held_ready;
        while (!b.release_holders.load())
            co_await async::sleep(1ms);
        co_return IoErr::Canceled;
    }
    const bool cancel =
            random % 100 < o.cancel_percent && (!o.cancel_until_ms || began < b.start + Ms(o.cancel_until_ms));
    if (cancel && random % 2 == 0)
        co_return IoErr::Canceled;
    std::size_t bytes = 0;
    const std::size_t expected = path == "/small.bin" ? 1024 : path == "/medium.bin" ? 65536 : 1048576;
    bool first = true;
    for (;;) {
        if (first && o.read_delay_ms)
            co_await async::sleep(std::min(Ms(o.read_delay_ms), remaining(deadline)));
        first = false;
        auto body = co_await exchange->read_body(64 * 1024, remaining(deadline));
        if (!body)
            co_return request_error(body.error(), "read_body", id, record.generation, exchange->stream_id());
        const bool complete = body->complete();
        while (auto *buf = body->first_readable()) {
            const auto size = buf->readable();
            const auto *data = buf->readable_data();
            bool matches = true;
            for (std::size_t i = 0; i < size; ++i) {
                if (data[i] != static_cast<std::uint8_t>((bytes + i) & 255U)) {
                    matches = false;
                    break;
                }
            }
            check(matches && bytes + size <= expected, "response body content/overflow");
            if (!matches || bytes + size > expected)
                co_return IoErr::Invalid;
            bytes += size;
            stats.bytes += size;
            body->consume(size);
        }
        if (cancel)
            co_return IoErr::Canceled;
        if (complete)
            break;
    }
    check(bytes == expected, "response body length");
    co_return bytes == expected ? IoErr::None : IoErr::Invalid;
}

std::uint64_t next_random(std::uint64_t &state) noexcept {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

DetachedTask Shard::lane(std::size_t lane_index) noexcept {
    auto &b = bench;
    auto &loop = event::EventLoop::current();
    mem::BufPool buffers;
    unsigned burst = 0;
    std::uint64_t rng = (b.options.seed + 1) ^ ((index + 1) * 0x9e3779b97f4a7c15ULL) ^ (lane_index + 1);
    const auto lanes = b.options.loops * b.options.concurrency;
    const auto interval = b.options.rate ? std::chrono::nanoseconds(1000000000ULL * lanes / b.options.rate) : 0ns;
    auto scheduled = b.start + (b.options.rate ? std::chrono::nanoseconds(1000000000ULL *
                                                                          (index * b.options.concurrency + lane_index) /
                                                                          b.options.rate)
                                               : 0ns);
    ++live_lanes;
    while (loop.now() < b.finish && !b.stopping.load()) {
        if (b.paused.load()) {
            co_await async::sleep(1ms);
            continue;
        }
        if (scheduled > loop.now()) {
            co_await async::sleep(std::chrono::ceil<Ms>(scheduled - loop.now()));
            continue;
        }
        if (interval > 0ns && loop.now() - scheduled >= interval) {
            const auto missed = static_cast<std::uint64_t>((loop.now() - scheduled) / interval);
            stats.rejected += missed;
            scheduled += interval * missed;
        }
        const auto began = loop.now();
        ++stats.started;
        ++live_requests;
        const auto random = next_random(rng);
        const auto result = co_await request(buffers, random % keys.size(), ++sequence, random);
        --live_requests;
        if (result == IoErr::None) {
            ++stats.success;
            if (loop.now() >= b.measure_start && loop.now() < b.finish)
                ++stats.measured_success;
        } else if (result == IoErr::Canceled) {
            ++stats.canceled;
        } else {
            ++stats.errors;
            ++stats.error_codes[static_cast<std::size_t>(result)];
        }
        if (began >= b.measure_start) {
            stats.latency.add(loop.now() - began);
            stats.scheduled_latency.add(loop.now() - (interval > 0ns ? scheduled : began));
            if (result == IoErr::None)
                stats.successful_latency.add(loop.now() - began);
        }
        buffers.reset();
        if (interval > 0ns)
            scheduled += interval;
        else
            scheduled = loop.now();
        // yield() only posts local work. A perpetually nonempty local queue can delay the poller,
        // including its cached clock/timers. Periodically require an actual timer turn.
        if (++burst == 16 || result != IoErr::None) {
            burst = 0;
            co_await async::sleep(1ms);
        }
    }
    if (interval > 0ns && scheduled < b.finish)
        stats.rejected += static_cast<std::uint64_t>((b.finish - scheduled - 1ns) / interval) + 1;
    --live_lanes;
}

void Shard::emit(const char *kind) noexcept {
    auto &b = bench;
    check(b.pool.idle_total() <= b.options.pool.max_idle_total, "idle budget");
    std::lock_guard lock(b.output_mutex);
    std::printf(
            "{\"type\":\"%s\",\"loop\":%zu,\"elapsed_ms\":%lld,\"started\":%llu,"
            "\"success\":%llu,\"errors\":%llu,\"canceled\":%llu,\"inflight\":%zu,"
            "\"acquire_pending\":%zu,\"connections\":%zu,\"idle\":%zu,\"groups\":%zu,"
            "\"held_leases\":%llu,\"dials\":%llu,\"violations\":%llu}\n",
            kind, index,
            static_cast<long long>(std::chrono::duration_cast<Ms>(event::EventLoop::current().now() - b.start).count()),
            static_cast<unsigned long long>(stats.started), static_cast<unsigned long long>(stats.success),
            static_cast<unsigned long long>(stats.errors), static_cast<unsigned long long>(stats.canceled),
            live_requests, pending, b.pool.connection_total(), b.pool.idle_total(), b.pool.group_count(),
            static_cast<unsigned long long>(stats.acquired - stats.released),
            static_cast<unsigned long long>(stats.dials), static_cast<unsigned long long>(stats.violations));
    std::fflush(stdout);
}

void Shard::check_drained() noexcept {
    check(bench.pool.connection_total() == 0 && bench.pool.idle_total() == 0 && bench.pool.group_count() == 0,
          "all pool gauges drained");
    check(pending == 0 && live_requests == 0 && stats.acquired == stats.released, "all requests/leases drained");
    check(stats.dials == stats.dial_ok + stats.dial_error + stats.dial_canceled, "dial conservation");
    for (std::size_t i = 0; i < keys.size(); ++i)
        check(groups[i].total == 0 && groups[i].ready == 0 && groups[i].dials == 0, "group callbacks drained");
}

DetachedTask Shard::monitor() noexcept {
    auto &b = bench;
    b.pool.set_conn_count_changed_callback(&Shard::count_changed, this);
    b.pool.set_dial_failed_callback(&Shard::dial_failed, this);
    ++b.initialized;
    if (!b.options.lifecycle && !b.options.fifo) {
        for (std::size_t i = 0; i < b.options.concurrency; ++i)
            async::spawn(b.group.at(index), [this, i]() { return lane(i); });
    }
    while (!b.stopping.load()) {
        co_await async::sleep(1s);
        emit("sample");
    }
    while (live_lanes)
        co_await async::sleep(1ms);
    ++b.tasks_done;
}

DetachedTask Shard::holder() noexcept {
    mem::BufPool buffers;
    const auto result = co_await request(buffers, 0, ++sequence, 0, true);
    check(result == IoErr::Canceled, "held real response canceled");
    ++bench.tasks_done;
}

DetachedTask Shard::pending_request(bool blocked_dial) noexcept {
    ++bench.pending_entered;
    ++pending;
    auto lease = co_await bench.pool.acquire(keys[blocked_dial ? 1 : 0], {&Shard::dial, this}, 30s);
    --pending;
    check(!lease && lease.error() == IoErr::Canceled, "admin cancels pending acquire/connector");
    ++bench.tasks_done;
}

DetachedTask Shard::probe(bool shutdown) noexcept {
    check_drained();
    if (shutdown) {
        auto result = co_await bench.pool.acquire(keys[0], {&Shard::dial, this}, 0ms);
        check(!result && result.error() == IoErr::Canceled, "shutdown rejects acquire");
        check(!bench.pool.try_acquire(keys[0]), "shutdown rejects try_acquire");
    } else {
        mem::BufPool buffers;
        const auto result = co_await request(buffers, 0, ++sequence, 99);
        check(result == IoErr::None, "clear permits real subsequent request");
    }
    ++bench.probes_done;
}

Task<void> Benchmark::lifecycle() noexcept {
    const unsigned workers = static_cast<unsigned>(options.loops);
    for (std::size_t round = 0; round <= options.rounds; ++round) {
        // Retire successful probes from the preceding round before the next barrier.
        co_await pool.clear_async();
        tasks_done = 0;
        held_ready = 0;
        pending_entered = 0;
        blocked_entered = 0;
        admin_started = 0;
        admin_done = 0;
        probes_done = 0;
        release_holders = false;
        for (auto &s: shards)
            async::spawn(group.at(s->index), [p = s.get()]() { return p->holder(); });
        while (held_ready.load() != workers)
            co_await async::sleep(1ms);
        for (auto &s: shards) {
            for (unsigned i = 0; i < 8; ++i)
                async::spawn(group.at(s->index), [p = s.get()]() { return p->pending_request(false); });
            async::spawn(group.at(s->index), [p = s.get()]() { return p->pending_request(true); });
        }
        while (pending_entered.load() != 9 * workers || blocked_entered.load() != workers)
            co_await async::sleep(1ms);
        const bool shutdown = round == options.rounds;
        const unsigned callers = shutdown ? workers * 2 + 1 : 1;
        for (unsigned i = 0; i < callers; ++i)
            async::spawn(group.at(i % workers), [this, shutdown, i]() { return admin(shutdown && i != 0); });
        while (admin_started.load() != callers)
            co_await async::sleep(1ms);
        while (tasks_done.load() != 9 * workers)
            co_await async::sleep(1ms);
        // The admin operation may observe a connection that has already entered
        // its deferred close path; the hard lifetime assertion is the final
        // drain check below, after the holder is released.
        release_holders = true;
        while (admin_done.load() != callers || tasks_done.load() != 10 * workers)
            co_await async::sleep(1ms);
        for (auto &s: shards)
            async::spawn(group.at(s->index), [p = s.get(), shutdown]() { return p->probe(shutdown); });
        while (probes_done.load() != workers)
            co_await async::sleep(1ms);
        ++lifecycle_rounds;
    }
    tasks_done = 0;
}

DetachedTask Shard::fifo_round() noexcept {
    auto &b = bench;
    for (std::size_t round = 0; round < b.options.rounds; ++round) {
        auto held = co_await b.pool.acquire(keys[0], {&Shard::dial, this}, 2s);
        check(held.has_value(), "FIFO initial connection");
        if (!held)
            break;
        fifo_started = fifo_done = fifo_next = fifo_timed_out = 0;
        for (std::size_t i = 0; i < 100; ++i) {
            async::spawn(b.group.at(index), [this, i]() -> DetachedTask {
                ++fifo_started;
                auto result = co_await bench.pool.acquire(keys[0], {&Shard::dial, this}, i % 10 == 0 ? 5ms : 2s);
                if (i % 10 == 0) {
                    check(!result && result.error() == IoErr::TimedOut, "FIFO waiter timeout");
                    ++fifo_timed_out;
                } else {
                    check(result.has_value(), "FIFO waiter succeeds");
                    while (fifo_next % 10 == 0)
                        ++fifo_next;
                    check(i == fifo_next, "FIFO order after canceled waiters");
                    ++fifo_next;
                    result = std::unexpected(IoErr::Canceled);
                    check(!bench.pool.try_acquire(keys[0]) || fifo_next >= 100, "try_acquire cannot barge");
                }
                ++fifo_done;
            });
            // The child is posted through the notify queue; a local defer loop can starve that queue.
            while (fifo_started != i + 1)
                co_await async::sleep(1ms);
        }
        while (fifo_timed_out != 10)
            co_await async::sleep(1ms);
        check(!b.pool.try_acquire(keys[0]), "poll saturated connection");
        const auto dials_before = stats.dials;
        auto poll = co_await b.pool.acquire(keys[0], {&Shard::dial, this}, 0ms);
        check(!poll && poll.error() == IoErr::Busy && stats.dials == dials_before, "zero timeout never dials");
        held = std::unexpected(IoErr::Canceled);
        while (fifo_done != 100)
            co_await async::sleep(1ms);
    }
    ++b.probes_done;
}

Task<void> Benchmark::fifo() noexcept {
    for (auto &s: shards)
        async::spawn(group.at(s->index), [p = s.get()]() { return p->fifo_round(); });
    while (probes_done.load() != options.loops)
        co_await async::sleep(1ms);
}

DetachedTask Benchmark::control() noexcept {
    while (initialized.load() != options.loops)
        co_await async::sleep(1ms);
    if (options.lifecycle)
        co_await lifecycle();
    else if (options.fifo)
        co_await fifo();
    else {
        auto next_clear = start + Ms(options.clear_ms);
        while (event::EventLoop::current().now() < finish) {
            co_await async::sleep(10ms);
            if (options.clear_ms && event::EventLoop::current().now() >= next_clear) {
                paused = true;
                const auto began = event::EventLoop::current().now();
                co_await pool.clear_async();
                admin_latency.add(event::EventLoop::current().now() - began);
                ++clears;
                paused = false;
                next_clear = event::EventLoop::current().now() + Ms(options.clear_ms);
            }
        }
    }
    stopping = true;
    while (tasks_done.load() != options.loops)
        co_await async::sleep(1ms);
    co_await pool.shutdown_async();
    probes_done = 0;
    for (auto &s: shards) {
        async::spawn(group.at(s->index), [this, p = s.get()]() -> DetachedTask {
            p->check_drained();
            p->emit("drained");
            ++probes_done;
            co_return;
        });
    }
    while (probes_done.load() != options.loops)
        co_await async::sleep(1ms);
    done = true;
}

bool Benchmark::summary() noexcept {
    Stats sum;
    for (auto &s: shards) {
        const auto &v = s->stats;
        sum.started += v.started;
        sum.success += v.success;
        sum.errors += v.errors;
        sum.canceled += v.canceled;
        sum.rejected += v.rejected;
        sum.acquired += v.acquired;
        sum.released += v.released;
        sum.fast_hits += v.fast_hits;
        sum.bytes += v.bytes;
        sum.dials += v.dials;
        sum.dial_ok += v.dial_ok;
        sum.dial_error += v.dial_error;
        sum.dial_canceled += v.dial_canceled;
        sum.dial_failed_callbacks += v.dial_failed_callbacks;
        sum.goaway += v.goaway;
        sum.closed += v.closed;
        sum.measured_success += v.measured_success;
        sum.max_connections = std::max(sum.max_connections, v.max_connections);
        sum.max_group = std::max(sum.max_group, v.max_group);
        sum.max_dials = std::max(sum.max_dials, v.max_dials);
        sum.max_streams = std::max(sum.max_streams, v.max_streams);
        sum.latency.merge(v.latency);
        sum.scheduled_latency.merge(v.scheduled_latency);
        sum.successful_latency.merge(v.successful_latency);
        sum.acquire_latency.merge(v.acquire_latency);
        for (std::size_t i = 0; i < kErrorCount; ++i) {
            sum.error_codes[i] += v.error_codes[i];
            sum.close_codes[i] += v.close_codes[i];
        }
    }
    if (sum.started != sum.success + sum.errors + sum.canceled)
        ++violations;
    auto number = [](const char *key, std::uint64_t value) {
        std::printf(",\"%s\":%llu", key, static_cast<unsigned long long>(value));
    };
    std::printf("{\"type\":\"summary\",\"scenario\":\"%s\"", options.lifecycle ? "lifecycle"
                                                             : options.fifo    ? "fifo"
                                                                               : "load");
    number("loops", options.loops);
    number("concurrency_per_loop", options.concurrency);
    number("duration_ms", options.duration_ms);
    number("warmup_ms", options.warmup_ms);
    number("seed", options.seed);
    number("started", sum.started);
    number("success", sum.success);
    number("errors", sum.errors);
    number("canceled", sum.canceled);
    number("loadgen_rejected", sum.rejected);
    number("acquired", sum.acquired);
    number("released", sum.released);
    number("fast_hits", sum.fast_hits);
    number("bytes_verified", sum.bytes);
    number("dials", sum.dials);
    number("dial_ok", sum.dial_ok);
    number("dial_error", sum.dial_error);
    number("dial_canceled", sum.dial_canceled);
    number("dial_failed_callbacks", sum.dial_failed_callbacks);
    number("closed", sum.closed);
    number("goaway", sum.goaway);
    number("max_connections_per_loop", sum.max_connections);
    number("max_connections_per_key", sum.max_group);
    number("max_parallel_dials_per_key", sum.max_dials);
    number("max_active_streams_per_connection", sum.max_streams);
    number("measured_success", sum.measured_success);
    number("clears", clears);
    number("lifecycle_rounds", lifecycle_rounds);
    number("fifo_rounds_per_loop", options.fifo ? options.rounds : 0);
    number("violations", violations.load());
    number("latency_p50_us", sum.latency.percentile(500));
    number("latency_p99_us", sum.latency.percentile(990));
    number("latency_p999_us", sum.latency.percentile(999));
    number("latency_max_us", sum.latency.max_us);
    number("scheduled_p99_us", sum.scheduled_latency.percentile(990));
    number("success_p99_us", sum.successful_latency.percentile(990));
    number("acquire_p99_us", sum.acquire_latency.percentile(990));
    number("admin_max_us", admin_latency.max_us);
    std::printf(",\"measured_rps\":%.2f,\"error_codes\":{", sum.measured_success * 1000.0 / options.duration_ms);
    bool first = true;
    for (std::size_t i = 0; i < kErrorCount; ++i) {
        if (!sum.error_codes[i])
            continue;
        const auto name = common::io_err_name(static_cast<IoErr>(i));
        std::printf("%s\"%.*s\":%llu", first ? "" : ",", static_cast<int>(name.size()), name.data(),
                    static_cast<unsigned long long>(sum.error_codes[i]));
        first = false;
    }
    std::printf("},\"close_codes\":{");
    first = true;
    for (std::size_t i = 0; i < kErrorCount; ++i) {
        if (!sum.close_codes[i])
            continue;
        const auto name = common::io_err_name(static_cast<IoErr>(i));
        std::printf("%s\"%.*s\":%llu", first ? "" : ",", static_cast<int>(name.size()), name.data(),
                    static_cast<unsigned long long>(sum.close_codes[i]));
        first = false;
    }
    bool accepted_errors = true;
    for (std::size_t i = 0; i < kErrorCount; ++i) {
        if (!sum.error_codes[i])
            continue;
        const auto error = static_cast<IoErr>(i);
        accepted_errors &= error == IoErr::TimedOut || error == IoErr::ConnReset || error == IoErr::ConnAborted ||
                           error == IoErr::ConnRefused || error == IoErr::NotConnected || error == IoErr::BrokenPipe ||
                           error == IoErr::Busy || error == IoErr::Invalid;
    }
    const bool pass = violations.load() == 0 && (sum.errors == 0 || (options.allow_errors && accepted_errors)) &&
                      (options.lifecycle || options.fifo || sum.success > 0 || options.bad_trust);
    std::printf("},\"pass\":%s}\n", pass ? "true" : "false");
    std::fflush(stdout);
    return pass;
}
} // namespace

int main(int argc, char **argv) {
    Options options;
    if (!parse(argc, argv, options)) {
        std::fputs("Invalid arguments; use --help\n", stderr);
        return 2;
    }
    Benchmark bench(std::move(options));
    if (!bench.pool.init()) {
        std::fputs("pool.init failed\n", stderr);
        return 2;
    }
    bench.start = Clock::now() + 100ms;
    bench.measure_start = bench.start + Ms(bench.options.warmup_ms);
    bench.finish = bench.measure_start + Ms(bench.options.duration_ms);
    bench.group.start();
    for (auto &s: bench.shards)
        async::spawn(bench.group.at(s->index), [p = s.get()]() { return p->monitor(); });
    async::spawn(bench.group.at(0), [&bench]() { return bench.control(); });
    // Independent thread watchdog also covers a completely blocked EventLoop.
    const auto watchdog =
            Clock::now() +
            (bench.options.lifecycle || bench.options.fifo
                     ? Ms(30000 + bench.options.rounds * 2000)
                     : Ms(bench.options.duration_ms + bench.options.warmup_ms + bench.options.timeout_ms + 30000));
    while (!bench.done.load()) {
        if (Clock::now() > watchdog) {
            std::fputs("WATCHDOG: benchmark did not drain\n", stderr);
            std::fflush(nullptr);
            std::abort();
        }
        std::this_thread::sleep_for(20ms);
    }
    bench.group.stop();
    bench.group.join();
    return bench.summary() ? 0 : 1;
}
