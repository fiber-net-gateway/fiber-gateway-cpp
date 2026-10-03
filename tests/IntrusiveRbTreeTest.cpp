#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <type_traits>
#include <vector>

#include <fiber/common/IntrusiveRbTree.h>

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

static_assert(!std::is_copy_constructible_v<TestNode>);
static_assert(!std::is_copy_assignable_v<TestNode>);
static_assert(!std::is_move_constructible_v<TestNode>);
static_assert(!std::is_move_assignable_v<TestNode>);

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
    const auto *sentinel = root->hook.parent;
    ASSERT_NE(sentinel, nullptr);
    EXPECT_FALSE(sentinel->linked());
    EXPECT_EQ(sentinel->parent, &root->hook);
    EXPECT_EQ(sentinel->left, sentinel);
    EXPECT_EQ(sentinel->right, sentinel);
    EXPECT_EQ(sentinel->color, fiber::common::IntrusiveRbTreeColor::Black);
    EXPECT_EQ(root->hook.color, fiber::common::IntrusiveRbTreeColor::Black);
    validate_node(root, sentinel);

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
    TestNode nodes[]{{.key = 8, .id = 0}, {.key = 3, .id = 1}, {.key = 10, .id = 2},
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
    TestNode nodes[]{{.key = 5, .id = 0}, {.key = 5, .id = 1}, {.key = 5, .id = 2},
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
    TestNode nodes[]{{.key = 20, .id = 0}, {.key = 10, .id = 1}, {.key = 30, .id = 2},
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
    TestNode nodes[]{{.key = 16, .id = 0}, {.key = 8, .id = 1},  {.key = 24, .id = 2}, {.key = 4, .id = 3},
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

TEST(IntrusiveRbTreeTest, ScopedNodeDestructionUnlinksAndTreeCanBeReused) {
    TestTree tree;
    TestNode survivor{.key = 10};
    tree.insert(survivor);
    {
        TestNode nodes[]{{.key = 4}, {.key = 12}, {.key = 2}, {.key = 6}, {.key = 11}, {.key = 14}};
        for (TestNode &node: nodes) {
            tree.insert(node);
        }
        EXPECT_EQ(keys_in_order(tree), (std::vector<int>{2, 4, 6, 10, 11, 12, 14}));
    }
    validate_tree(tree, {&survivor});

    survivor.hook.unlink_self();
    survivor.hook.unlink_self();
    validate_tree(tree, {});
    EXPECT_EQ(survivor.hook.left, nullptr);
    EXPECT_EQ(survivor.hook.right, nullptr);
    EXPECT_EQ(survivor.hook.parent, nullptr);

    {
        TestNode only{.key = 3};
        tree.insert(only);
        validate_tree(tree, {&only});
    }
    validate_tree(tree, {});
    tree.insert(survivor);
    validate_tree(tree, {&survivor});
}

TEST(IntrusiveRbTreeTest, UnlinkedNodeMayOutliveTreeAndJoinAnotherTree) {
    TestNode node{.key = 5};
    {
        TestTree tree;
        tree.insert(node);
        node.hook.unlink_self();
        validate_tree(tree, {});
    }
    {
        TestTree tree;
        tree.insert(node);
        validate_tree(tree, {&node});
        tree.erase(node);
    }
}

TEST(IntrusiveRbTreeTest, DestroyRootRepeatedlyPreservesTree) {
    TestTree tree;
    std::array<std::optional<TestNode>, 32> nodes{};
    std::vector<TestNode *> linked;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        TestNode &node = nodes[i].emplace();
        node.key = static_cast<int>(i);
        node.id = static_cast<int>(i);
        tree.insert(node);
        linked.push_back(&node);
    }
    while (!tree.empty()) {
        TestNode *root = tree.root();
        const int id = root->id;
        linked.erase(std::remove(linked.begin(), linked.end(), root), linked.end());
        nodes[id].reset();
        validate_tree(tree, linked);
    }
}

TEST(IntrusiveRbTreeTest, RandomInsertEraseUnlinkAndDestructionPreserveInvariants) {
    TestTree tree;
    std::array<std::optional<TestNode>, 64> nodes{};
    std::array<bool, 64> expected_linked{};
    std::mt19937 random(0x5eed);
    for (int step = 0; step < 4000; ++step) {
        SCOPED_TRACE(step);
        const std::size_t index = random() % nodes.size();
        auto &slot = nodes[index];
        if (!slot) {
            slot.emplace().key = static_cast<int>(random() % 16);
            tree.insert(*slot);
            expected_linked[index] = true;
        } else {
            switch (random() % 4) {
                case 0:
                    tree.erase(*slot);
                    expected_linked[index] = false;
                    break;
                case 1:
                    slot->hook.unlink_self();
                    expected_linked[index] = false;
                    break;
                case 2:
                    slot.reset();
                    expected_linked[index] = false;
                    break;
                case 3:
                    if (!expected_linked[index]) {
                        tree.insert(*slot);
                        expected_linked[index] = true;
                    }
                    break;
            }
        }

        std::vector<TestNode *> linked;
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            auto &node = nodes[i];
            EXPECT_EQ(node && node->hook.linked(), expected_linked[i]);
            if (expected_linked[i]) {
                ASSERT_TRUE(node.has_value());
                linked.push_back(&*node);
            }
        }
        validate_tree(tree, linked);
        const TestTree &const_tree = tree;
        EXPECT_EQ(const_tree.root(), tree.root());
        EXPECT_EQ(const_tree.minimum(), tree.minimum());
        for (TestNode *node: linked) {
            EXPECT_EQ(const_tree.next_of(*node), tree.next_of(*node));
        }
    }
}

TEST(IntrusiveRbTreeDeathTest, TreeMustOutliveLinkedNodes) {
    EXPECT_DEATH(
            {
                TestNode node{.key = 1};
                TestTree tree;
                tree.insert(node);
            },
            "FIBER_ASSERT failed: empty\\(\\)");
}
