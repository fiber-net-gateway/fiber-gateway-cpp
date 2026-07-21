#ifndef FIBER_CAT_EVENT_H
#define FIBER_CAT_EVENT_H

#include <expected>
#include <string_view>

#include "Message.h"

namespace fiber::cat {

class Transaction;

class Event {
public:
    Event() noexcept = default;
    Event(const Event &) = delete;
    Event &operator=(const Event &) = delete;
    Event(Event &&other) noexcept;
    Event &operator=(Event &&other) noexcept;
    ~Event();

    [[nodiscard]] static std::expected<Event, RecordError> create_root(std::string_view type, std::string_view name,
                                                                       RecordLimits limits = {}) noexcept;

    [[nodiscard]] bool valid() const noexcept { return tree_ != nullptr; }
    [[nodiscard]] MessageView view() const noexcept { return MessageView(node_); }
    [[nodiscard]] bool tree_ready() const noexcept;
    [[nodiscard]] bool tree_has_problem() const noexcept;

    RecordError add_data(std::string_view data) noexcept;
    RecordError add_data(std::string_view key, std::string_view value) noexcept;
    RecordError set_status(std::string_view status) noexcept;
    RecordError set_timestamp(std::uint64_t timestamp_millis) noexcept;
    RecordError complete() noexcept;
    RecordError complete(std::string_view status) noexcept;

private:
    friend class Transaction;

    Event(detail::TreeData *tree, detail::MessageNode *node) noexcept : tree_(tree), node_(node) {}

    void reset() noexcept;

    detail::TreeData *tree_ = nullptr;
    detail::MessageNode *node_ = nullptr;
};

} // namespace fiber::cat

#endif // FIBER_CAT_EVENT_H
