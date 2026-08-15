#if FIBER_ENABLE_SCRIPT_JIT

#include "StackMapPlugin.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <llvm/ExecutionEngine/JITLink/JITLink.h>
#include <llvm/Support/Error.h>

namespace fiber::script::jit::llvm_detail {

namespace {

class StackMapReader final {
public:
    StackMapReader(llvm::ArrayRef<char> bytes, llvm::endianness endian) noexcept : bytes_(bytes), endian_(endian) {}

    template<typename T>
    bool read(T &out) noexcept {
        static_assert(std::is_integral_v<T>);
        if (remaining() < sizeof(T)) {
            return false;
        }
        std::make_unsigned_t<T> value = 0;
        if (endian_ == llvm::endianness::little) {
            for (std::size_t i = 0; i < sizeof(T); ++i) {
                value |= static_cast<std::make_unsigned_t<T>>(static_cast<unsigned char>(bytes_[offset_ + i]))
                         << (i * 8u);
            }
        } else {
            for (std::size_t i = 0; i < sizeof(T); ++i) {
                value = static_cast<std::make_unsigned_t<T>>((value << 8u) |
                                                             static_cast<unsigned char>(bytes_[offset_ + i]));
            }
        }
        std::memcpy(&out, &value, sizeof(T));
        offset_ += sizeof(T);
        return true;
    }

    bool skip(std::size_t count) noexcept {
        if (remaining() < count) {
            return false;
        }
        offset_ += count;
        return true;
    }

    bool align(std::size_t alignment) noexcept {
        std::size_t aligned = (offset_ + alignment - 1u) & ~(alignment - 1u);
        return aligned >= offset_ && skip(aligned - offset_);
    }

    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }

private:
    llvm::ArrayRef<char> bytes_;
    llvm::endianness endian_;
    std::size_t offset_ = 0;
};

struct StackMapFunction {
    std::uint64_t address = 0;
    std::uint64_t stack_size = 0;
    std::uint64_t record_count = 0;
};

llvm::Error malformed(const char *message) {
    return llvm::make_error<llvm::StringError>(std::string("invalid LLVM stack map: ") + message,
                                               llvm::inconvertibleErrorCode());
}

llvm::Expected<llvm::ArrayRef<char>> stack_map_bytes(llvm::jitlink::LinkGraph &graph) {
    llvm::jitlink::Section *section = graph.findSectionByName(".llvm_stackmaps");
    if (!section) {
        section = graph.findSectionByName("__llvm_stackmaps");
    }
    if (!section) {
        return llvm::ArrayRef<char>{};
    }
    llvm::jitlink::Block *content = nullptr;
    for (llvm::jitlink::Block *block: section->blocks()) {
        if (block->getSize() == 0) {
            continue;
        }
        if (content) {
            return malformed("stack map section is split into multiple blocks");
        }
        content = block;
    }
    if (!content) {
        return llvm::ArrayRef<char>{};
    }
    return content->getContent();
}

llvm::Error capture_stack_maps(llvm::jitlink::LinkGraph &graph,
                               const std::shared_ptr<run::NativeStackMapTable> &table) {
    auto bytes = stack_map_bytes(graph);
    if (!bytes) {
        return bytes.takeError();
    }
    if (bytes->empty()) {
        table->set_records({});
        return llvm::Error::success();
    }
    if (bytes->size() > 16u * 1024u * 1024u) {
        return malformed("section exceeds size limit");
    }
    StackMapReader reader(*bytes, graph.getEndianness());
    std::uint8_t version = 0;
    std::uint8_t reserved8 = 0;
    std::uint16_t reserved16 = 0;
    std::uint32_t function_count = 0;
    std::uint32_t constant_count = 0;
    std::uint32_t record_count = 0;
    if (!reader.read(version) || !reader.read(reserved8) || !reader.read(reserved16) || !reader.read(function_count) ||
        !reader.read(constant_count) || !reader.read(record_count)) {
        return malformed("truncated header");
    }
    if (version != 3 || reserved8 != 0 || reserved16 != 0) {
        return malformed("unsupported header");
    }
    if (function_count > 65536u || record_count > 1048576u || constant_count > 1048576u) {
        return malformed("record count exceeds limit");
    }

    std::vector<StackMapFunction> functions;
    functions.reserve(function_count);
    std::uint64_t summed_records = 0;
    for (std::uint32_t i = 0; i < function_count; ++i) {
        StackMapFunction function;
        if (!reader.read(function.address) || !reader.read(function.stack_size) ||
            !reader.read(function.record_count)) {
            return malformed("truncated function table");
        }
        if (function.record_count > record_count || summed_records > record_count - function.record_count) {
            return malformed("inconsistent function record count");
        }
        summed_records += function.record_count;
        functions.push_back(function);
    }
    if (summed_records != record_count ||
        !reader.skip(static_cast<std::size_t>(constant_count) * sizeof(std::uint64_t))) {
        return malformed("truncated constant table");
    }

    const std::uint16_t expected_sp_register = graph.getTargetTriple().isAArch64() ? 31u : 7u;
    if (!graph.getTargetTriple().isAArch64() && !graph.getTargetTriple().isX86()) {
        return malformed("unsupported target architecture");
    }
    const std::uint16_t pointer_size = static_cast<std::uint16_t>(graph.getPointerSize());
    std::vector<run::NativeStackMapRecord> records;
    records.reserve(record_count);
    std::uint32_t records_read = 0;
    for (const StackMapFunction &function: functions) {
        for (std::uint64_t index = 0; index < function.record_count; ++index) {
            std::uint64_t statepoint_id = 0;
            std::uint32_t instruction_offset = 0;
            std::uint16_t record_reserved = 0;
            std::uint16_t location_count = 0;
            if (!reader.read(statepoint_id) || !reader.read(instruction_offset) || !reader.read(record_reserved) ||
                !reader.read(location_count)) {
                return malformed("truncated record header");
            }
            (void) statepoint_id;
            if (record_reserved != 0 || location_count > 32768u) {
                return malformed("invalid record header");
            }
            run::NativeStackMapRecord record;
            if (function.address > std::numeric_limits<std::uintptr_t>::max() - instruction_offset) {
                return malformed("return PC overflow");
            }
            record.return_pc = static_cast<std::uintptr_t>(function.address + instruction_offset);
            for (std::uint16_t location_index = 0; location_index < location_count; ++location_index) {
                std::uint8_t kind = 0;
                std::uint8_t location_reserved = 0;
                std::uint16_t size = 0;
                std::uint16_t dwarf_register = 0;
                std::uint16_t location_reserved16 = 0;
                std::int32_t offset = 0;
                if (!reader.read(kind) || !reader.read(location_reserved) || !reader.read(size) ||
                    !reader.read(dwarf_register) || !reader.read(location_reserved16) || !reader.read(offset)) {
                    return malformed("truncated location");
                }
                if (location_reserved != 0 || location_reserved16 != 0) {
                    return malformed("non-zero location reserved field");
                }
                // Kind 3 is Indirect: the root pointer is stored at register + offset.
                if (kind == 3u && size == pointer_size) {
                    if (dwarf_register != expected_sp_register) {
                        return malformed("root is not relative to the captured stack pointer");
                    }
                    if (offset < -static_cast<std::int64_t>(function.stack_size) - 4096 || offset > 4096) {
                        return malformed("root stack offset is outside the function frame");
                    }
                    record.stack_offsets.push_back(offset);
                } else if ((kind == 1u || kind == 2u) && size == pointer_size) {
                    return malformed("register/direct root location is unsupported");
                } else if (kind < 1u || kind > 5u) {
                    return malformed("unknown location kind");
                }
            }
            if (!reader.align(8)) {
                return malformed("truncated location padding");
            }
            std::uint16_t liveout_padding = 0;
            std::uint16_t liveout_count = 0;
            if (!reader.read(liveout_padding) || !reader.read(liveout_count) || liveout_padding != 0 ||
                !reader.skip(static_cast<std::size_t>(liveout_count) * 4u) || !reader.align(8)) {
                return malformed("truncated live-out table");
            }
            records.push_back(std::move(record));
            ++records_read;
        }
    }
    if (records_read != record_count) {
        return malformed("record count mismatch");
    }
    table->set_records(std::move(records));
    return llvm::Error::success();
}

} // namespace

void StackMapCapturePlugin::bind(llvm::orc::ResourceKey key, std::shared_ptr<run::NativeStackMapTable> table) {
    std::lock_guard lock(mutex_);
    tables_[key] = std::move(table);
}

std::shared_ptr<run::NativeStackMapTable> StackMapCapturePlugin::table_for(llvm::orc::ResourceKey key) {
    std::lock_guard lock(mutex_);
    auto it = tables_.find(key);
    return it == tables_.end() ? nullptr : it->second;
}

void StackMapCapturePlugin::erase(llvm::orc::ResourceKey key) {
    std::lock_guard lock(mutex_);
    tables_.erase(key);
}

void StackMapCapturePlugin::modifyPassConfig(llvm::orc::MaterializationResponsibility &responsibility,
                                             llvm::jitlink::LinkGraph &graph,
                                             llvm::jitlink::PassConfiguration &config) {
    (void) graph;
    llvm::orc::ResourceKey key = 0;
    if (llvm::Error error = responsibility.withResourceKeyDo([&](llvm::orc::ResourceKey value) { key = value; })) {
        llvm::consumeError(std::move(error));
        return;
    }
    std::shared_ptr<run::NativeStackMapTable> table = table_for(key);
    if (!table) {
        return;
    }
    config.PrePrunePasses.push_back([](llvm::jitlink::LinkGraph &linked_graph) -> llvm::Error {
        llvm::jitlink::Section *section = linked_graph.findSectionByName(".llvm_stackmaps");
        if (!section) {
            section = linked_graph.findSectionByName("__llvm_stackmaps");
        }
        if (!section) {
            return llvm::Error::success();
        }
        for (llvm::jitlink::Symbol *symbol: section->symbols()) {
            symbol->setLive(true);
        }
        if (section->symbols_size() == 0) {
            for (llvm::jitlink::Block *block: section->blocks()) {
                linked_graph.addAnonymousSymbol(*block, 0, block->getSize(), false, true);
            }
        }
        return llvm::Error::success();
    });
    config.PostFixupPasses.push_back([table = std::move(table)](llvm::jitlink::LinkGraph &linked_graph) -> llvm::Error {
        return capture_stack_maps(linked_graph, table);
    });
}

llvm::Error StackMapCapturePlugin::notifyFailed(llvm::orc::MaterializationResponsibility &responsibility) {
    llvm::orc::ResourceKey key = 0;
    if (llvm::Error error = responsibility.withResourceKeyDo([&](llvm::orc::ResourceKey value) { key = value; })) {
        return error;
    }
    erase(key);
    return llvm::Error::success();
}

llvm::Error StackMapCapturePlugin::notifyRemovingResources(llvm::orc::JITDylib &dylib, llvm::orc::ResourceKey key) {
    (void) dylib;
    erase(key);
    return llvm::Error::success();
}

void StackMapCapturePlugin::notifyTransferringResources(llvm::orc::JITDylib &dylib, llvm::orc::ResourceKey destination,
                                                        llvm::orc::ResourceKey source) {
    (void) dylib;
    std::lock_guard lock(mutex_);
    auto source_it = tables_.find(source);
    if (source_it == tables_.end()) {
        return;
    }
    tables_[destination] = std::move(source_it->second);
    tables_.erase(source_it);
}

} // namespace fiber::script::jit::llvm_detail

#endif
