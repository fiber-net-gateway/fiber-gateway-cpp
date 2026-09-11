#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <fiber/common/IntrusiveList.h>

namespace {

struct TestNode {
    int value = 0;
    fiber::common::IntrusiveListHook hook{};
};

using TestList = fiber::common::IntrusiveList<TestNode, offsetof(TestNode, hook)>;

static_assert(sizeof(fiber::common::IntrusiveListHook) == 2 * sizeof(void *));
static_assert(sizeof(TestList) == 2 * sizeof(void *));

std::vector<int> collected(TestList &list) {
    std::vector<int> values;
    for (TestNode *node = list.front(); node != nullptr; node = list.next_of(*node)) {
        values.push_back(node->value);
    }
    return values;
}

TestNode *owner_of(fiber::common::IntrusiveListHook &hook) noexcept {
    return reinterpret_cast<TestNode *>(reinterpret_cast<std::uint8_t *>(&hook) - offsetof(TestNode, hook));
}

std::vector<int> collected_over_anchor(fiber::common::IntrusiveListHook &anchor) {
    std::vector<int> values;
    for (auto *hook = anchor.next; hook != &anchor; hook = hook->next) {
        values.push_back(owner_of(*hook)->value);
    }
    return values;
}

// Every test declares its TestList before its nodes: the nodes then destruct
// first and unlink themselves, so ~TestList finds an empty ring. That is the
// ownership discipline the assert in ~IntrusiveList enforces.

TEST(IntrusiveListTest, FreshHookIsSelfLinkedAndUnlinked) {
    TestNode node{1};
    EXPECT_FALSE(node.hook.linked());
    // Unlinked representation is the self-ring: prev/next point at the hook itself.
    EXPECT_EQ(node.hook.prev, &node.hook);
    EXPECT_EQ(node.hook.next, &node.hook);
}

TEST(IntrusiveListTest, EmptyListHasNullEndpoints) {
    TestList list;
    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.front(), nullptr);
    EXPECT_EQ(list.back(), nullptr);
}

TEST(IntrusiveListTest, PushBackIteratesInOrder) {
    TestList list;
    TestNode a{1}, b{2}, c{3};
    list.push_back(a);
    list.push_back(b);
    list.push_back(c);

    EXPECT_FALSE(list.empty());
    EXPECT_EQ(collected(list), (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(list.front(), &a);
    EXPECT_EQ(list.back(), &c);
    EXPECT_EQ(list.next_of(a), &b);
    EXPECT_EQ(list.prev_of(c), &b);
    // Iteration terminates with nullptr at both ends; the anchor never leaks out.
    EXPECT_EQ(list.next_of(c), nullptr);
    EXPECT_EQ(list.prev_of(a), nullptr);
}

TEST(IntrusiveListTest, PushFrontOrdersNewestFirst) {
    TestList list;
    TestNode a{1}, b{2}, c{3};
    list.push_back(a);
    list.push_front(b);
    list.push_front(c);
    EXPECT_EQ(collected(list), (std::vector<int>{3, 2, 1}));
    EXPECT_EQ(list.front(), &c);
    EXPECT_EQ(list.back(), &a);
}

TEST(IntrusiveListTest, PushIsIdempotentForLinkedNode) {
    TestList list;
    TestNode a{1}, b{2};
    list.push_back(a);
    list.push_back(b);
    list.push_back(a);
    list.push_front(a);
    EXPECT_EQ(collected(list), (std::vector<int>{1, 2}));
}

TEST(IntrusiveListTest, EraseIsIdempotent) {
    TestList list;
    TestNode a{1}, b{2}, c{3};
    list.push_back(a);
    list.push_back(b);
    list.push_back(c);

    list.erase(b);
    list.erase(b);
    EXPECT_EQ(collected(list), (std::vector<int>{1, 3}));
    EXPECT_TRUE(a.hook.linked());
    EXPECT_TRUE(c.hook.linked());
    EXPECT_FALSE(b.hook.linked());
}

TEST(IntrusiveListTest, NextOfUnlinkedNodeIsNullptr) {
    TestList list;
    TestNode a{1}, b{2};
    list.push_back(a);
    list.push_back(b);
    list.erase(b);
    // An unlinked node terminates iteration, matching the null-terminated
    // contract of the old head/tail list.
    EXPECT_EQ(list.next_of(b), nullptr);
    EXPECT_EQ(list.prev_of(b), nullptr);
}

TEST(IntrusiveListTest, InsertAfterPositionsOwner) {
    TestList list;
    TestNode a{1}, b{2}, c{3}, d{4};
    list.push_back(a);
    list.push_back(b);
    list.push_back(c);

    list.insert_after(a, d);
    EXPECT_EQ(collected(list), (std::vector<int>{1, 4, 2, 3}));

    // Inserting an already-linked owner is a no-op.
    list.insert_after(c, d);
    EXPECT_EQ(collected(list), (std::vector<int>{1, 4, 2, 3}));

    // Inserting after the tail lands at the end.
    list.erase(d);
    list.insert_after(c, d);
    EXPECT_EQ(collected(list), (std::vector<int>{1, 2, 3, 4}));
}

TEST(IntrusiveListTest, InsertAfterUnlinkedPositionIsNoOp) {
    TestList list;
    TestNode a{1}, b{2}, d{4};
    list.push_back(a);
    list.push_back(b);
    list.erase(a);
    list.insert_after(a, d);
    EXPECT_EQ(collected(list), (std::vector<int>{2}));
    EXPECT_FALSE(d.hook.linked());
}

TEST(IntrusiveListTest, EraseDuringIterationWithPreReadNext) {
    TestList list;
    TestNode a{1}, b{2}, c{3};
    list.push_back(a);
    list.push_back(b);
    list.push_back(c);

    std::vector<int> seen;
    for (TestNode *node = list.front(); node != nullptr;) {
        TestNode *next = list.next_of(*node);
        seen.push_back(node->value);
        list.erase(*node);
        node = next;
    }
    EXPECT_EQ(seen, (std::vector<int>{1, 2, 3}));
    EXPECT_TRUE(list.empty());
}

TEST(IntrusiveListTest, UnlinkSelfDetachesWithoutList) {
    TestList list;
    TestNode a{1}, b{2};
    list.push_back(a);
    list.push_back(b);

    a.hook.unlink_self();
    EXPECT_FALSE(a.hook.linked());
    EXPECT_EQ(collected(list), (std::vector<int>{2}));
}

TEST(IntrusiveListTest, NodeDestructorUnlinksWhileLinked) {
    TestList list;
    TestNode a{1};
    list.push_back(a);
    TestNode *heap = new TestNode{9};
    list.push_back(*heap);
    EXPECT_EQ(collected(list), (std::vector<int>{1, 9}));

    // The dying node splices itself out; the list (and its destructor-time
    // empty assert below) stays consistent.
    delete heap;
    EXPECT_EQ(collected(list), (std::vector<int>{1}));
}

TEST(IntrusiveListTest, HookPrimitivesDriveBareAnchorRing) {
    // Pattern used by queues whose owner type cannot be named where the ring
    // lives (e.g. QuicConnection's connection-window waiters): a bare anchor
    // hook driven with insert_before/insert_after/unlink_self.
    fiber::common::IntrusiveListHook anchor{};
    TestNode a{1}, b{2}, c{3};

    // insert_before(anchor) is push_back.
    a.hook.insert_before(anchor);
    b.hook.insert_before(anchor);
    EXPECT_EQ(anchor.next, &a.hook);
    EXPECT_EQ(anchor.prev, &b.hook);

    // insert_after(anchor) is push_front.
    c.hook.insert_after(anchor);
    EXPECT_EQ(anchor.next, &c.hook);
    EXPECT_EQ(collected_over_anchor(anchor), (std::vector<int>{3, 1, 2}));

    // unlink_self drains the queue from its head.
    c.hook.unlink_self();
    a.hook.unlink_self();
    b.hook.unlink_self();
    EXPECT_EQ(anchor.next, &anchor);
    EXPECT_EQ(anchor.prev, &anchor);
}

TEST(IntrusiveListDeathTest, ListDestructorAssertsWhenNotEmpty) {
    TestNode node{1};
    EXPECT_DEATH(
            {
                TestList list;
                list.push_back(node);
            },
            "FIBER_ASSERT failed: empty");
}

} // namespace
