#ifndef SORBET_COMPILER_LLVM_IR_EMITTER_IMPL_H
#define SORBET_COMPILER_LLVM_IR_EMITTER_IMPL_H
#include "IREmitter.h"
#include "compiler/Core/ForwardDeclarations.h"
#include "core/core.h"
#include <string_view>
#include <vector>

namespace sorbet::compiler {

struct BasicBlockMap;

struct Alias {
    enum class AliasKind { Constant, InstanceField, ClassField, GlobalField };
    AliasKind kind;
    core::SymbolRef constantSym;
    core::NameRef instanceField;
    core::NameRef classField;
    core::SymbolRef globalField;
    static Alias forConstant(core::SymbolRef sym) {
        Alias ret;
        ret.kind = AliasKind::Constant;
        ret.constantSym = sym;
        return ret;
    }
    static Alias forClassField(core::NameRef name) {
        Alias ret;
        ret.kind = AliasKind::ClassField;
        ret.classField = name;
        return ret;
    }
    static Alias forInstanceField(core::NameRef name) {
        Alias ret;
        ret.kind = AliasKind::InstanceField;
        ret.instanceField = name;
        return ret;
    }
    static Alias forGlobalField(core::SymbolRef sym) {
        Alias ret;
        ret.kind = AliasKind::GlobalField;
        ret.globalField = sym;
        return ret;
    }
};

class IREmitterHelpers {
public:
    static bool isStaticInit(CompilerState &cs, core::SymbolRef sym);
    static std::string getFunctionName(CompilerState &cs, core::SymbolRef sym);
    static llvm::Function *lookupFunction(CompilerState &cs, core::SymbolRef sym);
    static llvm::Function *getOrCreateFunctionWeak(CompilerState &cs, core::SymbolRef sym);
    static llvm::Function *cleanFunctionBody(CompilerState &cs, llvm::Function *func);
    static llvm::Function *getOrCreateFunction(CompilerState &cs, core::SymbolRef sym);

    static llvm::Function *getInitFunction(CompilerState &cs, core::SymbolRef sym);

    static llvm::Value *emitMethodCall(CompilerState &cs, llvm::IRBuilderBase &builder, cfg::Send *send,
                                       const BasicBlockMap &blockMap, UnorderedMap<core::LocalVariable, Alias> &aliases,
                                       int rubyBlockId);

    static llvm::Value *emitMethodCallDirrect(CompilerState &cs, llvm::IRBuilderBase &builder, core::SymbolRef funSym,
                                              cfg::Send *send, const BasicBlockMap &blockMap,
                                              UnorderedMap<core::LocalVariable, Alias> &aliases, int rubyBlockId);

    static llvm::Value *emitMethodCallViaRubyVM(CompilerState &cs, llvm::IRBuilderBase &builder, cfg::Send *send,
                                                const BasicBlockMap &blockMap,
                                                UnorderedMap<core::LocalVariable, Alias> &aliases, int rubyBlockId);

    static BasicBlockMap getSorbetBlocks2LLVMBlockMapping(CompilerState &cs, cfg::CFG &cfg,
                                                          std::unique_ptr<ast::MethodDef> &md,
                                                          UnorderedMap<core::LocalVariable, Alias> &aliases,
                                                          llvm::Function *mainFunc);
};
} // namespace sorbet::compiler
#endif
