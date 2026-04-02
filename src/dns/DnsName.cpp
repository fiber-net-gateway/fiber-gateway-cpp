#include "DnsName.h"

#include <cstring>

namespace fiber::dns {

namespace {

constexpr std::size_t kMaxWireNameLen = 255;

} // namespace

common::IoResult<std::size_t> encode_name(std::string_view name,
                                          std::uint8_t *dst,
                                          std::size_t cap) noexcept {
    if ((dst == nullptr && cap != 0) || name.size() > kMaxWireNameLen) {
        return std::unexpected(common::IoErr::Invalid);
    }

    if (name.empty() || name == ".") {
        if (cap < 1) {
            return std::unexpected(common::IoErr::NoMem);
        }
        dst[0] = 0;
        return 1;
    }

    std::size_t write_pos = 0;
    std::size_t cursor = 0;
    while (cursor < name.size()) {
        std::size_t label_end = name.find('.', cursor);
        if (label_end == std::string_view::npos) {
            label_end = name.size();
        }
        std::size_t label_len = label_end - cursor;
        if (label_len == 0 || label_len > 63) {
            return std::unexpected(common::IoErr::Invalid);
        }
        if (write_pos + 1 + label_len + 1 > cap || write_pos + 1 + label_len + 1 > kMaxWireNameLen) {
            return std::unexpected(write_pos + 1 + label_len + 1 > cap ? common::IoErr::NoMem : common::IoErr::Invalid);
        }

        dst[write_pos++] = static_cast<std::uint8_t>(label_len);
        std::memcpy(dst + write_pos, name.data() + cursor, label_len);
        write_pos += label_len;

        if (label_end == name.size()) {
            break;
        }
        cursor = label_end + 1;
        if (cursor == name.size()) {
            break;
        }
    }

    if (write_pos >= cap) {
        return std::unexpected(common::IoErr::NoMem);
    }
    dst[write_pos++] = 0;
    return write_pos;
}

common::IoResult<DecodedName> decode_name(const std::uint8_t *packet,
                                          std::size_t packet_len,
                                          std::size_t offset,
                                          char *storage,
                                          std::size_t storage_cap) noexcept {
    if (packet == nullptr || offset >= packet_len || (storage == nullptr && storage_cap != 0)) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::size_t pos = offset;
    std::size_t next_offset = offset;
    std::size_t out_len = 0;
    std::size_t jumps = 0;
    bool jumped = false;

    while (true) {
        if (pos >= packet_len) {
            return std::unexpected(common::IoErr::Invalid);
        }

        std::uint8_t byte = packet[pos];
        if (byte == 0) {
            if (!jumped) {
                next_offset = pos + 1;
            }
            return DecodedName{std::string_view(storage, out_len), next_offset};
        }

        if ((byte & 0xc0U) == 0xc0U) {
            if (pos + 1 >= packet_len) {
                return std::unexpected(common::IoErr::Invalid);
            }
            std::size_t target = (static_cast<std::size_t>(byte & 0x3fU) << 8U) | packet[pos + 1];
            if (target >= packet_len || target >= pos) {
                return std::unexpected(common::IoErr::Invalid);
            }
            if (!jumped) {
                next_offset = pos + 2;
                jumped = true;
            }
            pos = target;
            if (++jumps > packet_len) {
                return std::unexpected(common::IoErr::Invalid);
            }
            continue;
        }

        if ((byte & 0xc0U) != 0 || byte > 63) {
            return std::unexpected(common::IoErr::Invalid);
        }

        std::size_t label_len = byte;
        if (pos + 1 + label_len > packet_len) {
            return std::unexpected(common::IoErr::Invalid);
        }
        if (out_len != 0) {
            if (out_len >= storage_cap) {
                return std::unexpected(common::IoErr::NoMem);
            }
            storage[out_len++] = '.';
        }
        if (out_len + label_len > storage_cap || out_len + label_len > kMaxWireNameLen) {
            return std::unexpected(out_len + label_len > storage_cap ? common::IoErr::NoMem : common::IoErr::Invalid);
        }
        std::memcpy(storage + out_len, packet + pos + 1, label_len);
        out_len += label_len;
        pos += 1 + label_len;
    }
}

} // namespace fiber::dns
