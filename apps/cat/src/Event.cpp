#include <fiber/cat/Event.h>

#include <utility>

#include "CatInternal.h"

namespace fiber::cat {

Event::Event(Event &&other) noexcept : data_(std::exchange(other.data_, nullptr)) {}

Event &Event::operator=(Event &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    data_ = std::exchange(other.data_, nullptr);
    return *this;
}

Event::~Event() { reset(); }

std::expected<Event, RecordError> Event::create_root(std::string_view type, std::string_view name,
                                                     RecordLimits limits) noexcept {
    auto created = detail::create_event_root(type, name, limits);
    if (!created) {
        return std::unexpected(created.error());
    }
    return Event(*created);
}

RecordError Event::add_data(std::string_view data) noexcept { return detail::add_data(data_, data); }

RecordError Event::add_data(std::string_view key, std::string_view value) noexcept {
    return detail::add_data(data_, key, value);
}

RecordError Event::set_type(std::string_view type) noexcept { return detail::set_type(data_, type); }

RecordError Event::set_name(std::string_view name) noexcept { return detail::set_name(data_, name); }

RecordError Event::set_status(std::string_view status_value) noexcept {
    return detail::set_status(data_, status_value);
}

RecordError Event::set_timestamp(std::uint64_t timestamp_millis) noexcept {
    return detail::set_timestamp(data_, timestamp_millis);
}

RecordError Event::complete() noexcept { return detail::complete(data_); }

RecordError Event::complete(std::string_view status_value) noexcept {
    const RecordError result = set_status(status_value);
    if (result != RecordError::None) {
        return result;
    }
    return complete();
}

void Event::reset() noexcept { detail::abandon(data_); }

} // namespace fiber::cat
