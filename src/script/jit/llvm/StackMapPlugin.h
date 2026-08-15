#ifndef FIBER_SCRIPT_JIT_LLVM_STACK_MAP_PLUGIN_H
#define FIBER_SCRIPT_JIT_LLVM_STACK_MAP_PLUGIN_H

#if FIBER_ENABLE_SCRIPT_JIT

#include <memory>
#include <mutex>
#include <unordered_map>

#include <llvm/ExecutionEngine/Orc/LinkGraphLinkingLayer.h>

#include <fiber/script/run/JitCode.h>

namespace fiber::script::jit::llvm_detail {

class StackMapCapturePlugin final : public llvm::orc::LinkGraphLinkingLayer::Plugin {
public:
    void bind(llvm::orc::ResourceKey key, std::shared_ptr<run::NativeStackMapTable> table);

    void modifyPassConfig(llvm::orc::MaterializationResponsibility &responsibility, llvm::jitlink::LinkGraph &graph,
                          llvm::jitlink::PassConfiguration &config) override;
    llvm::Error notifyFailed(llvm::orc::MaterializationResponsibility &responsibility) override;
    llvm::Error notifyRemovingResources(llvm::orc::JITDylib &dylib, llvm::orc::ResourceKey key) override;
    void notifyTransferringResources(llvm::orc::JITDylib &dylib, llvm::orc::ResourceKey destination,
                                     llvm::orc::ResourceKey source) override;

private:
    std::shared_ptr<run::NativeStackMapTable> table_for(llvm::orc::ResourceKey key);
    void erase(llvm::orc::ResourceKey key);

    std::mutex mutex_;
    std::unordered_map<llvm::orc::ResourceKey, std::shared_ptr<run::NativeStackMapTable>> tables_;
};

} // namespace fiber::script::jit::llvm_detail

#endif

#endif // FIBER_SCRIPT_JIT_LLVM_STACK_MAP_PLUGIN_H
