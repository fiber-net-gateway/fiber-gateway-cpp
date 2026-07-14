#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/BinaryHeap.h"

namespace {

struct TestNode {
    int key = 0;
    fiber::common::BinaryHeapNode hook{};
};

struct TestNodeLess {
    bool operator()(const TestNode *left, const TestNode *right) const noexcept { return left->key < right->key; }
};

using TestHeap = fiber::common::BinaryHeap<TestNode, offsetof(TestNode, hook), TestNodeLess>;

TestNode *owner_from_hook(fiber::common::BinaryHeapNode *hook) noexcept {
    if (!hook) {
        return nullptr;
    }
    return reinterpret_cast<TestNode *>(reinterpret_cast<std::uint8_t *>(hook) - offsetof(TestNode, hook));
}

const TestNode *owner_from_hook(const fiber::common::BinaryHeapNode *hook) noexcept {
    if (!hook) {
        return nullptr;
    }
    return reinterpret_cast<const TestNode *>(reinterpret_cast<const std::uint8_t *>(hook) - offsetof(TestNode, hook));
}

// Min-heap invariant: every node's key <= its children's keys, and parent
// links are consistent. Returns the number of nodes in the subtree.
std::size_t validate_subtree(const TestNode *node, const fiber::common::BinaryHeapNode *expected_parent) {
    if (!node) {
        return 0;
    }
    const auto &hook = node->hook;
    EXPECT_EQ(hook.parent, expected_parent);

    const TestNode *left = owner_from_hook(hook.left);
    const TestNode *right = owner_from_hook(hook.right);

    if (left) {
        EXPECT_LE(node->key, left->key);
    }
    if (right) {
        EXPECT_LE(node->key, right->key);
    }
    // A node may have at most one empty child, and only the right may be empty
    // while the left is present (complete-tree shape).
    if (!hook.left) {
        EXPECT_EQ(hook.right, nullptr);
    }

    std::size_t count = 1;
    count += validate_subtree(left, &hook);
    count += validate_subtree(right, &hook);
    return count;
}

void expect_heap_valid(const TestHeap &heap) {
    const TestNode *root = heap.min();
    std::size_t counted = validate_subtree(root, nullptr);
    EXPECT_EQ(counted, heap.size());
}

std::vector<int> drain_sorted(TestHeap &heap) {
    std::vector<int> keys;
    while (!heap.empty()) {
        TestNode *node = heap.min();
        EXPECT_NE(node, nullptr);
        if (!node) {
            break;
        }
        keys.push_back(node->key);
        heap.remove(*node);
        expect_heap_valid(heap);
    }
    EXPECT_EQ(heap.min(), nullptr);
    return keys;
}

} // namespace

TEST(BinaryHeapTest, EmptyHeapInitially) {
    TestHeap heap;
    EXPECT_TRUE(heap.empty());
    EXPECT_EQ(heap.size(), 0u);
    EXPECT_EQ(heap.min(), nullptr);
}

TEST(BinaryHeapTest, InsertAscendingExtractsAscending) {
    TestHeap heap;
    std::vector<TestNode> nodes(16);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        nodes[i].key = static_cast<int>(i);
        heap.insert(nodes[i]);
        expect_heap_valid(heap);
        EXPECT_EQ(heap.size(), i + 1);
    }
    EXPECT_EQ(heap.min()->key, 0);

    std::vector<int> keys = drain_sorted(heap);
    std::vector<int> expected;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        expected.push_back(static_cast<int>(i));
    }
    EXPECT_EQ(keys, expected);
}

TEST(BinaryHeapTest, InsertDescendingExtractsAscending) {
    TestHeap heap;
    std::vector<TestNode> nodes(16);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        nodes[i].key = static_cast<int>(nodes.size() - i);
        heap.insert(nodes[i]);
        expect_heap_valid(heap);
    }

    std::vector<int> keys = drain_sorted(heap);
    EXPECT_TRUE(std::is_sorted(keys.begin(), keys.end()));
    EXPECT_EQ(keys.front(), 1);
    EXPECT_EQ(keys.back(), 16);
}

TEST(BinaryHeapTest, InsertPseudoRandomPreservesHeapProperty) {
    TestHeap heap;
    std::vector<TestNode> nodes(200);
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        nodes[i].key = static_cast<int>(state >> 33) % 1000;
        heap.insert(nodes[i]);
        expect_heap_valid(heap);
    }
    EXPECT_EQ(heap.size(), nodes.size());

    std::vector<int> keys = drain_sorted(heap);
    EXPECT_TRUE(std::is_sorted(keys.begin(), keys.end()));
    EXPECT_EQ(keys.size(), nodes.size());
}

TEST(BinaryHeapTest, RemoveArbitraryPreservesHeapProperty) {
    TestHeap heap;
    std::vector<TestNode> nodes(64);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        nodes[i].key = static_cast<int>(i * 7 % 64);
        heap.insert(nodes[i]);
    }
    expect_heap_valid(heap);

    // Remove a handful of arbitrary (non-min) nodes still in the heap.
    heap.remove(nodes[3]);
    expect_heap_valid(heap);
    heap.remove(nodes[40]);
    expect_heap_valid(heap);
    heap.remove(nodes[20]);
    expect_heap_valid(heap);
    EXPECT_EQ(heap.size(), nodes.size() - 3);

    std::vector<int> keys = drain_sorted(heap);
    EXPECT_TRUE(std::is_sorted(keys.begin(), keys.end()));
    EXPECT_EQ(keys.size(), nodes.size() - 3);
}

TEST(BinaryHeapTest, RemoveAllInReverseInsertionOrder) {
    TestHeap heap;
    std::vector<TestNode> nodes(32);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        nodes[i].key = static_cast<int>(i);
        heap.insert(nodes[i]);
    }
    // Removing in reverse insertion order must never break the invariant.
    for (std::size_t i = nodes.size(); i > 0; --i) {
        heap.remove(nodes[i - 1]);
        expect_heap_valid(heap);
        EXPECT_EQ(heap.size(), i - 1);
    }
    EXPECT_TRUE(heap.empty());
    EXPECT_EQ(heap.min(), nullptr);
}

TEST(BinaryHeapTest, DuplicateKeysAreStable) {
    TestHeap heap;
    std::vector<TestNode> nodes(12);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        nodes[i].key = static_cast<int>(i % 3);
        heap.insert(nodes[i]);
    }
    expect_heap_valid(heap);

    std::vector<int> keys = drain_sorted(heap);
    EXPECT_TRUE(std::is_sorted(keys.begin(), keys.end()));
    EXPECT_EQ(keys.size(), nodes.size());
    // Four of each key 0, 1, 2.
    EXPECT_EQ(std::count(keys.begin(), keys.end(), 0), 4);
    EXPECT_EQ(std::count(keys.begin(), keys.end(), 1), 4);
    EXPECT_EQ(std::count(keys.begin(), keys.end(), 2), 4);
}

TEST(BinaryHeapTest, SingleNode) {
    TestHeap heap;
    TestNode node;
    node.key = 42;
    heap.insert(node);
    EXPECT_FALSE(heap.empty());
    EXPECT_EQ(heap.size(), 1u);
    ASSERT_NE(heap.min(), nullptr);
    EXPECT_EQ(heap.min()->key, 42);
    heap.remove(node);
    EXPECT_TRUE(heap.empty());
    EXPECT_EQ(heap.min(), nullptr);
}
