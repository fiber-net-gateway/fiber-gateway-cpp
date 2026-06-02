#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/IntrusiveRbTree.h"

namespace {

struct TestNode {
    int key = 0;
    int id = 0;
    fiber::common::IntrusiveRbTreeHook hook{};
};

struct TestNodeLess {
    bool operator()(const TestNode *left, const TestNode *right) const noexcept { return left->key < right->key; }
};

using TestTree = fiber::common::IntrusiveRbTree<TestNode, offsetof(TestNode, hook), TestNodeLess>;

TestNode *owner_from_hook(fiber::common::IntrusiveRbTreeHook *hook) noexcept {
    if (!hook || !hook->in_tree) {
        return nullptr;
    }
    return reinterpret_cast<TestNode *>(reinterpret_cast<std::uint8_t *>(hook) - offsetof(TestNode, hook));
}

const TestNode *owner_from_hook(const fiber::common::IntrusiveRbTreeHook *hook) noexcept {
    if (!hook || !hook->in_tree) {
        return nullptr;
    }
    return reinterpret_cast<const TestNode *>(reinterpret_cast<const std::uint8_t *>(hook) - offsetof(TestNode, hook));
}

std::vector<int> keys_in_order(TestTree &tree) {
    std::vector<int> keys;
    for (TestNode *node = tree.minimum(); node; node = tree.next_of(*node)) {
        keys.push_back(node->key);
    }
    return keys;
}

int validate_node(const TestNode *node, const fiber::common::IntrusiveRbTreeHook *expected_parent) {
    if (!node) {
        return 1;
    }

    const auto &hook = node->hook;
    EXPECT_TRUE(hook.in_tree);
    EXPECT_EQ(hook.parent, expected_parent);

    const TestNode *left = owner_from_hook(hook.left);
    const TestNode *right = owner_from_hook(hook.right);

    if (left) {
        EXPECT_LE(left->key, node->key);
    }
    if (right) {
        EXPECT_LE(node->key, right->key);
    }

    if (hook.color == fiber::common::IntrusiveRbTreeColor::Red) {
        EXPECT_EQ(hook.left->color, fiber::common::IntrusiveRbTreeColor::Black);
        EXPECT_EQ(hook.right->color, fiber::common::IntrusiveRbTreeColor::Black);
    }

    const int left_black_height = validate_node(left, &hook);
    const int right_black_height = validate_node(right, &hook);
    EXPECT_EQ(left_black_height, right_black_height);

    return left_black_height + (hook.color == fiber::common::IntrusiveRbTreeColor::Black ? 1 : 0);
}

void validate_tree(TestTree &tree, const std::vector<TestNode *> &linked_nodes) {
    TestNode *root = tree.root();
    if (linked_nodes.empty()) {
        EXPECT_TRUE(tree.empty());
        EXPECT_EQ(root, nullptr);
        EXPECT_EQ(tree.minimum(), nullptr);
        return;
    }

    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->hook.parent, nullptr);
    EXPECT_EQ(root->hook.color, fiber::common::IntrusiveRbTreeColor::Black);
    validate_node(root, nullptr);

    std::vector<int> actual = keys_in_order(tree);
    std::vector<int> expected;
    expected.reserve(linked_nodes.size());
    for (const TestNode *node: linked_nodes) {
        expected.push_back(node->key);
        EXPECT_TRUE(node->hook.linked());
    }
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(actual, expected);
}

} // namespace

TEST(IntrusiveRbTreeTest, EmptyTree) {
    TestTree tree;

    EXPECT_TRUE(tree.empty());
    EXPECT_EQ(tree.root(), nullptr);
    EXPECT_EQ(tree.minimum(), nullptr);
}

TEST(IntrusiveRbTreeTest, SingleInsertAndErase) {
    TestTree tree;
    TestNode node{.key = 7, .id = 1};

    tree.insert(node);
    validate_tree(tree, {&node});
    EXPECT_FALSE(tree.empty());
    EXPECT_EQ(tree.root(), &node);
    EXPECT_EQ(tree.minimum(), &node);
    EXPECT_EQ(tree.next_of(node), nullptr);

    tree.erase(node);
    validate_tree(tree, {});
    EXPECT_FALSE(node.hook.linked());
    EXPECT_EQ(node.hook.left, nullptr);
    EXPECT_EQ(node.hook.right, nullptr);
    EXPECT_EQ(node.hook.parent, nullptr);
}

TEST(IntrusiveRbTreeTest, TraversesInKeyOrder) {
    TestTree tree;
    std::vector<TestNode> nodes{{.key = 8, .id = 0}, {.key = 3, .id = 1}, {.key = 10, .id = 2},
                                {.key = 1, .id = 3}, {.key = 6, .id = 4}, {.key = 14, .id = 5},
                                {.key = 4, .id = 6}, {.key = 7, .id = 7}, {.key = 13, .id = 8}};
    std::vector<TestNode *> linked;

    for (TestNode &node: nodes) {
        tree.insert(node);
        linked.push_back(&node);
        validate_tree(tree, linked);
    }

    EXPECT_EQ(keys_in_order(tree), (std::vector<int>{1, 3, 4, 6, 7, 8, 10, 13, 14}));
}

TEST(IntrusiveRbTreeTest, AllowsDuplicateKeys) {
    TestTree tree;
    std::vector<TestNode> nodes{{.key = 5, .id = 0}, {.key = 5, .id = 1}, {.key = 5, .id = 2},
                                {.key = 3, .id = 3}, {.key = 7, .id = 4}, {.key = 5, .id = 5}};
    std::vector<TestNode *> linked;

    for (TestNode &node: nodes) {
        tree.insert(node);
        linked.push_back(&node);
    }

    validate_tree(tree, linked);
    EXPECT_EQ(keys_in_order(tree), (std::vector<int>{3, 5, 5, 5, 5, 7}));
}

TEST(IntrusiveRbTreeTest, DeletesLeafOneChildTwoChildrenAndRoot) {
    TestTree tree;
    std::vector<TestNode> nodes{{.key = 20, .id = 0}, {.key = 10, .id = 1}, {.key = 30, .id = 2},
                                {.key = 5, .id = 3},  {.key = 15, .id = 4}, {.key = 25, .id = 5},
                                {.key = 40, .id = 6}, {.key = 12, .id = 7}, {.key = 17, .id = 8}};
    std::vector<TestNode *> linked;

    for (TestNode &node: nodes) {
        tree.insert(node);
        linked.push_back(&node);
    }
    validate_tree(tree, linked);

    tree.erase(nodes[3]);
    linked.erase(std::remove(linked.begin(), linked.end(), &nodes[3]), linked.end());
    validate_tree(tree, linked);
    EXPECT_FALSE(nodes[3].hook.linked());

    tree.erase(nodes[4]);
    linked.erase(std::remove(linked.begin(), linked.end(), &nodes[4]), linked.end());
    validate_tree(tree, linked);
    EXPECT_FALSE(nodes[4].hook.linked());

    tree.erase(nodes[1]);
    linked.erase(std::remove(linked.begin(), linked.end(), &nodes[1]), linked.end());
    validate_tree(tree, linked);
    EXPECT_FALSE(nodes[1].hook.linked());

    TestNode *old_root = tree.root();
    ASSERT_NE(old_root, nullptr);
    tree.erase(*old_root);
    linked.erase(std::remove(linked.begin(), linked.end(), old_root), linked.end());
    validate_tree(tree, linked);
    EXPECT_FALSE(old_root->hook.linked());
}

TEST(IntrusiveRbTreeTest, MixedInsertDeleteUntilEmpty) {
    TestTree tree;
    std::vector<TestNode> nodes{{.key = 16, .id = 0}, {.key = 8, .id = 1},  {.key = 24, .id = 2}, {.key = 4, .id = 3},
                                {.key = 12, .id = 4}, {.key = 20, .id = 5}, {.key = 28, .id = 6}, {.key = 2, .id = 7},
                                {.key = 6, .id = 8},  {.key = 10, .id = 9}, {.key = 14, .id = 10}};
    std::vector<TestNode *> linked;

    for (TestNode &node: nodes) {
        tree.insert(node);
        linked.push_back(&node);
    }
    validate_tree(tree, linked);

    const int erase_order[] = {5, 0, 10, 1, 8, 2, 7, 4, 6, 3, 9};
    for (int index: erase_order) {
        tree.erase(nodes[index]);
        linked.erase(std::remove(linked.begin(), linked.end(), &nodes[index]), linked.end());
        validate_tree(tree, linked);
        EXPECT_FALSE(nodes[index].hook.linked());
    }

    EXPECT_TRUE(tree.empty());
}

TEST(IntrusiveRbTreeTest, EraseUnlinkedNodeIsNoop) {
    TestTree tree;
    TestNode linked{.key = 1, .id = 1};
    TestNode unlinked{.key = 2, .id = 2};

    tree.insert(linked);
    tree.erase(unlinked);

    validate_tree(tree, {&linked});
    EXPECT_FALSE(unlinked.hook.linked());
}
