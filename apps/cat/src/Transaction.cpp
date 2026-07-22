#include <fiber/cat/Transaction.h>

#include <utility>

#include "CatInternal.h"

namespace fiber::cat {

Transaction::Transaction(Transaction &&other) noexcept : data_(std::exchange(other.data_, nullptr)) {}

Transaction &Transaction::operator=(Transaction &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    data_ = std::exchange(other.data_, nullptr);
    return *this;
}

Transaction::~Transaction() { reset(); }

std::expected<Transaction, RecordError> Transaction::create_root(std::string_view type, std::string_view name,
                                                                 RecordLimits limits) noexcept {
    auto created = detail::create_transaction_root(type, name, limits);
    if (!created) {
        return std::unexpected(created.error());
    }
    return Transaction(*created);
}

std::expected<Transaction, RecordError> Transaction::start_transaction(std::string_view type,
                                                                       std::string_view name) noexcept {
    if (!data_) {
        return std::unexpected(RecordError::Completed);
    }
    auto child = detail::create_transaction(*data_, type, name);
    if (!child) {
        return std::unexpected(child.error());
    }
    return Transaction(*child);
}

std::expected<Event, RecordError> Transaction::start_event(std::string_view type, std::string_view name) noexcept {
    if (!data_) {
        return std::unexpected(RecordError::Completed);
    }
    auto child = detail::create_event(*data_, type, name);
    if (!child) {
        return std::unexpected(child.error());
    }
    return Event(*child);
}

RecordError Transaction::log_event(std::string_view type, std::string_view name, std::string_view status_value,
                                   std::string_view data) noexcept {
    auto created = start_event(type, name);
    if (!created) {
        return created.error();
    }
    Event event = std::move(*created);
    RecordError result = RecordError::None;
    if (status_value != status::Success) {
        result = event.set_status(status_value);
    }
    if (!data.empty()) {
        const RecordError data_result = event.add_data(data);
        if (result == RecordError::None) {
            result = data_result;
        }
    }
    const RecordError complete_result = event.complete();
    return result != RecordError::None ? result : complete_result;
}

RecordError Transaction::log_error(std::string_view message, std::string_view error) noexcept {
    return log_event("Exception", message, status::Error, error);
}

RecordError Transaction::log_completed_transaction(std::string_view type, std::string_view name,
                                                   std::chrono::microseconds duration, std::string_view status_value,
                                                   std::string_view data) noexcept {
    auto created = start_transaction(type, name);
    if (!created) {
        return created.error();
    }
    Transaction transaction = std::move(*created);
    return detail::complete_with_duration(transaction.data_, duration, status_value, data);
}

RecordError Transaction::add_data(std::string_view data) noexcept { return detail::add_data(data_, data); }

RecordError Transaction::add_data(std::string_view key, std::string_view value) noexcept {
    return detail::add_data(data_, key, value);
}

RecordError Transaction::set_status(std::string_view status_value) noexcept {
    return detail::set_status(data_, status_value);
}

RecordError Transaction::set_timestamp(std::uint64_t timestamp_millis) noexcept {
    return detail::set_timestamp(data_, timestamp_millis);
}

RecordError Transaction::set_duration(std::chrono::microseconds duration) noexcept {
    return detail::set_duration(data_, duration);
}

RecordError Transaction::complete() noexcept { return detail::complete(data_); }

RecordError Transaction::complete(std::string_view status_value) noexcept {
    const RecordError result = set_status(status_value);
    if (result != RecordError::None) {
        return result;
    }
    return complete();
}

void Transaction::reset() noexcept { detail::abandon(data_); }

} // namespace fiber::cat
