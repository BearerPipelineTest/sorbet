#ifndef SORBET_COMPILER_IREMITTER_IREMITTERHELPERS_H
#define SORBET_COMPILER_IREMITTER_IREMITTERHELPERS_H
#include "IREmitter.h"
#include "cfg/CFG.h"
#include "compiler/Core/ForwardDeclarations.h"
#include "core/core.h"
#include <string_view>
#include <vector>

namespace sorbet::compiler {

struct IREmitterContext;
struct MethodCallContext;

// TODO(jez) This shouldn't be at the top-level (sorbet::compiler). It should probably be nested in something.
// (Confusing to see bare `Alias` when there is also `cfg::Alias`)
struct Alias {
    enum class AliasKind { Constant, InstanceField, ClassField, GlobalField };
    AliasKind kind;
    core::SymbolRef constantSym;
    core::NameRef instanceField;
    core::NameRef classField;
    core::NameRef globalField;
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
    static Alias forGlobalField(core::NameRef name) {
        Alias ret;
        ret.kind = AliasKind::GlobalField;
        ret.globalField = name;
        return ret;
    }
};

class Intrinsics {
public:
    enum class HandleBlock : u1 {
        Handled = 1,
        Unhandled = 2,
    };
};

class IREmitterHelpers {
public:
    static bool isFileOrClassStaticInit(const core::GlobalState &gs, core::SymbolRef sym);

    // Returns a core::Loc whose start and end positions containt the bounds of the method sym.
    static core::Loc getMethodLineBounds(const core::GlobalState &gs, core::SymbolRef sym, core::FileRef file,
                                         core::LocOffsets offsets);

    // Returns a core::Loc whose begin pos contains the start line of the method.
    static core::Loc getMethodStart(const core::GlobalState &gs, core::SymbolRef sym) {
        auto loc = sym.data(gs)->loc();
        return getMethodLineBounds(gs, sym, loc.file(), loc.offsets());
    }

    static llvm::GlobalVariable *getStaticInitLocalsOffset(CompilerState &cs, core::SymbolRef sym);

    static std::string getFunctionName(CompilerState &cs, core::SymbolRef sym);
    static llvm::Function *lookupFunction(CompilerState &cs, core::SymbolRef sym);
    static llvm::Function *getOrCreateFunctionWeak(CompilerState &cs, core::SymbolRef sym);
    static llvm::Function *cleanFunctionBody(CompilerState &cs, llvm::Function *func);
    static llvm::Function *getOrCreateStaticInit(CompilerState &cs, core::SymbolRef sym, core::LocOffsets loc);
    static llvm::Function *getOrCreateFunction(CompilerState &cs, core::SymbolRef sym);

    static llvm::Function *getInitFunction(CompilerState &cs, core::SymbolRef sym);

    static std::size_t sendArgCount(cfg::Send *send);

    struct SendArgInfo {
        SendArgInfo(llvm::Value *argc, llvm::Value *argv, llvm::Value *kw_splat);

        llvm::Value *argc;
        llvm::Value *argv;
        llvm::Value *kw_splat;
    };

    static SendArgInfo fillSendArgArray(MethodCallContext &mcctx, const std::size_t offset);
    static SendArgInfo fillSendArgArray(MethodCallContext &mcctx);

    static llvm::Value *emitMethodCall(MethodCallContext &mcctx);

    static llvm::Value *makeInlineCache(CompilerState &cs, std::string slowFunName);

    static llvm::Value *callViaRubyVMSimple(CompilerState &cs, llvm::IRBuilderBase &build,
                                            const IREmitterContext &irctx, llvm::Value *self, llvm::Value *argv,
                                            llvm::Value *argc, llvm::Value *kw_splat, std::string_view name,
                                            llvm::Function *blkFun = nullptr, llvm::Value *localsOffset = nullptr);

    static llvm::Value *emitMethodCallViaRubyVM(MethodCallContext &mcctx);

    static IREmitterContext getSorbetBlocks2LLVMBlockMapping(CompilerState &cs, cfg::CFG &cfg, const ast::MethodDef &md,
                                                             llvm::Function *mainFunc);

    static void emitExceptionHandlers(CompilerState &gs, llvm::IRBuilderBase &builder, const IREmitterContext &irctx,
                                      int rubyBlockId, int bodyRubyBlockId, cfg::LocalRef exceptionValue);

    static void emitDebugLoc(CompilerState &gs, llvm::IRBuilderBase &builder, const IREmitterContext &irctx,
                             int rubyBlockId, core::Loc loc);

    static void emitReturn(CompilerState &gs, llvm::IRBuilderBase &builder, const IREmitterContext &irctx,
                           int rubyBlockId, llvm::Value *retVal);

    // Emit a type test.  The insertion point of the builder is set to the start of
    // the block following a successful test.
    static void emitTypeTest(CompilerState &gs, llvm::IRBuilderBase &builder, llvm::Value *value,
                             const core::TypePtr &expectedType, std::string_view description);

    // Return a value representing the literalish thing, which is either a LiteralType
    // or a type representing nil, false, or true.
    static llvm::Value *emitLiteralish(CompilerState &gs, llvm::IRBuilderBase &builder,
                                       const core::TypePtr &literalish);

    // Return true if the given blockId has a block argument.
    static bool hasBlockArgument(CompilerState &gs, int blockId, core::SymbolRef method, const IREmitterContext &irctx);

    // Given an owner as the Sorbet-visible symbol, return the parent symbol
    // as seen by the Ruby VM.
    static core::SymbolRef fixupOwningSymbol(const core::GlobalState &gs, core::SymbolRef sym);

    static std::string showClassNameWithoutOwner(const core::GlobalState &gs, core::SymbolRef sym);

    // Return true if the given symbol is a "root" symbol.
    static bool isRootishSymbol(const core::GlobalState &gs, core::SymbolRef sym);
};
} // namespace sorbet::compiler
#endif
