#include "Http2HpackDecoder.h"

#include <cstring>
#include <limits>

#include "../common/Assert.h"

namespace fiber::http {

namespace {

constexpr std::uint32_t kStaticTableEntryCount = Http2HpackStaticTable::kEntryCount;

} // namespace

bool Http2HpackDecoder::init(std::uint32_t max_dynamic_table_size, std::uint32_t max_string_size) noexcept {
    release();
    max_dynamic_table_size_ = max_dynamic_table_size;
    max_string_size_ = max_string_size;
    return decode_table_.init(max_dynamic_table_size_);
}

void Http2HpackDecoder::release() noexcept {
    decode_table_.release();
    scratch_.reset();
    scratch_cap_ = 0;
    ctx_ = nullptr;
    ops_ = nullptr;
    reset_block_state();
}

void Http2HpackDecoder::begin_block(void *ctx, const Ops *ops) noexcept {
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

void Http2HpackDecoder::reset_block_state() noexcept {
    state_ = State::Ready;
    literal_mode_ = LiteralMode::WithoutIndexing;
    string_huffman_ = false;
    allow_table_size_update_ = true;
    have_literal_name_ = false;
    integer_value_ = 0;
    integer_prefix_max_ = 0;
    integer_shift_ = 0;
    string_length_ = 0;
    string_received_ = 0;
}

common::IoErr Http2HpackDecoder::decode(const std::uint8_t *data, std::size_t len, bool end_block) noexcept {
    if (!ops_) {
        return common::IoErr::Invalid;
    }

    const std::uint8_t *pos = data;
    const std::uint8_t *end = data + len;
    while (pos != end) {
        common::IoErr err = common::IoErr::None;
        switch (state_) {
            case State::Ready:
                err = parse_ready(pos, end);
                break;
            case State::IndexedCont:
            case State::LiteralNameIndexCont:
            case State::LiteralNameLenCont:
            case State::LiteralValueLenCont:
            case State::TableSizeUpdateCont:
                err = parse_integer_cont(pos, end);
                break;
            case State::LiteralNameLenFirst:
            case State::LiteralValueLenFirst: {
                std::uint8_t byte = *pos++;
                string_huffman_ = (byte & 0x80U) != 0;
                integer_prefix_max_ = 0x7fU;
                integer_value_ = byte & integer_prefix_max_;
                if (integer_value_ != integer_prefix_max_) {
                    err = prepare_string_accumulator(
                        integer_value_, state_ == State::LiteralNameLenFirst ? State::LiteralNameData : State::LiteralValueData);
                } else {
                    integer_shift_ = 0;
                    state_ = state_ == State::LiteralNameLenFirst ? State::LiteralNameLenCont : State::LiteralValueLenCont;
                }
                break;
            }
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

common::IoErr Http2HpackDecoder::parse_ready(const std::uint8_t *&pos, const std::uint8_t *end) noexcept {
    if (pos == end) {
        return common::IoErr::None;
    }

    std::uint8_t byte = *pos++;
    if ((byte & 0x80U) != 0) {
        allow_table_size_update_ = false;
        integer_prefix_max_ = 0x7fU;
        integer_value_ = byte & integer_prefix_max_;
        if (integer_value_ != integer_prefix_max_) {
            return handle_indexed_field(integer_value_);
        }
        integer_shift_ = 0;
        state_ = State::IndexedCont;
        return common::IoErr::None;
    }

    if ((byte & 0x40U) != 0) {
        allow_table_size_update_ = false;
        literal_mode_ = LiteralMode::IncrementalIndexing;
        integer_prefix_max_ = 0x3fU;
        integer_value_ = byte & integer_prefix_max_;
        if (integer_value_ != integer_prefix_max_) {
            if (integer_value_ == 0) {
                state_ = State::LiteralNameLenFirst;
            } else {
                return handle_indexed_name(integer_value_);
            }
            return common::IoErr::None;
        }
        integer_shift_ = 0;
        state_ = State::LiteralNameIndexCont;
        return common::IoErr::None;
    }

    if ((byte & 0x20U) != 0) {
        if (!allow_table_size_update_) {
            return common::IoErr::Invalid;
        }
        integer_prefix_max_ = 0x1fU;
        integer_value_ = byte & integer_prefix_max_;
        if (integer_value_ != integer_prefix_max_) {
            return apply_table_size_update(integer_value_);
        }
        integer_shift_ = 0;
        state_ = State::TableSizeUpdateCont;
        return common::IoErr::None;
    }

    allow_table_size_update_ = false;
    literal_mode_ = (byte & 0x10U) != 0 ? LiteralMode::NeverIndexed : LiteralMode::WithoutIndexing;
    integer_prefix_max_ = 0x0fU;
    integer_value_ = byte & integer_prefix_max_;
    if (integer_value_ != integer_prefix_max_) {
        if (integer_value_ == 0) {
            state_ = State::LiteralNameLenFirst;
        } else {
            return handle_indexed_name(integer_value_);
        }
        return common::IoErr::None;
    }
    integer_shift_ = 0;
    state_ = State::LiteralNameIndexCont;
    return common::IoErr::None;
}

common::IoErr Http2HpackDecoder::parse_integer_cont(const std::uint8_t *&pos, const std::uint8_t *end) noexcept {
    while (pos != end) {
        std::uint8_t byte = *pos++;
        if (integer_shift_ >= 28U && (byte & 0x7fU) != 0) {
            return common::IoErr::Invalid;
        }
        integer_value_ += static_cast<std::uint32_t>(byte & 0x7fU) << integer_shift_;
        if ((byte & 0x80U) == 0) {
            switch (state_) {
                case State::IndexedCont:
                    state_ = State::Ready;
                    return handle_indexed_field(integer_value_);
                case State::LiteralNameIndexCont:
                    if (integer_value_ == 0) {
                        state_ = State::LiteralNameLenFirst;
                    } else {
                        return handle_indexed_name(integer_value_);
                    }
                    return common::IoErr::None;
                case State::LiteralNameLenCont:
                    return prepare_string_accumulator(integer_value_, State::LiteralNameData);
                case State::LiteralValueLenCont:
                    return prepare_string_accumulator(integer_value_, State::LiteralValueData);
                case State::TableSizeUpdateCont:
                    state_ = State::Ready;
                    return apply_table_size_update(integer_value_);
                default:
                    return common::IoErr::Invalid;
            }
        }
        integer_shift_ += 7U;
    }
    return common::IoErr::None;
}

common::IoErr Http2HpackDecoder::parse_string_data(const std::uint8_t *&pos, const std::uint8_t *end) noexcept {
    std::size_t remaining = string_length_ - string_received_;
    std::size_t available = static_cast<std::size_t>(end - pos);
    std::size_t take = remaining < available ? remaining : available;
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

common::IoErr Http2HpackDecoder::handle_indexed_field(std::uint32_t index) noexcept {
    TableEntryView entry;
    if (!resolve_index(index, entry)) {
        return common::IoErr::Invalid;
    }
    return ops_->on_indexed_field(ctx_, entry);
}

common::IoErr Http2HpackDecoder::handle_indexed_name(std::uint32_t index) noexcept {
    TableEntryView entry;
    if (!resolve_index(index, entry)) {
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

common::IoErr Http2HpackDecoder::handle_string_complete() noexcept {
    if (string_received_ != string_length_) {
        return common::IoErr::None;
    }

    if (state_ == State::LiteralNameData) {
        common::IoErr err =
            string_huffman_ ? ops_->on_name_huffman(ctx_, scratch_.get(), string_length_)
                            : ops_->on_name_raw(ctx_, scratch_.get(), string_length_);
        if (err != common::IoErr::None) {
            return err;
        }
        have_literal_name_ = true;
        return handle_literal_value_start();
    }

    FieldView field;
    FieldView *field_out = literal_mode_ == LiteralMode::IncrementalIndexing ? &field : nullptr;
    common::IoErr err = string_huffman_ ? ops_->on_value_huffman(ctx_, scratch_.get(), string_length_, field_out)
                                        : ops_->on_value_raw(ctx_, scratch_.get(), string_length_, field_out);
    if (err != common::IoErr::None) {
        return err;
    }
    if (field_out != nullptr &&
        ((!field.name.data() && !field.name.empty()) || (!field.value.data() && !field.value.empty()))) {
        return common::IoErr::Invalid;
    }
    if (field_out != nullptr) {
        (void)decode_table_.insert(field.name, field.value);
    }
    finish_literal_field();
    return common::IoErr::None;
}

common::IoErr Http2HpackDecoder::handle_literal_value_start() noexcept {
    if (!have_literal_name_) {
        return common::IoErr::Invalid;
    }
    state_ = State::LiteralValueLenFirst;
    reset_string_accumulator();
    return common::IoErr::None;
}

common::IoErr Http2HpackDecoder::apply_table_size_update(std::uint32_t size) noexcept {
    if (size > max_dynamic_table_size_) {
        return common::IoErr::Invalid;
    }
    decode_table_.set_max_size(size);
    return common::IoErr::None;
}

common::IoErr Http2HpackDecoder::prepare_string_accumulator(std::uint32_t string_length, State data_state) noexcept {
    if (string_length > max_string_size_) {
        return common::IoErr::Invalid;
    }
    string_length_ = string_length;
    string_received_ = 0;
    state_ = data_state;
    return handle_string_complete();
}

bool Http2HpackDecoder::resolve_index(std::uint32_t index, TableEntryView &entry) const noexcept {
    if (index == 0) {
        return false;
    }

    if (index <= kStaticTableEntryCount) {
        Http2HpackStaticTable::TableEntryView static_entry;
        if (!Http2HpackStaticTable::get_by_index(index, static_entry)) {
            return false;
        }
        entry.name = static_entry.name;
        entry.value = static_entry.value;
        entry.name_hash = static_entry.name_hash;
        return true;
    }

    Http2HpackDecodeTable::TableEntryView dynamic_entry;
    if (!decode_table_.get_by_index(index - kStaticTableEntryCount, dynamic_entry)) {
        return false;
    }
    entry.name = dynamic_entry.name;
    entry.value = dynamic_entry.value;
    entry.name_hash = dynamic_entry.name_hash;
    return true;
}

bool Http2HpackDecoder::ensure_scratch_capacity(std::size_t size) noexcept {
    if (size <= scratch_cap_) {
        return true;
    }
    if (size > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    auto next = std::make_unique<std::uint8_t[]>(size);
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

void Http2HpackDecoder::reset_string_accumulator() noexcept {
    string_length_ = 0;
    string_received_ = 0;
    string_huffman_ = false;
}

void Http2HpackDecoder::finish_literal_field() noexcept {
    have_literal_name_ = false;
    reset_string_accumulator();
    state_ = State::Ready;
}

} // namespace fiber::http
