#include <fiber/tls/record/TlsRecordWriter.h>

#include <algorithm>
#include <utility>

namespace fiber::tls {

common::IoResult<void> TlsRecordWriter::write(TlsContentType type, mem::IoBuf &&payload,
                                              mem::IoBufChain &out) noexcept {
    mem::IoBufChain chain;
    if (!chain.append(std::move(payload))) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return write(type, std::move(chain), out);
}

common::IoResult<void> TlsRecordWriter::write(TlsContentType type, mem::IoBufChain &&payload,
                                              mem::IoBufChain &out) noexcept {
    const bool empty = payload.readable_bytes() == 0;
    while (payload.readable_bytes() > 0) {
        const std::size_t chunk = std::min(payload.readable_bytes(), kTlsMaxPlaintextSize);

        // Take the record's bytes out of the payload chain: whole nodes move,
        // a node straddling the boundary leaves a zero-copy retained view.
        // take_prefix rolls back cleanly on allocation failure.
        mem::IoBufChain record_payload;
        if (!payload.take_prefix(chunk, record_payload)) {
            return std::unexpected(common::IoErr::NoMem);
        }

        if (const auto header = write_header(type, chunk, out); !header.has_value()) {
            return header;
        }
        if (!out.append_chain(std::move(record_payload))) {
            return std::unexpected(common::IoErr::NoMem);
        }
    }

    if (empty) {
        // Uniform semantics: a write always emits at least one record.
        if (const auto header = write_header(type, 0, out); !header.has_value()) {
            return header;
        }
    }
    return {};
}

common::IoResult<void> TlsRecordWriter::write_header(TlsContentType type, std::size_t length,
                                                     mem::IoBufChain &out) noexcept {
    mem::IoBuf header = mem::IoBuf::allocate(kTlsRecordHeaderSize);
    if (!header.valid()) {
        return std::unexpected(common::IoErr::NoMem);
    }
    tls_encode_record_header(header.writable_data(), type, legacy_version_, static_cast<std::uint16_t>(length));
    header.commit(kTlsRecordHeaderSize);
    if (!out.append(std::move(header))) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return {};
}

} // namespace fiber::tls
