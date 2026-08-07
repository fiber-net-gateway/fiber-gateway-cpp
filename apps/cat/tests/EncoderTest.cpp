#include <gtest/gtest.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include <fiber/event/EventLoop.h>

#include "CatEncoder.h"
#include "CatInternal.h"
#include "CatSystemMessage.h"
#include "CatSystemStats.h"

namespace {

using namespace std::chrono_literals;
using fiber::cat::RecordError;
using fiber::cat::detail::ClientEncodeContext;
using fiber::cat::detail::EncodeError;
using fiber::cat::detail::TraceContext;

template<typename F>
struct LoopCall {
    F callback;
    fiber::event::EventLoop *loop = nullptr;
    fiber::event::EventLoop::NotifyEntry entry{};

    static void run(LoopCall *call) noexcept {
        call->callback();
        call->loop->stop();
    }
};

template<typename F>
void run_on_loop(F &&callback) {
    fiber::event::EventLoop loop;
    LoopCall<std::decay_t<F>> call{.callback = std::forward<F>(callback), .loop = &loop};
    loop.post<LoopCall<std::decay_t<F>>, &LoopCall<std::decay_t<F>>::entry, &LoopCall<std::decay_t<F>>::run>(call);
    loop.run();
}

std::vector<std::uint8_t> encoded_bytes(const fiber::mem::IoBuf &encoded) {
    if (!encoded) {
        return {};
    }
    const std::uint8_t *begin = encoded.readable_data();
    return {begin, begin + encoded.readable()};
}

void expect_bytes(const fiber::mem::IoBuf &encoded, const std::vector<std::uint8_t> &expected) {
    const auto actual = encoded_bytes(encoded);
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        EXPECT_EQ(actual[index], expected[index]) << "byte index " << index;
    }
}

ClientEncodeContext full_context() noexcept {
    return {
            .app_key = "app",
            .hostname = "host",
            .ip = "1.2.3.4",
            .thread_group_name = "group",
            .thread_id = "42",
            .thread_name = "loop",
    };
}

ClientEncodeContext minimal_context() noexcept { return {.app_key = "a"}; }

void make_time_deterministic(fiber::cat::detail::MessageTraceData &trace, std::uint64_t wall_millis) {
    trace.steady_base = std::chrono::steady_clock::time_point{};
    trace.wall_base_millis = wall_millis;
}

void freeze_message(fiber::cat::detail::MessageData &message) {
    message.completed = true;
    if (message.kind != fiber::cat::MessageKind::Transaction) {
        return;
    }
    auto &transaction = static_cast<fiber::cat::detail::TransactionData &>(message);
    std::size_t child_index = 0;
    for (auto *chunk = transaction.children_head; chunk; chunk = chunk->next) {
        const std::size_t chunk_children =
                std::min(fiber::cat::detail::kChildrenPerChunk, transaction.child_count - child_index);
        for (std::size_t index = 0; index < chunk_children; ++index) {
            freeze_message(*chunk->children[index]);
        }
        child_index += chunk_children;
    }
}

void freeze_trace(fiber::cat::detail::MessageTrace &trace) {
    ASSERT_NE(trace.data, nullptr);
    ASSERT_NE(trace.data->root, nullptr);
    freeze_message(*trace.data->root);
    trace.data->open_message_count = 0;
}

TEST(CatEncoderTest, EncodesEventRootAsOfficialNt1Frame) {
    run_on_loop([] {
        fiber::mem::BufPool pool;
        TraceContext context{
                .propagation_context =
                        {
                                .message_id = "m",
                                .root_message_id = "r",
                                .parent_message_id = "p",
                                .session_token = "s",
                        },
        };
        auto created = fiber::cat::detail::create_event_root(pool, "old-type", "old-name", {}, std::move(context));
        ASSERT_TRUE(created);
        auto *event = *created;
        auto *trace = event->trace;
        ASSERT_EQ(fiber::cat::detail::set_type(event, "E"), RecordError::None);
        ASSERT_EQ(fiber::cat::detail::set_name(event, "n"), RecordError::None);
        make_time_deterministic(*event->trace->data, 123);
        event->time = std::chrono::steady_clock::time_point{};
        freeze_trace(*trace);
        auto encoded = fiber::cat::detail::encode_nt1(*trace->data, full_context());
        ASSERT_TRUE(encoded);

        const std::vector<std::uint8_t> expected{
                0x00, 0x00, 0x00, 0x33, 'N', 'T',  '1',  0x03, 'a',  'p', 'p',  0x04, 'h',  'o',
                's',  't',  0x07, '1',  '.', '2',  '.',  '3',  '.',  '4', 0x05, 'g',  'r',  'o',
                'u',  'p',  0x02, '4',  '2', 0x04, 'l',  'o',  'o',  'p', 0x01, 'm',  0x01, 'p',
                0x01, 'r',  0x01, 's',  'E', 0x7b, 0x01, 'E',  0x01, 'n', 0x01, '0',  0x00,
        };
        expect_bytes(*encoded, expected);
        fiber::cat::detail::discard_message_trace(trace);
    });
}

TEST(CatEncoderTest, EncodesMetricRootAsOfficialNt1Frame) {
    run_on_loop([] {
        fiber::mem::BufPool pool;
        TraceContext context{
                .propagation_context =
                        {
                                .message_id = "m",
                                .root_message_id = "r",
                                .parent_message_id = "p",
                                .session_token = "s",
                        },
        };
        auto created = fiber::cat::detail::create_metric_root(pool, "", "requests", {}, std::move(context));
        ASSERT_TRUE(created);
        auto *metric = *created;
        auto *trace = metric->trace;
        make_time_deterministic(*trace->data, 123);
        metric->time = std::chrono::steady_clock::time_point{};
        ASSERT_EQ(fiber::cat::detail::set_status(metric, "C"), RecordError::None);
        ASSERT_EQ(fiber::cat::detail::add_data(metric, "-3"), RecordError::None);
        freeze_trace(*trace);
        auto encoded = fiber::cat::detail::encode_nt1(*trace->data, full_context());
        ASSERT_TRUE(encoded);

        const std::vector<std::uint8_t> expected{
                0x00, 0x00, 0x00, 0x3b, 'N', 'T', '1',  0x03, 'a',  'p', 'p',  0x04, 'h',  'o', 's',  't',
                0x07, '1',  '.',  '2',  '.', '3', '.',  '4',  0x05, 'g', 'r',  'o',  'u',  'p', 0x02, '4',
                '2',  0x04, 'l',  'o',  'o', 'p', 0x01, 'm',  0x01, 'p', 0x01, 'r',  0x01, 's', 'M',  0x7b,
                0x00, 0x08, 'r',  'e',  'q', 'u', 'e',  's',  't',  's', 0x01, 'C',  0x02, '-', '3',
        };
        expect_bytes(*encoded, expected);
        fiber::cat::detail::discard_message_trace(trace);
    });
}

TEST(CatEncoderTest, EncodesHeartbeatRootAsOfficialNt1Frame) {
    run_on_loop([] {
        fiber::mem::BufPool pool;
        TraceContext context{
                .propagation_context =
                        {
                                .message_id = "m",
                                .root_message_id = "r",
                                .parent_message_id = "p",
                                .session_token = "s",
                        },
        };
        auto created = fiber::cat::detail::create_heartbeat_root(pool, "Heartbeat", "1.2.3.4", {}, std::move(context));
        ASSERT_TRUE(created);
        auto *heartbeat = *created;
        auto *trace = heartbeat->trace;
        make_time_deterministic(*trace->data, 123);
        heartbeat->time = std::chrono::steady_clock::time_point{};
        ASSERT_EQ(fiber::cat::detail::add_data(heartbeat, "x"), RecordError::None);
        freeze_trace(*trace);
        auto encoded = fiber::cat::detail::encode_nt1(*trace->data, full_context());
        ASSERT_TRUE(encoded);

        const std::vector<std::uint8_t> expected{
                0x00, 0x00, 0x00, 0x42, 'N',  'T', '1',  0x03, 'a',  'p', 'p', 0x04, 'h',  'o', 's',  't',  0x07, '1',
                '.',  '2',  '.',  '3',  '.',  '4', 0x05, 'g',  'r',  'o', 'u', 'p',  0x02, '4', '2',  0x04, 'l',  'o',
                'o',  'p',  0x01, 'm',  0x01, 'p', 0x01, 'r',  0x01, 's', 'H', 0x7b, 0x09, 'H', 'e',  'a',  'r',  't',
                'b',  'e',  'a',  't',  0x07, '1', '.',  '2',  '.',  '3', '.', '4',  0x01, '0', 0x01, 'x',
        };
        expect_bytes(*encoded, expected);
        fiber::cat::detail::discard_message_trace(trace);
    });
}

TEST(CatEncoderTest, EncodesOfficialAndProcessHeartbeatStatistics) {
    run_on_loop([] {
        fiber::cat::detail::HeartbeatSystemStats system{
                .load_1min = 1.25,
                .load_5min = 0.50,
                .load_15min = 0.10,
                .cpu_delta = {.user = 10, .nice = 1, .system = 5, .idle = 80, .iowait = 2, .irq = 1, .softirq = 1},
                .context_switches_delta = 30,
                .interrupts_delta = 20,
                .processes_running = 3,
                .processes_blocked = 1,
                .memory_total_bytes = 1024,
                .memory_free_bytes = 256,
                .memory_cached_bytes = 128,
                .swap_total_bytes = 512,
                .swap_free_bytes = 400,
                .process_virtual_bytes = 4096,
                .process_rss_bytes = 2048,
                .cpu_user_percent = 10.0,
                .cpu_nice_percent = 1.0,
                .cpu_system_percent = 5.0,
                .cpu_idle_percent = 80.0,
                .cpu_iowait_percent = 2.0,
                .cpu_irq_percent = 1.0,
                .cpu_softirq_percent = 1.0,
                .process_cpu_user_percent = 4.0,
                .process_cpu_system_percent = 1.0,
                .process_cpu_total_percent = 5.0,
                .memory_free_percent = 25.0,
                .memory_used_percent = 75.0,
                .load_valid = true,
                .cpu_valid = true,
                .scheduler_valid = true,
                .scheduler_delta_valid = true,
                .memory_valid = true,
                .process_cpu_valid = true,
                .process_memory_valid = true,
        };
        fiber::cat::detail::HeartbeatInfo info{
                .ip = "1.2.3.4",
                .system_stats = &system,
        };
        auto encoded = fiber::cat::detail::encode_heartbeat_nt1(full_context(), "message", info, 96, 16 * 1024);
        ASSERT_TRUE(encoded);
        const auto bytes = encoded_bytes(*encoded);
        const auto contains = [&](std::string_view value) {
            return std::search(bytes.begin(), bytes.end(), value.begin(), value.end()) != bytes.end();
        };
        EXPECT_TRUE(contains("extension id=\"system.process\""));
        EXPECT_TRUE(contains("id=\"cpu.user.percent\" value=\"10.00\""));
        EXPECT_TRUE(contains("id=\"mem.memtotal\" value=\"1024\""));
        EXPECT_TRUE(contains("id=\"process.rss.bytes\" value=\"2048\""));
        EXPECT_TRUE(contains("id=\"process.cpu.total.percent\" value=\"5.00\""));

        const std::string_view contents(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        std::size_t position = 0;
        std::size_t value_count = 0;
        while ((position = contents.find("value=\"", position)) != std::string_view::npos) {
            position += 7;
            const std::size_t end = contents.find('"', position);
            ASSERT_NE(end, std::string_view::npos);
            const std::string_view value = contents.substr(position, end - position);
            double parsed = 0.0;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
            EXPECT_EQ(result.ec, std::errc{}) << value;
            EXPECT_EQ(result.ptr, value.data() + value.size()) << value;
            EXPECT_TRUE(std::isfinite(parsed)) << value;
            ++value_count;
            position = end + 1;
        }
        EXPECT_GT(value_count, 0);
        EXPECT_FALSE(contains("app.key"));
        EXPECT_FALSE(contains("host.name"));
        EXPECT_FALSE(contains("host.ip"));
        EXPECT_FALSE(contains("client.version"));
        EXPECT_FALSE(contains("value=\"true\""));
        EXPECT_FALSE(contains("value=\"false\""));
    });
}

TEST(CatEncoderTest, OmitsOptionalSystemStatisticsWhenFieldBudgetIsExhausted) {
    run_on_loop([] {
        fiber::cat::detail::HeartbeatSystemStats system{
                .load_1min = 1.0,
                .load_5min = 1.0,
                .load_15min = 1.0,
                .load_valid = true,
        };
        fiber::cat::detail::HeartbeatInfo info{
                .ip = "1.2.3.4",
                .system_stats = &system,
        };
        auto encoded = fiber::cat::detail::encode_heartbeat_nt1(full_context(), "message", info, 26, 16 * 1024);
        ASSERT_TRUE(encoded);
        const auto bytes = encoded_bytes(*encoded);
        const auto contains = [&](std::string_view value) {
            return std::search(bytes.begin(), bytes.end(), value.begin(), value.end()) != bytes.end();
        };
        EXPECT_TRUE(contains("fiber2.cat"));
        EXPECT_FALSE(contains("system.process"));
    });
}

TEST(CatEncoderTest, EncodesNestedTransactionVarintsAndChunkedData) {
    run_on_loop([] {
        fiber::mem::BufPool pool;
        auto created = fiber::cat::detail::create_transaction_root(pool, "T", "root", {});
        ASSERT_TRUE(created);
        auto *root = *created;
        auto *trace = root->trace;
        make_time_deterministic(*root->trace->data, 300);
        root->time = std::chrono::steady_clock::time_point{};
        ASSERT_EQ(fiber::cat::detail::set_duration(root, 1500us), RecordError::None);
        ASSERT_EQ(fiber::cat::detail::add_data(root, "k=v"), RecordError::None);

        auto child_created = fiber::cat::detail::create_event(*root, "E", "child");
        ASSERT_TRUE(child_created);
        auto *child = *child_created;
        child->time = std::chrono::steady_clock::time_point(1ms);
        ASSERT_EQ(fiber::cat::detail::set_status(child, "ERR"), RecordError::None);
        const std::string full_chunk(128, 'x');
        ASSERT_EQ(fiber::cat::detail::add_data(child, full_chunk), RecordError::None);
        ASSERT_EQ(fiber::cat::detail::add_data(child, "y"), RecordError::None);

        freeze_trace(*trace);
        auto encoded = fiber::cat::detail::encode_nt1(*trace->data, minimal_context());
        ASSERT_TRUE(encoded);

        std::vector<std::uint8_t> expected{
                0x00, 0x00, 0x00, 0xb4, 'N',  'T',  '1',  0x01, 'a',  0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 't',  0xac, 0x02, 0x01, 'T',  0x04, 'r',  'o',  'o',  't',  'E',  0xad,
                0x02, 0x01, 'E',  0x05, 'c',  'h',  'i',  'l',  'd',  0x03, 'E',  'R',  'R',  0x82, 0x01,
        };
        expected.insert(expected.end(), 128, 'x');
        expected.push_back('&');
        expected.push_back('y');
        expected.insert(expected.end(), {'T', 0x01, '0', 0x03, 'k', '=', 'v', 0xdc, 0x0b});
        expect_bytes(*encoded, expected);
        fiber::cat::detail::discard_message_trace(trace);
    });
}

TEST(CatEncoderTest, EncodesChildrenAcrossFixedChunkBoundaryInOrder) {
    run_on_loop([] {
        fiber::mem::BufPool pool;
        auto created = fiber::cat::detail::create_transaction_root(pool, "T", "r", {});
        ASSERT_TRUE(created);
        auto *root = *created;
        auto *trace = root->trace;
        make_time_deterministic(*root->trace->data, 0);
        root->time = std::chrono::steady_clock::time_point{};
        ASSERT_EQ(fiber::cat::detail::set_duration(root, 0us), RecordError::None);

        for (char name = 'A'; name <= 'Q'; ++name) {
            const std::string_view child_name(&name, 1);
            auto child_created = fiber::cat::detail::create_event(*root, "E", child_name);
            ASSERT_TRUE(child_created);
            auto *child = *child_created;
            child->time = std::chrono::steady_clock::time_point{};
        }
        freeze_trace(*trace);
        auto encoded = fiber::cat::detail::encode_nt1(*trace->data, minimal_context());
        ASSERT_TRUE(encoded);

        std::vector<std::uint8_t> expected{
                0x00, 0x00, 0x00, 0xb2, 'N',  'T',  '1', 0x01, 'a',  0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 't', 0x00, 0x01, 'T',  0x01, 'r',
        };
        for (char name = 'A'; name <= 'Q'; ++name) {
            expected.insert(expected.end(),
                            {'E', 0x00, 0x01, 'E', 0x01, static_cast<std::uint8_t>(name), 0x01, '0', 0x00});
        }
        expected.insert(expected.end(), {'T', 0x01, '0', 0x00, 0x00});
        expect_bytes(*encoded, expected);
        fiber::cat::detail::discard_message_trace(trace);
    });
}

TEST(CatEncoderTest, Nt1TreatsAbsentAndExplicitEmptyDataEqually) {
    run_on_loop([] {
        auto encode = [](bool add_empty) {
            fiber::mem::BufPool pool;
            auto created = fiber::cat::detail::create_event_root(pool, "E", "n", {});
            EXPECT_TRUE(created);
            if (!created) {
                return std::vector<std::uint8_t>{};
            }
            auto *event = *created;
            auto *trace = event->trace;
            make_time_deterministic(*event->trace->data, 0);
            event->time = std::chrono::steady_clock::time_point{};
            if (add_empty) {
                EXPECT_EQ(fiber::cat::detail::add_data(event, ""), RecordError::None);
            }
            freeze_trace(*trace);
            auto encoded = fiber::cat::detail::encode_nt1(*trace->data, minimal_context());
            EXPECT_TRUE(encoded);
            std::vector<std::uint8_t> result;
            if (encoded) {
                result = encoded_bytes(*encoded);
            }
            fiber::cat::detail::discard_message_trace(trace);
            return result;
        };

        EXPECT_EQ(encode(false), encode(true));
    });
}

TEST(CatEncoderTest, ReportsInvalidTraceWithoutSubmittingPartialFrame) {
    run_on_loop([] {
        fiber::mem::BufPool pool;
        auto created = fiber::cat::detail::create_event_root(pool, "E", "invalid", {});
        ASSERT_TRUE(created);
        auto *event = *created;
        auto *trace = event->trace;
        make_time_deterministic(*event->trace->data, 0);
        event->time = std::chrono::steady_clock::time_point{};
        freeze_trace(*trace);
        trace->data->message_count = 2;

        auto encoded = fiber::cat::detail::encode_nt1(*trace->data, minimal_context());
        ASSERT_FALSE(encoded);
        EXPECT_EQ(encoded.error(), EncodeError::InvalidTrace);
        fiber::cat::detail::discard_message_trace(trace);
    });
}

TEST(CatEncoderTest, InjectsTruncationMarkerAfterTreeLimitWithoutPoolAllocation) {
    run_on_loop([] {
        fiber::cat::RecordLimits limits;
        limits.max_children_per_transaction = 1;
        fiber::mem::BufPool pool;
        auto created = fiber::cat::detail::create_transaction_root(pool, "T", "root", limits);
        ASSERT_TRUE(created);
        auto *root = *created;
        auto *trace = root->trace;
        make_time_deterministic(*trace->data, 0);
        root->time = std::chrono::steady_clock::time_point{};
        ASSERT_EQ(fiber::cat::detail::add_data(root, "base"), RecordError::None);
        ASSERT_TRUE(fiber::cat::detail::create_event(*root, "E", "ok"));
        auto dropped = fiber::cat::detail::create_event(*root, "E", "drop");
        ASSERT_FALSE(dropped);
        EXPECT_EQ(dropped.error(), RecordError::LimitExceeded);
        EXPECT_TRUE(trace->data->truncated);
        EXPECT_TRUE(trace->data->has_problem);
        EXPECT_EQ(trace->data->dropped_message_count, 1);
        EXPECT_EQ(trace->data->dropped_data_bytes, 5);

        freeze_trace(*trace);
        auto encoded = fiber::cat::detail::encode_nt1(*trace->data, minimal_context());
        ASSERT_TRUE(encoded);
        const auto bytes = encoded_bytes(*encoded);
        constexpr std::string_view marker = "base&CatClient.Truncated=count:1,bytes:5,reason:limit";
        EXPECT_NE(std::search(bytes.begin(), bytes.end(), marker.begin(), marker.end()), bytes.end());
        fiber::cat::detail::discard_message_trace(trace);
    });
}

TEST(CatEncoderTest, UsesConfiguredDataSeparatorBeforeTruncationMarker) {
    run_on_loop([] {
        fiber::cat::RecordLimits limits;
        limits.max_data_bytes_per_message = 4;
        fiber::mem::BufPool pool;
        auto created = fiber::cat::detail::create_transaction_root(pool, "T", "root", limits);
        ASSERT_TRUE(created);
        auto *root = *created;
        auto *trace = root->trace;
        make_time_deterministic(*trace->data, 0);
        root->time = std::chrono::steady_clock::time_point{};
        ASSERT_EQ(fiber::cat::detail::set_data_separator(root, ' '), RecordError::None);
        ASSERT_EQ(fiber::cat::detail::add_data(root, "base"), RecordError::None);
        ASSERT_EQ(fiber::cat::detail::add_data(root, "overflow"), RecordError::LimitExceeded);

        freeze_trace(*trace);
        auto encoded = fiber::cat::detail::encode_nt1(*trace->data, minimal_context());
        ASSERT_TRUE(encoded);
        const auto bytes = encoded_bytes(*encoded);
        constexpr std::string_view marker = "base CatClient.Truncated=count:0,bytes:8,reason:limit";
        EXPECT_NE(std::search(bytes.begin(), bytes.end(), marker.begin(), marker.end()), bytes.end());
        fiber::cat::detail::discard_message_trace(trace);
    });
}

TEST(CatEncoderTest, EncodesOfficialPt1NestedTextAndPreservesRawControlCharacters) {
    run_on_loop([] {
        const char *old_timezone = std::getenv("TZ");
        const std::string saved_timezone = old_timezone ? old_timezone : "";
        const bool had_timezone = old_timezone != nullptr;
        ASSERT_EQ(::setenv("TZ", "UTC", 1), 0);
        ::tzset();

        fiber::mem::BufPool pool;
        auto created = fiber::cat::detail::create_transaction_root(pool, "old-type", "old-root", {});
        ASSERT_TRUE(created);
        auto *root = *created;
        auto *trace = root->trace;
        ASSERT_EQ(fiber::cat::detail::set_type(root, "T"), RecordError::None);
        ASSERT_EQ(fiber::cat::detail::set_name(root, "root"), RecordError::None);
        make_time_deterministic(*trace->data, 123);
        root->time = std::chrono::steady_clock::time_point{};
        ASSERT_EQ(fiber::cat::detail::set_duration(root, 1500us), RecordError::None);
        auto child_created = fiber::cat::detail::create_event(*root, "old-event", "old-child");
        ASSERT_TRUE(child_created);
        auto *child = *child_created;
        ASSERT_EQ(fiber::cat::detail::set_type(child, "E"), RecordError::None);
        ASSERT_EQ(fiber::cat::detail::set_name(child, "child"), RecordError::None);
        child->time = std::chrono::steady_clock::time_point(1ms);
        ASSERT_EQ(fiber::cat::detail::set_status(child, "ERR"), RecordError::None);
        ASSERT_EQ(fiber::cat::detail::add_data(child, "a\tb\n\\"), RecordError::None);
        freeze_trace(*trace);

        auto encoded = fiber::cat::detail::encode_pt1(*trace->data, minimal_context());
        ASSERT_TRUE(encoded);
        const std::string payload = "PT1\ta\t\t\t\t\t\t\t\t\t\n"
                                    "t1970-01-01 00:00:00.123\tT\troot\t\n"
                                    "E1970-01-01 00:00:00.124\tE\tchild\tERR\ta\tb\n\\\t\n"
                                    "T1970-01-01 00:00:00.124\tT\troot\t0\t1500us\t\t\n";
        std::vector<std::uint8_t> expected{
                static_cast<std::uint8_t>(payload.size() >> 24U),
                static_cast<std::uint8_t>(payload.size() >> 16U),
                static_cast<std::uint8_t>(payload.size() >> 8U),
                static_cast<std::uint8_t>(payload.size()),
        };
        expected.insert(expected.end(), payload.begin(), payload.end());
        expect_bytes(*encoded, expected);
        fiber::cat::detail::discard_message_trace(trace);

        if (had_timezone) {
            ASSERT_EQ(::setenv("TZ", saved_timezone.c_str(), 1), 0);
        } else {
            ASSERT_EQ(::unsetenv("TZ"), 0);
        }
        ::tzset();
    });
}

} // namespace
