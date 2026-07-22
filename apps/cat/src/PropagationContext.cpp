#include <fiber/cat/PropagationContext.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace fiber::cat::detail {

struct PropagationContextData {
    std::atomic<std::uint32_t> references{1};
    std::size_t message_id_size = 0;
    std::size_t root_message_id_size = 0;
    std::size_t parent_message_id_size = 0;
    std::size_t session_token_size = 0;

    [[nodiscard]] const char *text() const noexcept { return reinterpret_cast<const char *>(this + 1); }
};

} // namespace fiber::cat::detail

namespace fiber::cat {

namespace {

inline constexpr std::size_t kMaxPropagationIdBytes = 1024;
inline constexpr std::size_t kMaxSessionTokenBytes = 4096;

bool checked_add(std::size_t lhs, std::size_t rhs, std::size_t &result) noexcept {
    if (lhs > std::numeric_limits<std::size_t>::max() - rhs) {
        return false;
    }
    result = lhs + rhs;
    return true;
}

bool valid_header_value(std::string_view value) noexcept {
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) { return byte >= 0x21 && byte <= 0x7e; });
}

std::string_view field(const detail::PropagationContextData *data, std::size_t offset, std::size_t size) noexcept {
    return data && size != 0 ? std::string_view(data->text() + offset, size) : std::string_view{};
}

} // namespace

PropagationContext::PropagationContext(const PropagationContext &other) noexcept : data_(other.data_) {
    if (data_) {
        data_->references.fetch_add(1, std::memory_order_relaxed);
    }
}

PropagationContext &PropagationContext::operator=(const PropagationContext &other) noexcept {
    if (this == &other) {
        return *this;
    }
    detail::PropagationContextData *replacement = other.data_;
    if (replacement) {
        replacement->references.fetch_add(1, std::memory_order_relaxed);
    }
    reset();
    data_ = replacement;
    return *this;
}

PropagationContext::PropagationContext(PropagationContext &&other) noexcept :
    data_(std::exchange(other.data_, nullptr)) {}

PropagationContext &PropagationContext::operator=(PropagationContext &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    data_ = std::exchange(other.data_, nullptr);
    return *this;
}

PropagationContext::~PropagationContext() { reset(); }

std::expected<PropagationContext, RecordError> PropagationContext::create(MessageTraceContext context) noexcept {
    if ((context.message_id.empty() && (!context.root_message_id.empty() || !context.parent_message_id.empty())) ||
        context.message_id.size() > kMaxPropagationIdBytes || context.root_message_id.size() > kMaxPropagationIdBytes ||
        context.parent_message_id.size() > kMaxPropagationIdBytes ||
        context.session_token.size() > kMaxSessionTokenBytes || !valid_header_value(context.message_id) ||
        !valid_header_value(context.root_message_id) || !valid_header_value(context.parent_message_id) ||
        !valid_header_value(context.session_token)) {
        return std::unexpected(RecordError::InvalidContext);
    }

    std::size_t text_size = 0;
    if (!checked_add(context.message_id.size(), context.root_message_id.size(), text_size) ||
        !checked_add(text_size, context.parent_message_id.size(), text_size) ||
        !checked_add(text_size, context.session_token.size(), text_size) ||
        text_size > std::numeric_limits<std::size_t>::max() - sizeof(detail::PropagationContextData)) {
        return std::unexpected(RecordError::LimitExceeded);
    }

    void *storage = ::operator new(sizeof(detail::PropagationContextData) + text_size, std::nothrow);
    if (!storage) {
        return std::unexpected(RecordError::NoMemory);
    }
    auto *data = new (storage) detail::PropagationContextData{
            .message_id_size = context.message_id.size(),
            .root_message_id_size = context.root_message_id.size(),
            .parent_message_id_size = context.parent_message_id.size(),
            .session_token_size = context.session_token.size(),
    };
    char *out = reinterpret_cast<char *>(data + 1);
    for (const std::string_view value:
         {context.message_id, context.root_message_id, context.parent_message_id, context.session_token}) {
        std::copy(value.begin(), value.end(), out);
        out += value.size();
    }
    return PropagationContext(data);
}

std::string_view PropagationContext::message_id() const noexcept {
    return field(data_, 0, data_ ? data_->message_id_size : 0);
}

std::string_view PropagationContext::root_message_id() const noexcept {
    return field(data_, data_ ? data_->message_id_size : 0, data_ ? data_->root_message_id_size : 0);
}

std::string_view PropagationContext::parent_message_id() const noexcept {
    const std::size_t offset = data_ ? data_->message_id_size + data_->root_message_id_size : 0;
    return field(data_, offset, data_ ? data_->parent_message_id_size : 0);
}

std::string_view PropagationContext::session_token() const noexcept {
    const std::size_t offset =
            data_ ? data_->message_id_size + data_->root_message_id_size + data_->parent_message_id_size : 0;
    return field(data_, offset, data_ ? data_->session_token_size : 0);
}

MessageTraceContext PropagationContext::view() const noexcept {
    return {
            .message_id = message_id(),
            .root_message_id = root_message_id(),
            .parent_message_id = parent_message_id(),
            .session_token = session_token(),
    };
}

void PropagationContext::reset() noexcept {
    detail::PropagationContextData *data = std::exchange(data_, nullptr);
    if (data && data->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::destroy_at(data);
        ::operator delete(data);
    }
}

} // namespace fiber::cat
