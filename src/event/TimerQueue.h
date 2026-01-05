#ifndef FIBER_EVENT_TIMER_QUEUE_H
#define FIBER_EVENT_TIMER_QUEUE_H

#include <cstddef>

namespace fiber::event {

class TimerQueue {
public:
    struct Node {
        Node *left = nullptr;
        Node *right = nullptr;
        Node *parent = nullptr;
    };

    using Compare = bool (*)(const Node *a, const Node *b);

    TimerQueue();

    void init();
    Node *min() const;
    std::size_t size() const;
    bool empty() const;

    void insert(Node *node, Compare less_than);
    void remove(Node *node, Compare less_than);
    void dequeue(Compare less_than);

private:
    static void swap_nodes(TimerQueue *heap, Node *parent, Node *child);

    Node *min_ = nullptr;
    std::size_t count_ = 0;
};

} // namespace fiber::event

#endif // FIBER_EVENT_TIMER_QUEUE_H
