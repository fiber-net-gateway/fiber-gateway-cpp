#ifndef FIBER_CAT_TRANSACTION_H
#define FIBER_CAT_TRANSACTION_H

#include <chrono>
#include <expected>
#include <string_view>

#include "Event.h"
#include "Message.h"
#include "Status.h"

namespace fiber::cat {

class Transaction {
public:
    Transaction() noexcept = default;
    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;
    Transaction(Transaction &&other) noexcept;
    Transaction &operator=(Transaction &&other) noexcept;
    ~Transaction();

    [[nodiscard]] static std::expected<Transaction, RecordError>
    create_root(std::string_view type, std::string_view name, RecordLimits limits = {}) noexcept;

    [[nodiscard]] bool valid() const noexcept { return data_ != nullptr; }

    [[nodiscard]] std::expected<Transaction, RecordError> start_transaction(std::string_view type,
                                                                            std::string_view name) noexcept;
    [[nodiscard]] std::expected<Event, RecordError> start_event(std::string_view type, std::string_view name) noexcept;

    RecordError log_event(std::string_view type, std::string_view name, std::string_view status = status::Success,
                          std::string_view data = {}) noexcept;

    RecordError add_data(std::string_view data) noexcept;
    RecordError add_data(std::string_view key, std::string_view value) noexcept;
    RecordError set_status(std::string_view status) noexcept;
    RecordError set_timestamp(std::uint64_t timestamp_millis) noexcept;
    RecordError set_duration(std::chrono::microseconds duration) noexcept;
    RecordError complete() noexcept;
    RecordError complete(std::string_view status) noexcept;

private:
    explicit Transaction(detail::TransactionData *data) noexcept : data_(data) {}

    void reset() noexcept;

    detail::TransactionData *data_ = nullptr;
};

} // namespace fiber::cat

#endif // FIBER_CAT_TRANSACTION_H
