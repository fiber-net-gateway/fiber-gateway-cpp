#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <event/EventLoop.h>

#include "CatClientCore.h"
#include "CatInternal.h"

namespace {

using namespace std::chrono_literals;
using fiber::cat::RecordError;
using fiber::cat::detail::CatClientCore;
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

class CapturingCore final : public CatClientCore {
public:
    explicit CapturingCore(ClientEncodeContext context) noexcept : context_(context) {}

    [[nodiscard]] ClientEncodeContext encode_context() const noexcept override { return context_; }

    void submit_encoded(fiber::mem::IoBuf message) noexcept override {
        encoded = std::move(message);
        ++submission_count;
    }

    void on_encode_failure(EncodeError error) noexcept override {
        last_error = error;
        ++failure_count;
    }

    fiber::mem::IoBuf encoded;
    EncodeError last_error = EncodeError::InvalidTrace;
    std::size_t submission_count = 0;
    std::size_t failure_count = 0;

private:
    ClientEncodeContext context_;
};

std::vector<std::uint8_t> encoded_bytes(const CapturingCore &core) {
    if (!core.encoded) {
        return {};
    }
    const std::uint8_t *begin = core.encoded.readable_data();
    return {begin, begin + core.encoded.readable()};
}

void expect_bytes(const CapturingCore &core, const std::vector<std::uint8_t> &expected) {
    const auto actual = encoded_bytes(core);
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

TEST(CatEncoderTest, EncodesEventRootAsOfficialNt1Frame) {
    run_on_loop([] {
        auto core = std::make_shared<CapturingCore>(full_context());
        TraceContext context{
                .core = core,
                .message_id = "m",
                .root_message_id = "r",
                .parent_message_id = "p",
                .session_token = "s",
        };
        auto created = fiber::cat::detail::create_event_root("E", "n", {}, std::move(context));
        ASSERT_TRUE(created);
        auto *event = *created;
        make_time_deterministic(*event->trace->data, 123);
        event->time = std::chrono::steady_clock::time_point{};

        EXPECT_EQ(fiber::cat::detail::complete(event), RecordError::None);
        EXPECT_EQ(event, nullptr);
        ASSERT_EQ(core->submission_count, 1);
        EXPECT_EQ(core->failure_count, 0);

        const std::vector<std::uint8_t> expected{
                0x00, 0x00, 0x00, 0x33, 'N', 'T',  '1',  0x03, 'a',  'p', 'p',  0x04, 'h',  'o',
                's',  't',  0x07, '1',  '.', '2',  '.',  '3',  '.',  '4', 0x05, 'g',  'r',  'o',
                'u',  'p',  0x02, '4',  '2', 0x04, 'l',  'o',  'o',  'p', 0x01, 'm',  0x01, 'p',
                0x01, 'r',  0x01, 's',  'E', 0x7b, 0x01, 'E',  0x01, 'n', 0x01, '0',  0x00,
        };
        expect_bytes(*core, expected);
    });
}

TEST(CatEncoderTest, EncodesNestedTransactionVarintsAndChunkedData) {
    run_on_loop([] {
        auto core = std::make_shared<CapturingCore>(minimal_context());
        auto created = fiber::cat::detail::create_transaction_root("T", "root", {}, TraceContext{.core = core});
        ASSERT_TRUE(created);
        auto *root = *created;
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

        EXPECT_EQ(fiber::cat::detail::complete(root), RecordError::None);
        EXPECT_EQ(core->submission_count, 0);
        EXPECT_EQ(fiber::cat::detail::complete(child), RecordError::None);
        ASSERT_EQ(core->submission_count, 1);
        EXPECT_EQ(core->failure_count, 0);

        std::vector<std::uint8_t> expected{
                0x00, 0x00, 0x00, 0xb4, 'N',  'T',  '1',  0x01, 'a',  0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 't',  0xac, 0x02, 0x01, 'T',  0x04, 'r',  'o',  'o',  't',  'E',  0xad,
                0x02, 0x01, 'E',  0x05, 'c',  'h',  'i',  'l',  'd',  0x03, 'E',  'R',  'R',  0x82, 0x01,
        };
        expected.insert(expected.end(), 128, 'x');
        expected.push_back('&');
        expected.push_back('y');
        expected.insert(expected.end(), {'T', 0x01, '0', 0x03, 'k', '=', 'v', 0xdc, 0x0b});
        expect_bytes(*core, expected);
    });
}

TEST(CatEncoderTest, EncodesChildrenAcrossFixedChunkBoundaryInOrder) {
    run_on_loop([] {
        auto core = std::make_shared<CapturingCore>(minimal_context());
        auto created = fiber::cat::detail::create_transaction_root("T", "r", {}, TraceContext{.core = core});
        ASSERT_TRUE(created);
        auto *root = *created;
        make_time_deterministic(*root->trace->data, 0);
        root->time = std::chrono::steady_clock::time_point{};
        ASSERT_EQ(fiber::cat::detail::set_duration(root, 0us), RecordError::None);

        for (char name = 'A'; name <= 'Q'; ++name) {
            const std::string_view child_name(&name, 1);
            auto child_created = fiber::cat::detail::create_event(*root, "E", child_name);
            ASSERT_TRUE(child_created);
            auto *child = *child_created;
            child->time = std::chrono::steady_clock::time_point{};
            ASSERT_EQ(fiber::cat::detail::complete(child), RecordError::None);
        }
        ASSERT_EQ(fiber::cat::detail::complete(root), RecordError::None);
        ASSERT_EQ(core->submission_count, 1);

        std::vector<std::uint8_t> expected{
                0x00, 0x00, 0x00, 0xb2, 'N',  'T',  '1', 0x01, 'a',  0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 't', 0x00, 0x01, 'T',  0x01, 'r',
        };
        for (char name = 'A'; name <= 'Q'; ++name) {
            expected.insert(expected.end(),
                            {'E', 0x00, 0x01, 'E', 0x01, static_cast<std::uint8_t>(name), 0x01, '0', 0x00});
        }
        expected.insert(expected.end(), {'T', 0x01, '0', 0x00, 0x00});
        expect_bytes(*core, expected);
    });
}

TEST(CatEncoderTest, Nt1TreatsAbsentAndExplicitEmptyDataEqually) {
    run_on_loop([] {
        auto encode = [](bool add_empty) {
            auto core = std::make_shared<CapturingCore>(minimal_context());
            auto created = fiber::cat::detail::create_event_root("E", "n", {}, TraceContext{.core = core});
            EXPECT_TRUE(created);
            if (!created) {
                return std::vector<std::uint8_t>{};
            }
            auto *event = *created;
            make_time_deterministic(*event->trace->data, 0);
            event->time = std::chrono::steady_clock::time_point{};
            if (add_empty) {
                EXPECT_EQ(fiber::cat::detail::add_data(event, ""), RecordError::None);
            }
            EXPECT_EQ(fiber::cat::detail::complete(event), RecordError::None);
            return encoded_bytes(*core);
        };

        EXPECT_EQ(encode(false), encode(true));
    });
}

TEST(CatEncoderTest, ReportsInvalidTraceWithoutSubmittingPartialFrame) {
    run_on_loop([] {
        auto core = std::make_shared<CapturingCore>(minimal_context());
        auto created = fiber::cat::detail::create_event_root("E", "invalid", {}, TraceContext{.core = core});
        ASSERT_TRUE(created);
        auto *event = *created;
        make_time_deterministic(*event->trace->data, 0);
        event->time = std::chrono::steady_clock::time_point{};
        event->trace->data->message_count = 2;

        EXPECT_EQ(fiber::cat::detail::complete(event), RecordError::None);
        EXPECT_EQ(event, nullptr);
        EXPECT_EQ(core->submission_count, 0);
        EXPECT_EQ(core->failure_count, 1);
        EXPECT_EQ(core->last_error, EncodeError::InvalidTrace);
        EXPECT_FALSE(core->encoded);
    });
}

} // namespace
