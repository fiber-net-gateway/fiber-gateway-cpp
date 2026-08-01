#ifndef FIBER_CAT_EVENT_H
#define FIBER_CAT_EVENT_H

#include <cstdint>
#include <expected>
#include <string_view>

#include "Message.h"
#include "MessageTrace.h"

namespace fiber::cat {

class CatClient;
class Transaction;

namespace detail {
struct MessageHandleAccess;
}

class Event {
public:
    Event() noexcept = default;
    Event(const Event &) = delete;
    Event &operator=(const Event &) = delete;
    Event(Event &&other) noexcept;
    Event &operator=(Event &&other) noexcept;
    ~Event();

    [[nodiscard]] bool valid() const noexcept { return data_ != nullptr; }
    [[nodiscard]] MessageTrace message_trace() const noexcept;

    RecordError add_data(std::string_view data) noexcept;
    RecordError add_data(std::string_view key, std::string_view value) noexcept;
    RecordError set_type(std::string_view type) noexcept;
    RecordError set_name(std::string_view name) noexcept;
    RecordError set_status(std::string_view status) noexcept;
    RecordError set_timestamp(std::uint64_t timestamp_millis) noexcept;
    RecordError complete() noexcept;
    RecordError complete(std::string_view status) noexcept;

private:
    friend struct detail::MessageHandleAccess;
    friend class CatClient;
    friend class Transaction;

    [[nodiscard]] static std::expected<Event, RecordError> create_root(CatClient &client, mem::BufPool &pool,
                                                                       std::string_view type, std::string_view name,
                                                                       MessageTraceCreateOptions options) noexcept;

    explicit Event(detail::EventData *data) noexcept : data_(data) {}

    void reset() noexcept;

    detail::EventData *data_ = nullptr;
};

} // namespace fiber::cat

#endif // FIBER_CAT_EVENT_H
