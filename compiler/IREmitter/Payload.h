#ifndef SORBET_COMPILER_PAYLOAD_H
#define SORBET_COMPILER_PAYLOAD_H

namespace sorbet::compiler {

struct BasicBlockMap;

// This class serves as forwarder to payload.c, which are the c wrappers for
// Ruby functions. These functions can (and do) use information known during
// compile time to dispatch to different c functions, but other than that, they
// should mostly be forwarders.
class Payload {
public:
    // api for actual code emission
    static llvm::Value *idIntern(CompilerState &cs, llvm::IRBuilderBase &builder, std::string_view idName);
    static llvm::Value *setExpectedBool(CompilerState &cs, llvm::IRBuilderBase &builder, llvm::Value *boolean,
                                        bool expected);
    // boxed raw value from rawData into target. Assumes that types are compatible.
    static void boxRawValue(CompilerState &cs, llvm::IRBuilderBase &builder, llvm::AllocaInst *storeTarget,
                            llvm::Value *rawData);
    static llvm::Value *unboxRawValue(CompilerState &cs, llvm::IRBuilderBase &builder, llvm::AllocaInst *storeTarget);

    static llvm::Value *rubyNil(CompilerState &cs, llvm::IRBuilderBase &builder);
    static llvm::Value *rubyFalse(CompilerState &cs, llvm::IRBuilderBase &builder);
    static llvm::Value *rubyTrue(CompilerState &cs, llvm::IRBuilderBase &builder);
    static void raiseArity(CompilerState &cs, llvm::IRBuilderBase &builder, llvm::Value *currentArgCount, int minArgs,
                           int maxArgs);
    static llvm::Value *longToRubyValue(CompilerState &cs, llvm::IRBuilderBase &builder, long num);
    static llvm::Value *doubleToRubyValue(CompilerState &cs, llvm::IRBuilderBase &builder, double num);
    static llvm::Value *cPtrToRubyString(CompilerState &cs, llvm::IRBuilderBase &builder, std::string_view str);
    static llvm::Value *testIsTruthy(CompilerState &cs, llvm::IRBuilderBase &builder, llvm::Value *val);
    static llvm::Value *getRubyConstant(CompilerState &cs, core::SymbolRef sym, llvm::IRBuilderBase &builder);
    static llvm::Value *toCString(CompilerState &cs, std::string_view str, llvm::IRBuilderBase &builder);
    static llvm::Value *typeTest(CompilerState &cs, llvm::IRBuilderBase &builder, llvm::Value *val,
                                 const core::TypePtr &type);
    static llvm::Value *setRubyStackFrame(CompilerState &cs, llvm::IRBuilderBase &builder,
                                          std::unique_ptr<ast::MethodDef> &md);
    static void setLineNumber(CompilerState &cs, llvm::IRBuilderBase &builder, core::Loc loc, core::SymbolRef sym);
    static llvm::Value *loadSelf(CompilerState &cs, llvm::IRBuilderBase &builder);

    static llvm::Value *varGet(CompilerState &cs, core::LocalVariable local, llvm::IRBuilderBase &builder,
                               const BasicBlockMap &blockMap, const UnorderedMap<core::LocalVariable, Alias> &aliases,
                               int rubyBlockId);
    static void varSet(CompilerState &cs, core::LocalVariable local, llvm::Value *var, llvm::IRBuilderBase &builder,
                       const BasicBlockMap &blockMap, UnorderedMap<core::LocalVariable, Alias> &aliases,
                       int rubyBlockId);
};
} // namespace sorbet::compiler
#endif
