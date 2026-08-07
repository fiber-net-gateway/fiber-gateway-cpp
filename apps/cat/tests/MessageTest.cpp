#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <type_traits>
#include <utility>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/cat/Cat.h>
#include <fiber/common/mem/BufPool.h>
#include <fiber/event/EventLoop.h>

#include "CatInternal.h"

namespace {

using namespace std::chrono_literals;
using fiber::cat::Event;
using fiber::cat::RecordError;
using fiber::cat::RecordLimits;
using fiber::cat::Transaction;

static_assert(sizeof(Transaction) == sizeof(void *));
static_assert(sizeof(Event) == sizeof(void *));
static_assert(!std::is_copy_constructible_v<Transaction>);
static_assert(!std::is_copy_constructible_v<Event>);

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

std::string flatten_data(const fiber::cat::detail::MessageData &message) {
    std::string result;
    result.reserve(message.data_size);
    for (const auto *chunk = message.data_head; chunk; chunk = chunk->next) {
        result.append(chunk->data(), chunk->used);
    }
    return result;
}

std::expected<Transaction, RecordError> create_transaction_root(fiber::mem::BufPool &pool, std::string_view type,
                                                                std::string_view name,
                                                                RecordLimits limits = {}) noexcept {
    auto created = fiber::cat::detail::create_transaction_root(pool, type, name, limits);
    if (!created) {
        return std::unexpected(created.error());
    }
    return fiber::cat::detail::MessageHandleAccess::transaction(*created);
}

std::expected<Event, RecordError> create_event_root(fiber::mem::BufPool &pool, std::string_view type,
                                                    std::string_view name, RecordLimits limits = {}) noexcept {
    auto created = fiber::cat::detail::create_event_root(pool, type, name, limits);
    if (!created) {
        return std::unexpected(created.error());
    }
    return fiber::cat::detail::MessageHandleAccess::event(*created);
}

TEST(CatMessageTest, BuildsNestedMessagesAndConsumesCompletedHandles) {
    run_on_loop([] {
        fiber::mem::BufPool pool;
        auto root_result = create_transaction_root(pool, "URL", "/orders");
        ASSERT_TRUE(root_result);
        Transaction root = std::move(*root_result);
        EXPECT_EQ(root.add_data("method=GET"), RecordError::None);
        EXPECT_EQ(root.add_data("route", "/orders"), RecordError::None);

        auto child_result = root.start_transaction("SQL", "select-order");
        ASSERT_TRUE(child_result);
        Transaction child = std::move(*child_result);
        EXPECT_EQ(child.set_duration(1500us), RecordError::None);
        EXPECT_EQ(child.log_event("Cache", "miss", "CACHE_MISS", "key=42"), RecordError::None);
        EXPECT_EQ(child.complete(), RecordError::None);
        EXPECT_FALSE(child.valid());

        EXPECT_EQ(root.complete(), RecordError::None);
        EXPECT_FALSE(root.valid());
        EXPECT_EQ(root.complete(), RecordError::None);
        EXPECT_EQ(root.add_data("late"), RecordError::Completed);
        auto late_child = root.start_event("E", "late");
        ASSERT_FALSE(late_child);
        EXPECT_EQ(late_child.error(), RecordError::Completed);
    });
}

TEST(CatMessageTest, RenamesTransactionsAndEventsBeforeCompletion) {
    run_on_loop([] {
        fiber::mem::BufPool pool;
        auto root_result = create_transaction_root(pool, "URL", "/v1/chat/completions");
        ASSERT_TRUE(root_result);
        Transaction root = std::move(*root_result);
        EXPECT_EQ(root.set_type("LLM"), RecordError::None);
        EXPECT_EQ(root.set_name("/v1/chat/completions:gpt-5.5"), RecordError::None);

        auto event_result = root.start_event("Attempt", "pending");
        ASSERT_TRUE(event_result);
        Event event = std::move(*event_result);
        EXPECT_EQ(event.set_type("ProviderAttempt"), RecordError::None);
        EXPECT_EQ(event.set_name("primary"), RecordError::None);
        EXPECT_EQ(event.complete(), RecordError::None);

        EXPECT_EQ(root.complete(), RecordError::None);
        EXPECT_EQ(root.set_type("late"), RecordError::Completed);
        EXPECT_EQ(root.set_name("late"), RecordError::Completed);
        EXPECT_EQ(event.set_type("late"), RecordError::Completed);
        EXPECT_EQ(event.set_name("late"), RecordError::Completed);
    });
}

TEST(CatMessageTest, RenameLimitsAreAtomicAndChargeOnlyChangedNonEmptyValues) {
    run_on_loop([] {
        RecordLimits limits;
        limits.max_type_bytes = 4;
        limits.max_name_bytes = 8;
        fiber::mem::BufPool pool;
        auto root_result = fiber::cat::detail::create_transaction_root(pool, "URL", "old", limits);
        ASSERT_TRUE(root_result);
        auto *root = *root_result;
        auto *trace = root->trace;

        const std::size_t initial_payload = trace->data->payload_bytes;
        EXPECT_EQ(fiber::cat::detail::set_type(root, "URL"), RecordError::None);
        EXPECT_EQ(fiber::cat::detail::set_name(root, "old"), RecordError::None);
        EXPECT_EQ(trace->data->payload_bytes, initial_payload);

        EXPECT_EQ(fiber::cat::detail::set_type(root, "LLM"), RecordError::None);
        EXPECT_EQ(fiber::cat::detail::set_name(root, "renamed"), RecordError::None);
        EXPECT_EQ(root->type, "LLM");
        EXPECT_EQ(root->name, "renamed");
        EXPECT_EQ(trace->data->payload_bytes, initial_payload + 3 + 7);

        const std::size_t renamed_payload = trace->data->payload_bytes;
        EXPECT_EQ(fiber::cat::detail::set_type(root, "model"), RecordError::LimitExceeded);
        EXPECT_EQ(fiber::cat::detail::set_name(root, "too-long!"), RecordError::LimitExceeded);
        EXPECT_EQ(root->type, "LLM");
        EXPECT_EQ(root->name, "renamed");
        EXPECT_EQ(trace->data->payload_bytes, renamed_payload);

        EXPECT_EQ(fiber::cat::detail::set_type(root, ""), RecordError::None);
        EXPECT_EQ(fiber::cat::detail::set_name(root, ""), RecordError::None);
        EXPECT_TRUE(root->type.empty());
        EXPECT_TRUE(root->name.empty());
        EXPECT_EQ(trace->data->payload_bytes, renamed_payload);
        EXPECT_EQ(fiber::cat::detail::complete(root), RecordError::None);
    });
}

TEST(CatMessageTest, RootCanCompleteBeforeExistingChild) {
    run_on_loop([] {
        fiber::mem::BufPool pool;
        auto root_result = create_transaction_root(pool, "URL", "/parallel");
        ASSERT_TRUE(root_result);
        Transaction root = std::move(*root_result);
        fiber::cat::MessageTrace trace = root.message_trace();
        auto child_result = root.start_transaction("Call", "backend");
        ASSERT_TRUE(child_result);
        Transaction child = std::move(*child_result);

        EXPECT_EQ(root.complete(), RecordError::None);
        EXPECT_FALSE(root.valid());
        EXPECT_TRUE(trace.valid());
        EXPECT_TRUE(child.valid());
        EXPECT_EQ(child.add_data("phase", "after-root"), RecordError::None);
        EXPECT_EQ(child.complete(), RecordError::None);
        EXPECT_FALSE(child.valid());
        EXPECT_FALSE(trace.valid());
    });
}

TEST(CatMessageTest, CompletingOneTraceDoesNotResetSharedCallerPool) {
    run_on_loop([] {
        fiber::mem::BufPool pool;
        auto first_result = create_transaction_root(pool, "URL", "/first");
        auto second_result = create_transaction_root(pool, "URL", "/second");
        ASSERT_TRUE(first_result);
        ASSERT_TRUE(second_result);
        Transaction first = std::move(*first_result);
        Transaction second = std::move(*second_result);

        EXPECT_EQ(first.complete(), RecordError::None);
        EXPECT_FALSE(first.valid());
        EXPECT_TRUE(second.valid());
        EXPECT_EQ(second.add_data("still", "alive"), RecordError::None);
        EXPECT_EQ(second.complete(), RecordError::None);
        EXPECT_FALSE(second.valid());
    });
}

TEST(CatMessageTest, AbandonedChildCompletesIncompleteWithoutInvalidatingParent) {
    run_on_loop([] {
        fiber::mem::BufPool pool;
        auto root_result = create_transaction_root(pool, "URL", "/incomplete");
        ASSERT_TRUE(root_result);
        Transaction root = std::move(*root_result);
        {
            auto child_result = root.start_transaction("Call", "abandoned");
            ASSERT_TRUE(child_result);
            Transaction child = std::move(*child_result);
            EXPECT_TRUE(child.valid());
        }

        EXPECT_TRUE(root.valid());
        EXPECT_EQ(root.add_data("continued"), RecordError::None);
        EXPECT_EQ(root.complete(), RecordError::None);
    });
}

TEST(CatMessageTest, AbandonMarksInternalMessageIncomplete) {
    run_on_loop([] {
        fiber::mem::BufPool pool;
        auto root_result = fiber::cat::detail::create_transaction_root(pool, "T", "root", {});
        ASSERT_TRUE(root_result);
        auto *root = *root_result;
        auto child_result = fiber::cat::detail::create_event(*root, "E", "abandoned");
        ASSERT_TRUE(child_result);
        auto *child = *child_result;
        auto *observed_child = child;

        fiber::cat::detail::abandon(child);
        EXPECT_EQ(child, nullptr);
        EXPECT_TRUE(observed_child->completed);
        EXPECT_EQ(observed_child->status, fiber::cat::status::Incomplete);
        EXPECT_TRUE(root->trace->data->has_problem);

        EXPECT_EQ(fiber::cat::detail::complete(root), RecordError::None);
    });
}

TEST(CatMessageTest, CompletionPreservesExplicitTransactionDuration) {
    run_on_loop([] {
        fiber::mem::BufPool pool;
        auto root_result = fiber::cat::detail::create_transaction_root(pool, "T", "root", {});
        ASSERT_TRUE(root_result);
        auto *root = *root_result;
        auto child_result = fiber::cat::detail::create_event(*root, "E", "keep-trace-alive");
        ASSERT_TRUE(child_result);
        auto *child = *child_result;
        auto *observed_root = root;

        EXPECT_EQ(fiber::cat::detail::set_duration(root, 1500us), RecordError::None);
        EXPECT_EQ(fiber::cat::detail::complete(root), RecordError::None);
        EXPECT_EQ(root, nullptr);
        EXPECT_TRUE(observed_root->completed);
        EXPECT_TRUE(observed_root->explicit_duration);
        EXPECT_EQ(observed_root->duration, 1500us);

        EXPECT_EQ(fiber::cat::detail::complete(child), RecordError::None);
    });
}

TEST(CatMessageTest, MoveAssignmentAbandonsPreviousMessage) {
    run_on_loop([] {
        fiber::mem::BufPool pool;
        auto first_result = create_transaction_root(pool, "URL", "/first");
        auto second_result = create_transaction_root(pool, "URL", "/second");
        ASSERT_TRUE(first_result);
        ASSERT_TRUE(second_result);
        Transaction first = std::move(*first_result);
        Transaction second = std::move(*second_result);

        first = std::move(second);
        EXPECT_TRUE(first.valid());
        EXPECT_FALSE(second.valid());
        EXPECT_EQ(first.complete(), RecordError::None);
    });
}

TEST(CatMessageTest, EnforcesTreeChildAndDataLimits) {
    run_on_loop([] {
        RecordLimits limits;
        limits.max_messages = 2;
        limits.max_children_per_transaction = 1;
        limits.max_data_bytes_per_message = 4;
        fiber::mem::BufPool pool;
        auto root_result = create_transaction_root(pool, "T", "root", limits);
        ASSERT_TRUE(root_result);
        Transaction root = std::move(*root_result);

        EXPECT_EQ(root.add_data("12345"), RecordError::LimitExceeded);
        EXPECT_EQ(root.add_data("1234"), RecordError::None);
        auto event_result = root.start_event("E", "one");
        ASSERT_TRUE(event_result);
        auto second = root.start_event("E", "two");
        ASSERT_FALSE(second);
        EXPECT_EQ(second.error(), RecordError::LimitExceeded);

        EXPECT_EQ(root.complete(), RecordError::None);
        EXPECT_EQ(event_result->complete(), RecordError::None);
    });
}

TEST(CatMessageTest, SupportsStandaloneEventAndConsumesItOnCompletion) {
    run_on_loop([] {
        fiber::mem::BufPool pool;
        auto event_result = create_event_root(pool, "Exception", "read failed");
        ASSERT_TRUE(event_result);
        Event event = std::move(*event_result);
        EXPECT_EQ(event.add_data("code", "EIO"), RecordError::None);
        EXPECT_EQ(event.set_timestamp(1234), RecordError::None);
        EXPECT_EQ(event.complete(fiber::cat::status::Error), RecordError::None);
        EXPECT_FALSE(event.valid());
        EXPECT_EQ(event.set_status(fiber::cat::status::Success), RecordError::Completed);
    });
}

TEST(CatMessageTest, FixedChildrenChunksPreserveInsertionOrderAcrossBoundaries) {
    run_on_loop([] {
        fiber::mem::BufPool pool;
        auto root_result = fiber::cat::detail::create_transaction_root(pool, "T", "root", {});
        ASSERT_TRUE(root_result);
        auto *root = *root_result;
        std::array<fiber::cat::detail::EventData *, 33> children{};
        for (std::size_t i = 0; i < children.size(); ++i) {
            auto child = fiber::cat::detail::create_event(*root, "E", "child");
            ASSERT_TRUE(child);
            children[i] = *child;
        }

        ASSERT_EQ(root->child_count, 33U);
        auto *first = root->children_head;
        ASSERT_NE(first, nullptr);
        auto *second = first->next;
        ASSERT_NE(second, nullptr);
        auto *third = second->next;
        ASSERT_NE(third, nullptr);
        EXPECT_EQ(third->next, nullptr);
        EXPECT_EQ(first->children[0], children[0]);
        EXPECT_EQ(first->children[15], children[15]);
        EXPECT_EQ(second->children[0], children[16]);
        EXPECT_EQ(second->children[15], children[31]);
        EXPECT_EQ(third->children[0], children[32]);

        EXPECT_EQ(fiber::cat::detail::complete(root), RecordError::None);
        for (auto *&child: children) {
            EXPECT_EQ(fiber::cat::detail::complete(child), RecordError::None);
        }
    });
}

TEST(CatMessageTest, FlatDataUsesMultipleChunksWithoutExposingPartialEntries) {
    run_on_loop([] {
        fiber::mem::BufPool pool;
        auto root_result = fiber::cat::detail::create_transaction_root(pool, "T", "root", {});
        ASSERT_TRUE(root_result);
        auto *root = *root_result;
        const std::string first(120, 'a');
        const std::string second(20, 'b');

        EXPECT_EQ(fiber::cat::detail::add_data(root, first), RecordError::None);
        EXPECT_EQ(fiber::cat::detail::add_data(root, second), RecordError::None);
        EXPECT_EQ(fiber::cat::detail::add_data(root, "key", "value"), RecordError::None);
        EXPECT_NE(root->data_head, root->data_tail);
        EXPECT_EQ(flatten_data(*root), first + "&" + second + "&key=value");
        EXPECT_EQ(root->data_size, first.size() + second.size() + 11U);

        EXPECT_EQ(fiber::cat::detail::complete(root), RecordError::None);
    });
}

TEST(CatMessageTest, EmptyDataStillSeparatesTheNextEntry) {
    run_on_loop([] {
        fiber::mem::BufPool pool;
        auto root_result = fiber::cat::detail::create_transaction_root(pool, "T", "root", {});
        ASSERT_TRUE(root_result);
        auto *root = *root_result;
        EXPECT_EQ(fiber::cat::detail::add_data(root, ""), RecordError::None);
        EXPECT_EQ(fiber::cat::detail::add_data(root, "x"), RecordError::None);
        EXPECT_EQ(flatten_data(*root), "&x");
        EXPECT_EQ(fiber::cat::detail::complete(root), RecordError::None);
    });
}

TEST(CatMessageTest, ExplicitSpaceSeparatorMustBeSetBeforeData) {
    run_on_loop([] {
        fiber::mem::BufPool pool;
        auto root_result = fiber::cat::detail::create_transaction_root(pool, "T", "root", {});
        ASSERT_TRUE(root_result);
        auto *root = *root_result;

        EXPECT_EQ(fiber::cat::detail::set_data_separator(root, '\n'), RecordError::InvalidArgument);
        EXPECT_EQ(fiber::cat::detail::set_data_separator(root, ' '), RecordError::None);
        EXPECT_EQ(fiber::cat::detail::add_data(root, "method", "GET"), RecordError::None);
        EXPECT_EQ(fiber::cat::detail::add_data(root, "host", "example.test"), RecordError::None);
        EXPECT_EQ(flatten_data(*root), "method=GET host=example.test");
        EXPECT_EQ(fiber::cat::detail::set_data_separator(root, '&'), RecordError::InvalidArgument);

        EXPECT_EQ(fiber::cat::detail::complete(root), RecordError::None);
    });
}

struct ObservedContexts {
    std::array<std::string_view, 4> keys{};
    std::array<std::string_view, 4> values{};
    std::size_t size = 0;
};

bool observe_context(void *opaque, std::string_view key, std::string_view value) noexcept {
    auto &observed = *static_cast<ObservedContexts *>(opaque);
    if (observed.size >= observed.keys.size()) {
        return false;
    }
    observed.keys[observed.size] = key;
    observed.values[observed.size] = value;
    ++observed.size;
    return true;
}

TEST(CatMessageTest, TraceContextSupportsUpsertLookupRemovalAndOrderedIteration) {
    run_on_loop([] {
        fiber::mem::BufPool pool;
        auto created = fiber::cat::detail::create_message_trace(pool, {});
        ASSERT_TRUE(created);
        auto *trace = *created;

        EXPECT_EQ(fiber::cat::detail::put_context(*trace, "tenant", "blue"), RecordError::None);
        EXPECT_EQ(fiber::cat::detail::put_context(*trace, "request", "one"), RecordError::None);
        EXPECT_EQ(fiber::cat::detail::put_context(*trace, "empty", ""), RecordError::None);
        EXPECT_EQ(fiber::cat::detail::put_context(*trace, "tenant", "green"), RecordError::None);

        auto tenant = fiber::cat::detail::get_context(*trace, "tenant");
        ASSERT_TRUE(tenant);
        ASSERT_TRUE(tenant->has_value());
        EXPECT_EQ(**tenant, "green");
        auto empty = fiber::cat::detail::get_context(*trace, "empty");
        ASSERT_TRUE(empty);
        ASSERT_TRUE(empty->has_value());
        EXPECT_TRUE((**empty).empty());
        auto missing = fiber::cat::detail::get_context(*trace, "missing");
        ASSERT_TRUE(missing);
        EXPECT_FALSE(missing->has_value());

        ObservedContexts observed;
        EXPECT_EQ(fiber::cat::detail::for_each_context(*trace, &observed, observe_context), RecordError::None);
        ASSERT_EQ(observed.size, 3U);
        EXPECT_EQ(observed.keys[0], "tenant");
        EXPECT_EQ(observed.values[0], "green");
        EXPECT_EQ(observed.keys[1], "request");
        EXPECT_EQ(observed.keys[2], "empty");

        auto removed = fiber::cat::detail::remove_context(*trace, "tenant");
        ASSERT_TRUE(removed);
        EXPECT_TRUE(*removed);
        EXPECT_EQ(fiber::cat::detail::put_context(*trace, "tenant", "red"), RecordError::None);
        observed = {};
        EXPECT_EQ(fiber::cat::detail::for_each_context(*trace, &observed, observe_context), RecordError::None);
        ASSERT_EQ(observed.size, 3U);
        EXPECT_EQ(observed.keys[0], "request");
        EXPECT_EQ(observed.keys[1], "empty");
        EXPECT_EQ(observed.keys[2], "tenant");

        removed = fiber::cat::detail::remove_context(*trace, "missing");
        ASSERT_TRUE(removed);
        EXPECT_FALSE(*removed);
        fiber::cat::detail::discard_message_trace(trace);
        EXPECT_EQ(trace->data, nullptr);
    });
}

TEST(CatMessageTest, TraceContextEnforcesLimitsAndChargesRemovedArenaStorage) {
    run_on_loop([] {
        RecordLimits limits;
        limits.max_context_entries = 1;
        limits.max_context_key_bytes = 4;
        limits.max_context_value_bytes = 4;
        limits.max_context_bytes =
                8 * sizeof(fiber::cat::detail::ContextEntry *) + sizeof(fiber::cat::detail::ContextEntry) + 2;
        fiber::mem::BufPool pool;
        auto created = fiber::cat::detail::create_message_trace(pool, limits);
        ASSERT_TRUE(created);
        auto *trace = *created;

        EXPECT_EQ(fiber::cat::detail::put_context(*trace, "a", "b"), RecordError::None);
        EXPECT_EQ(fiber::cat::detail::put_context(*trace, "next", "v"), RecordError::LimitExceeded);
        EXPECT_EQ(fiber::cat::detail::put_context(*trace, "a", "12345"), RecordError::LimitExceeded);
        EXPECT_EQ(fiber::cat::detail::put_context(*trace, "a", "bb"), RecordError::LimitExceeded);
        auto old_value = fiber::cat::detail::get_context(*trace, "a");
        ASSERT_TRUE(old_value);
        ASSERT_TRUE(old_value->has_value());
        EXPECT_EQ(**old_value, "b");

        auto removed = fiber::cat::detail::remove_context(*trace, "a");
        ASSERT_TRUE(removed);
        EXPECT_TRUE(*removed);
        EXPECT_EQ(fiber::cat::detail::put_context(*trace, "c", "d"), RecordError::LimitExceeded);
        EXPECT_EQ(fiber::cat::detail::put_context(*trace, "", "v"), RecordError::InvalidArgument);

        fiber::cat::detail::discard_message_trace(trace);
    });
}

struct MutatingContextVisitor {
    fiber::cat::detail::MessageTrace *trace = nullptr;
    RecordError mutation = RecordError::Completed;
};

bool mutate_context(void *opaque, std::string_view, std::string_view) noexcept {
    auto &state = *static_cast<MutatingContextVisitor *>(opaque);
    state.mutation = fiber::cat::detail::put_context(*state.trace, "second", "value");
    return true;
}

TEST(CatMessageTest, TraceContextDetectsMutationDuringIterationAndExpiresOnCommit) {
    run_on_loop([] {
        fiber::mem::BufPool pool;
        auto created = fiber::cat::detail::create_message_trace(pool, {});
        ASSERT_TRUE(created);
        auto *trace = *created;
        EXPECT_EQ(fiber::cat::detail::put_context(*trace, "first", "value"), RecordError::None);

        MutatingContextVisitor visitor{.trace = trace};
        EXPECT_EQ(fiber::cat::detail::for_each_context(*trace, &visitor, mutate_context), RecordError::InvalidArgument);
        EXPECT_EQ(visitor.mutation, RecordError::None);

        auto root = fiber::cat::detail::create_event_root(*trace, "E", "root");
        ASSERT_TRUE(root);
        auto *event = *root;
        EXPECT_EQ(fiber::cat::detail::complete(event), RecordError::None);
        EXPECT_EQ(trace->data, nullptr);
        auto expired = fiber::cat::detail::get_context(*trace, "first");
        ASSERT_FALSE(expired);
        EXPECT_EQ(expired.error(), RecordError::Completed);
        ObservedContexts observed;
        EXPECT_EQ(fiber::cat::detail::for_each_context(*trace, &observed, observe_context), RecordError::Completed);

        fiber::cat::detail::discard_message_trace(trace);
    });
}

TEST(CatMessageTest, ConvenienceApisLogErrorsAndCompletedDurationsWithoutImplicitStack) {
    run_on_loop([] {
        fiber::mem::BufPool pool;
        auto root_result = create_transaction_root(pool, "URL", "/orders");
        ASSERT_TRUE(root_result);
        Transaction root = std::move(*root_result);
        EXPECT_EQ(root.log_completed_transaction("Cache", "lookup", 2500us, fiber::cat::status::Success, "hit=true"),
                  RecordError::None);
        EXPECT_EQ(root.log_error("backend failed", "connection reset"), RecordError::None);
        EXPECT_TRUE(root.valid());
        EXPECT_EQ(root.complete(), RecordError::None);
        EXPECT_FALSE(root.valid());
    });
}

struct InterleaveResult {
    bool recorded = false;
    bool consumed = false;
};

fiber::async::DetachedTask record_interleaved(std::string_view root_name, std::string_view event_name,
                                              std::chrono::milliseconds delay, InterleaveResult *result,
                                              std::atomic<unsigned int> *done) {
    fiber::mem::BufPool pool;
    auto root_result = create_transaction_root(pool, "URL", root_name);
    if (!root_result) {
        done->fetch_add(1, std::memory_order_relaxed);
        co_return;
    }
    Transaction root = std::move(*root_result);
    co_await fiber::async::sleep(delay);
    result->recorded = root.log_event("Marker", event_name) == RecordError::None;
    result->consumed = root.complete() == RecordError::None && !root.valid();
    if (done->fetch_add(1, std::memory_order_acq_rel) + 1 == 2) {
        fiber::event::EventLoop::current().stop();
    }
}

TEST(CatMessageTest, CoroutineInterleavingKeepsTracesIndependent) {
    fiber::event::EventLoop loop;
    std::atomic<unsigned int> done{0};
    InterleaveResult first;
    InterleaveResult second;
    fiber::async::spawn(loop, [&] { return record_interleaved("first", "first-event", 2ms, &first, &done); });
    fiber::async::spawn(loop, [&] { return record_interleaved("second", "second-event", 1ms, &second, &done); });
    loop.run();

    EXPECT_TRUE(first.recorded);
    EXPECT_TRUE(first.consumed);
    EXPECT_TRUE(second.recorded);
    EXPECT_TRUE(second.consumed);
}

} // namespace
