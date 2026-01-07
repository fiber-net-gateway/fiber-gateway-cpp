#include "TimerQueue.h"

namespace fiber::event {

TimerQueue::TimerQueue() {
    init();
}

void TimerQueue::init() {
    min_ = nullptr;
    count_ = 0;
}

TimerQueue::Node *TimerQueue::min() const {
    return min_;
}

std::size_t TimerQueue::size() const {
    return count_;
}

bool TimerQueue::empty() const {
    return count_ == 0;
}

void TimerQueue::swap_nodes(TimerQueue *heap, Node *parent, Node *child) {
    Node *sibling;
    Node temp = *parent;
    *parent = *child;
    *child = temp;

    parent->parent = child;
    if (child->left == child) {
        child->left = parent;
        sibling = child->right;
    } else {
        child->right = parent;
        sibling = child->left;
    }
    if (sibling) {
        sibling->parent = child;
    }

    if (parent->left) {
        parent->left->parent = parent;
    }
    if (parent->right) {
        parent->right->parent = parent;
    }

    if (!child->parent) {
        heap->min_ = child;
    } else if (child->parent->left == parent) {
        child->parent->left = child;
    } else {
        child->parent->right = child;
    }
}

void TimerQueue::insert(Node *node) {
    Node **parent;
    Node **child;
    std::size_t path;
    std::size_t n;
    std::size_t k;

    node->left = nullptr;
    node->right = nullptr;
    node->parent = nullptr;

    path = 0;
    for (k = 0, n = 1 + count_; n >= 2; k += 1, n /= 2) {
        path = (path << 1) | (n & 1);
    }

    parent = child = &min_;
    while (k > 0) {
        parent = child;
        if (path & 1) {
            child = &(*child)->right;
        } else {
            child = &(*child)->left;
        }
        path >>= 1;
        k -= 1;
    }

    node->parent = *parent;
    *child = node;
    count_ += 1;

    while (node->parent && *node < *node->parent) {
        swap_nodes(this, node->parent, node);
    }
}

void TimerQueue::remove(Node *node) {
    Node *smallest;
    Node **max;
    Node *child;
    std::size_t path;
    std::size_t k;
    std::size_t n;

    if (count_ == 0) {
        return;
    }

    path = 0;
    for (k = 0, n = count_; n >= 2; k += 1, n /= 2) {
        path = (path << 1) | (n & 1);
    }

    max = &min_;
    while (k > 0) {
        if (path & 1) {
            max = &(*max)->right;
        } else {
            max = &(*max)->left;
        }
        path >>= 1;
        k -= 1;
    }

    count_ -= 1;

    child = *max;
    *max = nullptr;

    if (child == node) {
        if (child == min_) {
            min_ = nullptr;
        }
        return;
    }

    child->left = node->left;
    child->right = node->right;
    child->parent = node->parent;

    if (child->left) {
        child->left->parent = child;
    }

    if (child->right) {
        child->right->parent = child;
    }

    if (!node->parent) {
        min_ = child;
    } else if (node->parent->left == node) {
        node->parent->left = child;
    } else {
        node->parent->right = child;
    }

    for (;;) {
        smallest = child;
        if (child->left && *child->left < *smallest) {
            smallest = child->left;
        }
        if (child->right && *child->right < *smallest) {
            smallest = child->right;
        }
        if (smallest == child) {
            break;
        }
        swap_nodes(this, child, smallest);
    }

    while (child->parent && *child < *child->parent) {
        swap_nodes(this, child->parent, child);
    }
}

void TimerQueue::dequeue() {
    remove(min_);
}

} // namespace fiber::event
