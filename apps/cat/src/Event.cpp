#include <fiber/cat/Event.h>

#include <utility>

#include <fiber/cat/CatClient.h>
#include <fiber/cat/MessageTrace.h>

#include "CatInternal.h"

namespace fiber::cat {

Event detail::MessageHandleAccess::event(detail::EventData *data) noexcept { return Event(data); }

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

std::expected<Event, RecordError> Event::create_root(CatClient &client, mem::BufPool &pool, std::string_view type,
                                                     std::string_view name,
                                                     MessageTraceCreateOptions options) noexcept {
    auto trace = MessageTrace::create(client, pool, options);
    if (!trace) {
        return std::unexpected(trace.error());
    }
    auto created = detail::create_event_root(*trace->trace_, type, name);
    if (!created) {
        detail::discard_message_trace(trace->trace_);
        return std::unexpected(created.error());
    }
    return Event(*created);
}

MessageTrace Event::message_trace() const noexcept { return MessageTrace(data_ ? data_->trace : nullptr); }

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
