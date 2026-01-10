#ifndef FIBER_EVENT_EVENT_LOOP_GROUP_H
#define FIBER_EVENT_EVENT_LOOP_GROUP_H

#include <cstddef>
#include <memory>
#include <vector>

#include "../async/ThreadGroup.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "EventLoop.h"

namespace fiber::async {

class SignalSet;

} // namespace fiber::async

namespace fiber::event {

class EventLoopGroup : public common::NonCopyable,
                       public common::NonMovable {
public:
    explicit EventLoopGroup(std::size_t size);
    ~EventLoopGroup();

    void start();
    void start(const fiber::async::SignalSet &mask);
    void stop();
    void join();

    std::size_t size() const noexcept {
        return loops_.size();
    }

    EventLoop &at(std::size_t index);
    const EventLoop &at(std::size_t index) const;

    std::vector<std::unique_ptr<EventLoop>> loops_;
    fiber::async::ThreadGroup threads_;

private:
    void start_with_mask(const fiber::async::SignalSet *mask);
};

} // namespace fiber::event

#endif // FIBER_EVENT_EVENT_LOOP_GROUP_H
