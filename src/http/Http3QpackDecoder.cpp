#include "Http3QpackDecoder.h"

#include <cstring>
#include <limits>
#include <new>

#include "../common/Assert.h"

namespace fiber::http {

namespace {

constexpr std::uint64_t kMaxQpackInteger = (1ULL << 62U) - 1U;

} // namespace

bool Http3QpackDecoder::init(std::uint32_t max_string_size) noexcept {
    release();
    max_string_size_ = max_string_size;
    return true;
}

void Http3QpackDecoder::release() noexcept {
    scratch_.reset();
    scratch_cap_ = 0;
    ctx_ = nullptr;
    ops_ = nullptr;
    reset_block_state();
}

void Http3QpackDecoder::begin_block(void *ctx, const Ops *ops) noexcept {
    FIBER_ASSERT(ops != nullptr);
    FIBER_ASSERT(ops->on_indexed_field != nullptr);
    FIBER_ASSERT(ops->on_indexed_name != nullptr);
    FIBER_ASSERT(ops->on_name_raw != nullptr);
    FIBER_ASSERT(ops->on_name_huffman != nullptr);
    FIBER_ASSERT(ops->on_value_raw != nullptr);
    FIBER_ASSERT(ops->on_value_huffman != nullptr);
    ctx_ = ctx;
    ops_ = ops;
    reset_block_state();
}

void Http3QpackDecoder::reset_block_state() noexcept {
    state_ = State::PrefixRequiredInsertCount;
    string_huffman_ = false;
    have_literal_name_ = false;
    integer_value_ = 0;
    integer_prefix_max_ = 0;
    integer_shift_ = 0;
    string_length_ = 0;
    string_received_ = 0;
}

common::IoErr Http3QpackDecoder::decode(const std::uint8_t *data, std::size_t len, bool end_block) noexcept {
    if (!ops_ || (data == nullptr && len != 0)) {
        return common::IoErr::Invalid;
    }

    const std::uint8_t *pos = data;
    const std::uint8_t *end = len == 0 ? data : data + len;
    while (pos != end) {
        common::IoErr err = common::IoErr::None;
        switch (state_) {
            case State::PrefixRequiredInsertCount:
                err = parse_required_insert_count(pos, end);
                break;
            case State::PrefixBaseFirst:
                err = parse_base_first(pos, end);
                break;
            case State::Ready:
                err = parse_ready(pos, end);
                break;
            case State::PrefixRequiredInsertCountCont:
            case State::PrefixBaseCont:
            case State::IndexedCont:
            case State::LiteralNameIndexCont:
            case State::LiteralNameLenCont:
            case State::LiteralValueLenCont:
                err = parse_integer_cont(pos, end);
                break;
            case State::LiteralValueLenFirst:
                err = parse_value_length_first(pos, end);
                break;
            case State::LiteralNameData:
            case State::LiteralValueData:
                err = parse_string_data(pos, end);
                break;
        }
        if (err != common::IoErr::None) {
            return err;
        }
    }

    if (end_block && state_ != State::Ready) {
        return common::IoErr::Invalid;
    }
    return common::IoErr::None;
}

common::IoErr Http3QpackDecoder::parse_required_insert_count(const std::uint8_t *&pos,
                                                             const std::uint8_t *end) noexcept {
    if (pos == end) {
        return common::IoErr::None;
    }

    const std::uint8_t byte = *pos++;
    if (byte != 0) {
        return common::IoErr::Invalid;
    }

    integer_prefix_max_ = 0xffU;
    integer_value_ = 0;
    state_ = State::PrefixBaseFirst;
    return common::IoErr::None;
}

common::IoErr Http3QpackDecoder::parse_base_first(const std::uint8_t *&pos, const std::uint8_t *end) noexcept {
    if (pos == end) {
        return common::IoErr::None;
    }

    const std::uint8_t byte = *pos++;
    if ((byte & 0x80U) != 0) {
        return common::IoErr::Invalid;
    }

    integer_prefix_max_ = 0x7fU;
    integer_value_ = byte & integer_prefix_max_;
    if (integer_value_ != integer_prefix_max_) {
        state_ = State::Ready;
        return common::IoErr::None;
    }

    integer_shift_ = 0;
    state_ = State::PrefixBaseCont;
    return common::IoErr::None;
}

common::IoErr Http3QpackDecoder::parse_ready(const std::uint8_t *&pos, const std::uint8_t *end) noexcept {
    if (pos == end) {
        return common::IoErr::None;
    }

    const std::uint8_t byte = *pos++;
    if ((byte & 0x80U) != 0) {
        if ((byte & 0x40U) == 0) {
            return common::IoErr::Invalid;
        }
        integer_prefix_max_ = 0x3fU;
        integer_value_ = byte & integer_prefix_max_;
        if (integer_value_ != integer_prefix_max_) {
            return handle_indexed_field(integer_value_);
        }
        integer_shift_ = 0;
        state_ = State::IndexedCont;
        return common::IoErr::None;
    }

    if ((byte & 0x40U) != 0) {
        if ((byte & 0x10U) == 0) {
            return common::IoErr::Invalid;
        }
        integer_prefix_max_ = 0x0fU;
        integer_value_ = byte & integer_prefix_max_;
        if (integer_value_ != integer_prefix_max_) {
            return handle_indexed_name(integer_value_);
        }
        integer_shift_ = 0;
        state_ = State::LiteralNameIndexCont;
        return common::IoErr::None;
    }

    if ((byte & 0xe0U) == 0x20U) {
        string_huffman_ = (byte & 0x08U) != 0;
        integer_prefix_max_ = 0x07U;
        integer_value_ = byte & integer_prefix_max_;
        if (integer_value_ != integer_prefix_max_) {
            return prepare_string_accumulator(integer_value_, State::LiteralNameData);
        }
        integer_shift_ = 0;
        state_ = State::LiteralNameLenCont;
        return common::IoErr::None;
    }

    return common::IoErr::Invalid;
}

common::IoErr Http3QpackDecoder::parse_integer_cont(const std::uint8_t *&pos, const std::uint8_t *end) noexcept {
    while (pos != end) {
        const std::uint8_t byte = *pos++;
        const std::uint64_t chunk = byte & 0x7fU;
        if (integer_shift_ >= 63U && chunk != 0) {
            return common::IoErr::Invalid;
        }
        const std::uint64_t contribution = chunk << integer_shift_;
        if (contribution > kMaxQpackInteger || integer_value_ > kMaxQpackInteger - contribution) {
            return common::IoErr::Invalid;
        }
        integer_value_ += contribution;
        if ((byte & 0x80U) == 0) {
            switch (state_) {
                case State::PrefixRequiredInsertCountCont:
                    if (integer_value_ != 0) {
                        return common::IoErr::Invalid;
                    }
                    state_ = State::PrefixBaseFirst;
                    return common::IoErr::None;
                case State::PrefixBaseCont:
                    state_ = State::Ready;
                    return common::IoErr::None;
                case State::IndexedCont:
                    state_ = State::Ready;
                    return handle_indexed_field(integer_value_);
                case State::LiteralNameIndexCont:
                    return handle_indexed_name(integer_value_);
                case State::LiteralNameLenCont:
                    return prepare_string_accumulator(integer_value_, State::LiteralNameData);
                case State::LiteralValueLenCont:
                    return prepare_string_accumulator(integer_value_, State::LiteralValueData);
                default:
                    return common::IoErr::Invalid;
            }
        }
        if (integer_shift_ > 56U) {
            return common::IoErr::Invalid;
        }
        integer_shift_ = static_cast<std::uint8_t>(integer_shift_ + 7U);
    }
    return common::IoErr::None;
}

common::IoErr Http3QpackDecoder::parse_value_length_first(const std::uint8_t *&pos, const std::uint8_t *end) noexcept {
    if (pos == end) {
        return common::IoErr::None;
    }

    const std::uint8_t byte = *pos++;
    string_huffman_ = (byte & 0x80U) != 0;
    integer_prefix_max_ = 0x7fU;
    integer_value_ = byte & integer_prefix_max_;
    if (integer_value_ != integer_prefix_max_) {
        return prepare_string_accumulator(integer_value_, State::LiteralValueData);
    }
    integer_shift_ = 0;
    state_ = State::LiteralValueLenCont;
    return common::IoErr::None;
}

common::IoErr Http3QpackDecoder::parse_string_data(const std::uint8_t *&pos, const std::uint8_t *end) noexcept {
    const std::size_t remaining = string_length_ - string_received_;
    const std::size_t available = static_cast<std::size_t>(end - pos);
    const std::size_t take = remaining < available ? remaining : available;
    if (take == 0) {
        return handle_string_complete();
    }

    if (!ensure_scratch_capacity(string_length_)) {
        return common::IoErr::NoMem;
    }
    std::memcpy(scratch_.get() + string_received_, pos, take);
    pos += take;
    string_received_ += static_cast<std::uint32_t>(take);
    return handle_string_complete();
}

common::IoErr Http3QpackDecoder::handle_indexed_field(std::uint64_t index) noexcept {
    TableEntryView entry;
    if (!resolve_static_index(index, entry)) {
        return common::IoErr::Invalid;
    }
    return ops_->on_indexed_field(ctx_, entry);
}

common::IoErr Http3QpackDecoder::handle_indexed_name(std::uint64_t index) noexcept {
    TableEntryView entry;
    if (!resolve_static_index(index, entry)) {
        return common::IoErr::Invalid;
    }
    if (entry.name.size() > max_string_size_) {
        return common::IoErr::Invalid;
    }
    have_literal_name_ = true;
    common::IoErr err = ops_->on_indexed_name(ctx_, entry.name, entry.name_hash);
    if (err != common::IoErr::None) {
        return err;
    }
    return handle_literal_value_start();
}

common::IoErr Http3QpackDecoder::handle_string_complete() noexcept {
    if (string_received_ != string_length_) {
        return common::IoErr::None;
    }

    if (state_ == State::LiteralNameData) {
        common::IoErr err = string_huffman_ ? ops_->on_name_huffman(ctx_, scratch_.get(), string_length_)
                                            : ops_->on_name_raw(ctx_, scratch_.get(), string_length_);
        if (err != common::IoErr::None) {
            return err;
        }
        have_literal_name_ = true;
        return handle_literal_value_start();
    }

    common::IoErr err = string_huffman_ ? ops_->on_value_huffman(ctx_, scratch_.get(), string_length_)
                                        : ops_->on_value_raw(ctx_, scratch_.get(), string_length_);
    if (err != common::IoErr::None) {
        return err;
    }
    finish_literal_field();
    return common::IoErr::None;
}

common::IoErr Http3QpackDecoder::handle_literal_value_start() noexcept {
    if (!have_literal_name_) {
        return common::IoErr::Invalid;
    }
    state_ = State::LiteralValueLenFirst;
    reset_string_accumulator();
    return common::IoErr::None;
}

common::IoErr Http3QpackDecoder::prepare_string_accumulator(std::uint64_t string_length, State data_state) noexcept {
    if (string_length > max_string_size_) {
        return common::IoErr::Invalid;
    }
    string_length_ = static_cast<std::uint32_t>(string_length);
    string_received_ = 0;
    state_ = data_state;
    return handle_string_complete();
}

bool Http3QpackDecoder::resolve_static_index(std::uint64_t index, TableEntryView &entry) const noexcept {
    if (index > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    return Http3QpackStaticTable::get_by_index(static_cast<std::uint32_t>(index), entry);
}

bool Http3QpackDecoder::ensure_scratch_capacity(std::size_t size) noexcept {
    if (size <= scratch_cap_) {
        return true;
    }
    if (size > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    std::unique_ptr<std::uint8_t[]> next(new (std::nothrow) std::uint8_t[size]);
    if (!next) {
        return false;
    }
    if (scratch_ && string_received_ != 0) {
        std::memcpy(next.get(), scratch_.get(), string_received_);
    }
    scratch_ = std::move(next);
    scratch_cap_ = static_cast<std::uint32_t>(size);
    return true;
}

void Http3QpackDecoder::reset_string_accumulator() noexcept {
    string_length_ = 0;
    string_received_ = 0;
    string_huffman_ = false;
}

void Http3QpackDecoder::finish_literal_field() noexcept {
    have_literal_name_ = false;
    reset_string_accumulator();
    state_ = State::Ready;
}

} // namespace fiber::http
