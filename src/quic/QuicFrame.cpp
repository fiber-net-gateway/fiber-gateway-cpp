#include "QuicFrame.h"

#include <cstring>
#include <expected>
#include <new>

namespace fiber::quic {

common::IoResult<void> quic_frame_set_owned_data(QuicFrame &frame, const std::uint8_t *data, std::size_t len) noexcept {
    if ((data == nullptr && len != 0) || frame.data_block != nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (len == 0) {
        frame.data = {nullptr, 0};
        return {};
    }

    auto *block = new (std::nothrow) QuicFrameDataBlock{};
    if (block == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }

    block->data = new (std::nothrow) std::uint8_t[len];
    if (block->data == nullptr) {
        delete block;
        return std::unexpected(common::IoErr::NoMem);
    }

    std::memcpy(block->data, data, len);
    block->len = len;
    block->refs = 1;
    frame.data_block = block;
    frame.data = {block->data, len};
    return {};
}

void quic_frame_retain_data(QuicFrame &frame) noexcept {
    if (frame.data_block != nullptr) {
        ++frame.data_block->refs;
    }
}

void quic_frame_release_data(QuicFrame &frame) noexcept {
    QuicFrameDataBlock *block = frame.data_block;
    if (block == nullptr) {
        return;
    }

    frame.data_block = nullptr;
    frame.data = {};
    if (block->refs > 1) {
        --block->refs;
        return;
    }

    delete[] block->data;
    delete block;
}

} // namespace fiber::quic
