// These violate our poisons so have to happen first
#include "llvm/IR/DerivedTypes.h" // FunctionType
#include "llvm/IR/IRBuilder.h"

#include "IREmitterHelpers.h"
#include "Payload.h"
#include "ast/Trees.h"
#include "common/sort.h"
#include "common/typecase.h"
#include "compiler/Core/CompilerState.h"
#include "compiler/IREmitter/IREmitterContext.h"
#include <string>

using namespace std;
namespace sorbet::compiler {
namespace {
llvm::IRBuilder<> &builderCast(llvm::IRBuilderBase &builder) {
    return static_cast<llvm::IRBuilder<> &>(builder);
};
} // namespace

llvm::Value *Payload::setExpectedBool(CompilerState &cs, llvm::IRBuilderBase &builder, llvm::Value *value,
                                      bool expected) {
    return builderCast(builder).CreateIntrinsic(llvm::Intrinsic::ID::expect, {llvm::Type::getInt1Ty(cs)},
                                                {value, builder.getInt1(expected)});
}

void Payload::boxRawValue(CompilerState &cs, llvm::IRBuilderBase &builder, llvm::AllocaInst *target,
                          llvm::Value *rawData) {
    builderCast(builder).CreateStore(rawData, builderCast(builder).CreateStructGEP(target, 0));
}

llvm::Value *Payload::unboxRawValue(CompilerState &cs, llvm::IRBuilderBase &builder, llvm::AllocaInst *target) {
    return builderCast(builder).CreateLoad(builderCast(builder).CreateStructGEP(target, 0), "rawRubyValue");
}

llvm::Value *Payload::rubyUndef(CompilerState &cs, llvm::IRBuilderBase &builder) {
    return builderCast(builder).CreateCall(cs.getFunction("sorbet_rubyUndef"), {}, "undefValueRaw");
}

llvm::Value *Payload::rubyNil(CompilerState &cs, llvm::IRBuilderBase &builder) {
    return builderCast(builder).CreateCall(cs.getFunction("sorbet_rubyNil"), {}, "nilValueRaw");
}

llvm::Value *Payload::rubyFalse(CompilerState &cs, llvm::IRBuilderBase &builder) {
    return builderCast(builder).CreateCall(cs.getFunction("sorbet_rubyFalse"), {}, "falseValueRaw");
}

llvm::Value *Payload::rubyTopSelf(CompilerState &cs, llvm::IRBuilderBase &builder) {
    return builderCast(builder).CreateCall(cs.getFunction("sorbet_rubyTopSelf"), {}, "topSelf");
}

llvm::Value *Payload::rubyTrue(CompilerState &cs, llvm::IRBuilderBase &builder) {
    return builderCast(builder).CreateCall(cs.getFunction("sorbet_rubyTrue"), {}, "trueValueRaw");
}

void Payload::raiseArity(CompilerState &cs, llvm::IRBuilderBase &builder, llvm::Value *currentArgCount, int minArgs,
                         int maxArgs) {
    builderCast(builder).CreateCall(cs.getFunction("sorbet_raiseArity"),
                                    {currentArgCount, llvm::ConstantInt::get(cs, llvm::APInt(32, minArgs, true)),
                                     llvm::ConstantInt::get(cs, llvm::APInt(32, maxArgs, true))

                                    });
    builderCast(builder).CreateUnreachable();
}
llvm::Value *Payload::longToRubyValue(CompilerState &cs, llvm::IRBuilderBase &builder, long num) {
    return builderCast(builder).CreateCall(cs.getFunction("sorbet_longToRubyValue"),
                                           {llvm::ConstantInt::get(cs, llvm::APInt(64, num, true))}, "rawRubyInt");
}

llvm::Value *Payload::doubleToRubyValue(CompilerState &cs, llvm::IRBuilderBase &builder, double num) {
    return builderCast(builder).CreateCall(cs.getFunction("sorbet_doubleToRubyValue"),
                                           {llvm::ConstantFP::get(llvm::Type::getDoubleTy(cs), num)}, "rawRubyDouble");
}

llvm::Value *Payload::cPtrToRubyRegexp(CompilerState &cs, llvm::IRBuilderBase &build, std::string_view str,
                                       int options) {
    auto &builder = builderCast(build);
    // all regexp are frozen. We'll allocate it at load time and share it.
    string rawName = "rubyRegexpFrozen_" + (string)str;
    auto tp = llvm::Type::getInt64Ty(cs);
    auto zero = llvm::ConstantInt::get(cs, llvm::APInt(64, 0));
    llvm::Constant *indices[] = {zero};

    auto globalDeclaration = static_cast<llvm::GlobalVariable *>(cs.module->getOrInsertGlobal(rawName, tp, [&] {
        llvm::IRBuilder<> globalInitBuilder(cs);
        auto ret =
            new llvm::GlobalVariable(*cs.module, tp, false, llvm::GlobalVariable::InternalLinkage, zero, rawName);
        ret->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        ret->setAlignment(8);
        // create constructor
        std::vector<llvm::Type *> NoArgs(0, llvm::Type::getVoidTy(cs));
        auto ft = llvm::FunctionType::get(llvm::Type::getVoidTy(cs), NoArgs, false);
        auto constr = llvm::Function::Create(ft, llvm::Function::InternalLinkage, {"Constr_", rawName}, *cs.module);

        auto bb = llvm::BasicBlock::Create(cs, "constr", constr);
        globalInitBuilder.SetInsertPoint(bb);
        auto rawCString = Payload::toCString(cs, str, globalInitBuilder);
        auto rawStr =
            globalInitBuilder.CreateCall(cs.getFunction("sorbet_cPtrToRubyRegexpFrozen"),
                                         {rawCString, llvm::ConstantInt::get(cs, llvm::APInt(64, str.length())),

                                          llvm::ConstantInt::get(cs, llvm::APInt(32, options))});
        globalInitBuilder.CreateStore(rawStr,
                                      llvm::ConstantExpr::getInBoundsGetElementPtr(ret->getValueType(), ret, indices));
        globalInitBuilder.CreateRetVoid();
        globalInitBuilder.SetInsertPoint(cs.globalConstructorsEntry);
        globalInitBuilder.CreateCall(constr, {});

        return ret;
    }));

    ENFORCE(cs.functionEntryInitializers->getParent() == builder.GetInsertBlock()->getParent(),
            "you're calling this function from something low-level that passed a IRBuilder that points outside of "
            "function currently being generated");
    auto oldInsertPoint = builder.saveIP();
    builder.SetInsertPoint(cs.functionEntryInitializers);
    auto name = llvm::StringRef(str.data(), str.length());
    auto global = builder.CreateLoad(
        llvm::ConstantExpr::getInBoundsGetElementPtr(globalDeclaration->getValueType(), globalDeclaration, indices),
        {"rubyRegexp_", name});
    builder.restoreIP(oldInsertPoint);

    // todo(perf): mark these as immutable with https://llvm.org/docs/LangRef.html#llvm-invariant-start-intrinsic
    return global;
}

llvm::Value *Payload::cPtrToRubyString(CompilerState &cs, llvm::IRBuilderBase &build, std::string_view str,
                                       bool frozen) {
    auto &builder = builderCast(build);
    if (!frozen) {
        auto rawCString = Payload::toCString(cs, str, builder);
        return builder.CreateCall(cs.getFunction("sorbet_cPtrToRubyString"),
                                  {rawCString, llvm::ConstantInt::get(cs, llvm::APInt(64, str.length(), true))},
                                  "rawRubyStr");
    }
    // this is a frozen string. We'll allocate it at load time and share it.
    string rawName = "rubyStrFrozen_" + (string)str;
    auto tp = llvm::Type::getInt64Ty(cs);
    auto zero = llvm::ConstantInt::get(cs, llvm::APInt(64, 0));
    llvm::Constant *indices[] = {zero};

    auto globalDeclaration = static_cast<llvm::GlobalVariable *>(cs.module->getOrInsertGlobal(rawName, tp, [&] {
        llvm::IRBuilder<> globalInitBuilder(cs);
        auto ret =
            new llvm::GlobalVariable(*cs.module, tp, false, llvm::GlobalVariable::InternalLinkage, zero, rawName);
        ret->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        ret->setAlignment(8);
        // create constructor
        std::vector<llvm::Type *> NoArgs(0, llvm::Type::getVoidTy(cs));
        auto ft = llvm::FunctionType::get(llvm::Type::getVoidTy(cs), NoArgs, false);
        auto constr = llvm::Function::Create(ft, llvm::Function::InternalLinkage, {"Constr_", rawName}, *cs.module);

        auto bb = llvm::BasicBlock::Create(cs, "constr", constr);
        globalInitBuilder.SetInsertPoint(bb);
        auto rawCString = Payload::toCString(cs, str, globalInitBuilder);
        auto rawStr =
            globalInitBuilder.CreateCall(cs.getFunction("sorbet_cPtrToRubyStringFrozen"),
                                         {rawCString, llvm::ConstantInt::get(cs, llvm::APInt(64, str.length()))});
        globalInitBuilder.CreateStore(rawStr,
                                      llvm::ConstantExpr::getInBoundsGetElementPtr(ret->getValueType(), ret, indices));
        globalInitBuilder.CreateRetVoid();
        globalInitBuilder.SetInsertPoint(cs.globalConstructorsEntry);
        globalInitBuilder.CreateCall(constr, {});

        return ret;
    }));

    auto name = llvm::StringRef(str.data(), str.length());
    auto global = builder.CreateLoad(
        llvm::ConstantExpr::getInBoundsGetElementPtr(globalDeclaration->getValueType(), globalDeclaration, indices),
        {"rubyStr_", name});

    // todo(perf): mark these as immutable with https://llvm.org/docs/LangRef.html#llvm-invariant-start-intrinsic
    return global;
}

llvm::Value *Payload::testIsUndef(CompilerState &cs, llvm::IRBuilderBase &builder, llvm::Value *val) {
    return builderCast(builder).CreateCall(cs.getFunction("sorbet_testIsUndef"), {val}, "isUndef");
}

llvm::Value *Payload::testIsTruthy(CompilerState &cs, llvm::IRBuilderBase &builder, llvm::Value *val) {
    return builderCast(builder).CreateCall(cs.getFunction("sorbet_testIsTruthy"), {val}, "cond");
}

llvm::Value *Payload::idIntern(CompilerState &cs, llvm::IRBuilderBase &build, std::string_view idName) {
    auto &builder = builderCast(build);
    auto zero = llvm::ConstantInt::get(cs, llvm::APInt(64, 0));
    auto name = llvm::StringRef(idName.data(), idName.length());
    llvm::Constant *indices[] = {zero};
    string rawName = "rubyIdPrecomputed_" + (string)idName;
    auto tp = llvm::Type::getInt64Ty(cs);
    auto globalDeclaration = static_cast<llvm::GlobalVariable *>(cs.module->getOrInsertGlobal(rawName, tp, [&] {
        llvm::IRBuilder<> globalInitBuilder(cs);
        auto ret =
            new llvm::GlobalVariable(*cs.module, tp, false, llvm::GlobalVariable::InternalLinkage, zero, rawName);
        ret->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        ret->setAlignment(8);
        // create constructor
        std::vector<llvm::Type *> NoArgs(0, llvm::Type::getVoidTy(cs));
        auto ft = llvm::FunctionType::get(llvm::Type::getVoidTy(cs), NoArgs, false);
        auto constr = llvm::Function::Create(ft, llvm::Function::InternalLinkage, {"Constr_", rawName}, *cs.module);

        auto bb = llvm::BasicBlock::Create(cs, "constr", constr);
        globalInitBuilder.SetInsertPoint(bb);
        auto rawCString = Payload::toCString(cs, idName, globalInitBuilder);
        auto rawID = globalInitBuilder.CreateCall(
            cs.getFunction("sorbet_idIntern"),
            {rawCString, llvm::ConstantInt::get(cs, llvm::APInt(64, idName.length()))}, "rawId");
        globalInitBuilder.CreateStore(rawID,
                                      llvm::ConstantExpr::getInBoundsGetElementPtr(ret->getValueType(), ret, indices));
        globalInitBuilder.CreateRetVoid();
        globalInitBuilder.SetInsertPoint(cs.allocRubyIdsEntry);
        globalInitBuilder.CreateCall(constr, {});

        return ret;
    }));

    auto global = builder.CreateLoad(
        llvm::ConstantExpr::getInBoundsGetElementPtr(globalDeclaration->getValueType(), globalDeclaration, indices),
        {"rubyId_", name});

    // todo(perf): mark these as immutable with https://llvm.org/docs/LangRef.html#llvm-invariant-start-intrinsic
    return global;
}

namespace {
std::string showClassName(const core::GlobalState &gs, core::SymbolRef sym) {
    auto owner = sym.data(gs)->owner;
    bool includeOwner = owner.exists() && owner != core::Symbols::root() && owner != core::Symbols::PackageRegistry() &&
                        owner.data(gs)->name != core::Names::Constants::PkgRoot_Package();
    string ownerStr = includeOwner ? showClassName(gs, owner) + "::" : "";
    return ownerStr + IREmitterHelpers::showClassNameWithoutOwner(gs, sym);
}

} // namespace

llvm::Value *Payload::getRubyConstant(CompilerState &cs, core::SymbolRef sym, llvm::IRBuilderBase &build) {
    ENFORCE(sym.data(cs)->isClassOrModule() || sym.data(cs)->isStaticField() || sym.data(cs)->isTypeMember());
    auto &builder = builderCast(build);
    sym = IREmitterHelpers::fixupOwningSymbol(cs, sym);
    auto str = showClassName(cs, sym);
    ENFORCE(str.length() < 2 || (str[0] != ':'), "implementation assumes that strings dont start with ::");
    auto functionName = sym.data(cs)->isClassOrModule() ? "sorbet_i_getRubyClass" : "sorbet_i_getRubyConstant";
    return builder.CreateCall(
        cs.getFunction(functionName),
        {Payload::toCString(cs, str, builder), llvm::ConstantInt::get(cs, llvm::APInt(64, str.length()))});
}

llvm::Value *Payload::toCString(CompilerState &cs, string_view str, llvm::IRBuilderBase &builder) {
    llvm::StringRef valueRef(str.data(), str.length());
    auto globalName = "addr_str_" + (string)str;
    auto globalDeclaration =
        static_cast<llvm::GlobalVariable *>(cs.module->getOrInsertGlobal(globalName, builder.getInt8PtrTy(), [&] {
            auto valueGlobal = builder.CreateGlobalString(valueRef, llvm::Twine("str_") + valueRef);
            auto zero = llvm::ConstantInt::get(cs, llvm::APInt(64, 0));
            llvm::Constant *indicesString[] = {zero, zero};
            auto addrGlobalInitializer =
                llvm::ConstantExpr::getInBoundsGetElementPtr(valueGlobal->getValueType(), valueGlobal, indicesString);
            auto addrGlobal =
                new llvm::GlobalVariable(*cs.module, builder.getInt8PtrTy(), true,
                                         llvm::GlobalVariable::InternalLinkage, addrGlobalInitializer, globalName);
            addrGlobal->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
            addrGlobal->setAlignment(8);

            return addrGlobal;
        }));

    return builderCast(builder).CreateLoad(globalDeclaration);
}

namespace {
const vector<pair<core::ClassOrModuleRef, string>> optimizedTypeTests = {
    {core::Symbols::untyped(), "sorbet_isa_Untyped"},
    {core::Symbols::Array(), "sorbet_isa_Array"},
    {core::Symbols::FalseClass(), "sorbet_isa_FalseClass"},
    {core::Symbols::TrueClass(), "sorbet_isa_TrueClass"},
    {core::Symbols::Float(), "sorbet_isa_Float"},
    {core::Symbols::Hash(), "sorbet_isa_Hash"},
    {core::Symbols::Integer(), "sorbet_isa_Integer"},
    {core::Symbols::NilClass(), "sorbet_isa_NilClass"},
    {core::Symbols::Proc(), "sorbet_isa_Proc"},
    {core::Symbols::Rational(), "sorbet_isa_Rational"},
    {core::Symbols::Regexp(), "sorbet_isa_Regexp"},
    {core::Symbols::String(), "sorbet_isa_String"},
    {core::Symbols::Symbol(), "sorbet_isa_Symbol"},
    {core::Symbols::Proc(), "sorbet_isa_Proc"},
    {core::Symbols::rootSingleton(), "sorbet_isa_RootSingleton"},
};
}

static bool isProc(core::SymbolRef sym) {
    if (sym.kind() != core::SymbolRef::Kind::ClassOrModule) {
        return false;
    }
    auto id = sym.classOrModuleIndex();
    return id >= core::Symbols::Proc0().id() && id <= core::Symbols::last_proc().id();
}

llvm::Value *Payload::typeTest(CompilerState &cs, llvm::IRBuilderBase &b, llvm::Value *val, const core::TypePtr &type) {
    auto &builder = builderCast(b);
    llvm::Value *ret = nullptr;
    typecase(
        type,
        [&](const core::ClassType &ct) {
            for (const auto &[candidate, specializedCall] : optimizedTypeTests) {
                if (ct.symbol == candidate) {
                    ret = builder.CreateCall(cs.getFunction(specializedCall), {val});
                    return;
                }
            }

            if (ct.symbol.data(cs)->name.isTEnumName(cs)) {
                // T.let(..., MyEnum::X$1) is special. These are singleton values, so we can do a type
                // test with an object (reference) equality check.
                ret = builder.CreateCall(cs.getFunction("sorbet_testObjectEqual_p"),
                                         {Payload::getRubyConstant(cs, ct.symbol, builder), val});
                return;
            }

            auto attachedClass = ct.symbol.data(cs)->attachedClass(cs);
            // todo: handle attached of attached class
            if (attachedClass.exists()) {
                ret = builder.CreateCall(cs.getFunction("sorbet_isa_class_of"),
                                         {val, Payload::getRubyConstant(cs, attachedClass, builder)});
                return;
            }
            auto sym = isProc(ct.symbol) ? core::Symbols::Proc() : ct.symbol;
            ret = builder.CreateCall(cs.getFunction("sorbet_isa"), {val, Payload::getRubyConstant(cs, sym, builder)});
        },
        [&](const core::AppliedType &at) {
            core::ClassOrModuleRef klass = at.klass;
            auto base = typeTest(cs, builder, val, core::make_type<core::ClassType>(klass));
            ret = base;
            // todo: ranges, hashes, sets, enumerator, and, overall, enumerables
        },
        [&](const core::OrType &ct) {
            // TODO: reoder types so that cheap test is done first
            auto left = typeTest(cs, builder, val, ct.left);
            auto rightBlockStart = llvm::BasicBlock::Create(cs, "orRight", builder.GetInsertBlock()->getParent());
            auto contBlock = llvm::BasicBlock::Create(cs, "orContinue", builder.GetInsertBlock()->getParent());
            auto leftEnd = builder.GetInsertBlock();
            builder.CreateCondBr(left, contBlock, rightBlockStart);
            builder.SetInsertPoint(rightBlockStart);
            auto right = typeTest(cs, builder, val, ct.right);
            auto rightEnd = builder.GetInsertBlock();
            builder.CreateBr(contBlock);
            builder.SetInsertPoint(contBlock);
            auto phi = builder.CreatePHI(builder.getInt1Ty(), 2, "orTypeTest");
            phi->addIncoming(left, leftEnd);
            phi->addIncoming(right, rightEnd);
            ret = phi;
        },
        [&](const core::AndType &ct) {
            // TODO: reoder types so that cheap test is done first
            auto left = typeTest(cs, builder, val, ct.left);
            auto rightBlockStart = llvm::BasicBlock::Create(cs, "andRight", builder.GetInsertBlock()->getParent());
            auto contBlock = llvm::BasicBlock::Create(cs, "andContinue", builder.GetInsertBlock()->getParent());
            auto leftEnd = builder.GetInsertBlock();
            builder.CreateCondBr(left, rightBlockStart, contBlock);
            builder.SetInsertPoint(rightBlockStart);
            auto right = typeTest(cs, builder, val, ct.right);
            auto rightEnd = builder.GetInsertBlock();
            builder.CreateBr(contBlock);
            builder.SetInsertPoint(contBlock);
            auto phi = builder.CreatePHI(builder.getInt1Ty(), 2, "andTypeTest");
            phi->addIncoming(left, leftEnd);
            phi->addIncoming(right, rightEnd);
            ret = phi;
        },
        [&](const core::TypePtr &_default) { ret = builder.getInt1(true); });
    ENFORCE(ret != nullptr);
    return ret;
}

llvm::Value *Payload::boolToRuby(CompilerState &cs, llvm::IRBuilderBase &builder, llvm::Value *u1) {
    return builderCast(builder).CreateCall(cs.getFunction("sorbet_boolToRuby"), {u1}, "rubyBool");
}

namespace {

llvm::Value *allocateRubyStackFrames(CompilerState &cs, llvm::IRBuilderBase &build, const IREmitterContext &irctx,
                                     const ast::MethodDef &md, int rubyBlockId);

llvm::Value *getIseqType(CompilerState &cs, llvm::IRBuilderBase &build, const IREmitterContext &irctx,
                         int rubyBlockId) {
    auto &builder = builderCast(build);
    switch (irctx.rubyBlockType[rubyBlockId]) {
        case FunctionType::Method:
        case FunctionType::StaticInit:
            return builder.CreateCall(cs.getFunction("sorbet_rubyIseqTypeMethod"), {}, "ISEQ_TYPE_METHOD");

        case FunctionType::Block:
            return builder.CreateCall(cs.getFunction("sorbet_rubyIseqTypeBlock"), {}, "ISEQ_TYPE_BLOCK");

        case FunctionType::Rescue:
            return builder.CreateCall(cs.getFunction("sorbet_rubyIseqTypeRescue"), {}, "ISEQ_TYPE_RESCUE");

        case FunctionType::Ensure:
            return builder.CreateCall(cs.getFunction("sorbet_rubyIseqTypeEnsure"), {}, "ISEQ_TYPE_ENSURE");

        case FunctionType::ExceptionBegin:
            // Exception body functions inherit the iseq entry for their containing context, so we should never be
            // generating an iseq entry for them.
            Exception::raise("Allocating an iseq for a FunctionType::ExceptionBegin function");
            break;

        case FunctionType::Unused:
            // This should never happen, as we should be skipping iseq initialization for unused functions.
            Exception::raise("Picking an ISEQ_TYPE for an unused function!");
            break;
    }
}

std::tuple<string_view, llvm::Value *> getIseqInfo(CompilerState &cs, llvm::IRBuilderBase &build,
                                                   const IREmitterContext &irctx, const ast::MethodDef &md,
                                                   int rubyBlockId) {
    string_view funcName;
    llvm::Value *parent = nullptr;
    switch (irctx.rubyBlockType[rubyBlockId]) {
        case FunctionType::Method:
        case FunctionType::StaticInit: {
            if (IREmitterHelpers::isFileOrClassStaticInit(cs, md.symbol)) {
                funcName = "<top (required)>";
            } else {
                funcName = md.symbol.data(cs)->name.shortName(cs);
            }

            parent = llvm::Constant::getNullValue(llvm::Type::getInt8PtrTy(cs));
            break;
        }

        case FunctionType::Block:
            funcName = "block for"sv;
            parent = allocateRubyStackFrames(cs, build, irctx, md, irctx.rubyBlockParent[rubyBlockId]);
            break;

        case FunctionType::Rescue:
            funcName = "rescue for"sv;
            parent = allocateRubyStackFrames(cs, build, irctx, md, irctx.rubyBlockParent[rubyBlockId]);
            break;

        case FunctionType::Ensure:
            funcName = "ensure for"sv;
            parent = allocateRubyStackFrames(cs, build, irctx, md, irctx.rubyBlockParent[rubyBlockId]);
            break;

        case FunctionType::ExceptionBegin:
            // Exception body functions inherit the iseq entry for their containing context, so we should never be
            // generating an iseq entry for them.
            Exception::raise("Allocating an iseq for a FunctionType::ExceptionBegin function");
            break;

        case FunctionType::Unused:
            // This should never happen, as we should be skipping iseq initialization for unused functions.
            Exception::raise("Picking an ISEQ_TYPE for an unused function!");
            break;
    }

    return {funcName, parent};
}

bool allocatesLocals(CompilerState &cs, const IREmitterContext &irctx, core::SymbolRef method, int rubyBlockId) {
    switch (irctx.rubyBlockType[rubyBlockId]) {
        case FunctionType::Method:
        case FunctionType::StaticInit:
            return true;

        case FunctionType::Block:
        case FunctionType::Rescue:
        case FunctionType::Ensure:
        case FunctionType::ExceptionBegin:
        case FunctionType::Unused:
            return false;
    }
}

// Fill the locals array with interned ruby IDs.
void fillLocals(CompilerState &cs, llvm::IRBuilderBase &build, const IREmitterContext &irctx, int rubyBlockId,
                int baseOffset, llvm::Value *locals) {
    auto &builder = builderCast(build);

    // The map used to store escaped variables isn't stable, so we first sort it into a vector. This isn't great, but
    // without this step the locals are processed in random order, making the llvm output unstable.
    vector<pair<cfg::LocalRef, int>> escapedVariables{};
    for (auto &entry : irctx.escapedVariableIndices) {
        escapedVariables.emplace_back(entry);
    }

    fast_sort(escapedVariables, [](const auto &left, const auto &right) -> bool { return left.second < right.second; });

    for (auto &entry : escapedVariables) {
        auto *id = Payload::idIntern(cs, builder, entry.first.data(irctx.cfg)._name.shortName(cs));
        auto *offset = llvm::ConstantInt::get(cs, llvm::APInt(32, baseOffset + entry.second, false));
        llvm::Value *indices[] = {offset};
        builder.CreateStore(id, builder.CreateGEP(locals, indices));
    }
}

// Fetches the global that points to the current static-init locals array, and the number of elements allocated.
tuple<llvm::GlobalVariable *, llvm::GlobalVariable *, int> getStaticInitLocals(CompilerState &cs) {
    auto *staticInitLocalsName = "<static-init-locals>";
    auto *staticInitLocalsPtr = cs.module->getGlobalVariable(staticInitLocalsName, true);

    auto *staticInitLocalsSizeName = "<static-init-locals-size>";
    auto *staticInitLocalsSizePtr = cs.module->getGlobalVariable(staticInitLocalsSizeName, true);

    if (staticInitLocalsPtr != nullptr) {
        ENFORCE(staticInitLocalsSizePtr && staticInitLocalsSizePtr->hasInitializer());

        auto *init = staticInitLocalsSizePtr->getInitializer();
        ENFORCE(llvm::isa<llvm::ConstantInt>(init));

        return {staticInitLocalsPtr, staticInitLocalsSizePtr, llvm::cast<llvm::ConstantInt>(init)->getZExtValue()};
    } else {
        ENFORCE(staticInitLocalsSizePtr == nullptr);

        // the variable doesn't exist, so there haven't been any locals allocated for static-init.
        {
            auto *type = llvm::PointerType::getUnqual(llvm::Type::getInt64Ty(cs));
            auto nullv = llvm::ConstantPointerNull::get(type);
            staticInitLocalsPtr = new llvm::GlobalVariable(
                *cs.module, type, false, llvm::GlobalVariable::InternalLinkage, nullv, staticInitLocalsName);
        }

        {
            auto *type = llvm::Type::getInt32Ty(cs);
            auto *zero = llvm::ConstantInt::get(cs, llvm::APInt(32, 0, false));
            staticInitLocalsSizePtr = new llvm::GlobalVariable(
                *cs.module, type, false, llvm::GlobalVariable::InternalLinkage, zero, staticInitLocalsSizeName);
        }

        // Both globals do not change at runtime -- they only have their initializers changed during code generation.
        staticInitLocalsPtr->setConstant(true);
        staticInitLocalsSizePtr->setConstant(true);

        return {staticInitLocalsPtr, staticInitLocalsSizePtr, 0};
    }
}

// Allocate an array to hold local variable ids before calling `sorbet_allocateRubyStackFrame`. There are three cases
// that this addresses:
//
// (1) Normal methods:
//     Allocate an array on the C stack (of the method we're emitting) that is large enough to contain all of the
//     escaped locals, and place their ids inside of it
//
// (2) static-init methods:
//     Re-focus on the file-level static-init method, extending its array of locals to include the ones required for
//     this specific static-init method.
//
// (3) Blocks and exception-related functions:
//     All locals are inherited from the containing method, so none need to be allocated in the iseq
tuple<llvm::Value *, llvm::Value *> getLocals(CompilerState &cs, llvm::IRBuilderBase &build,
                                              const IREmitterContext &irctx, const ast::MethodDef &md,
                                              int rubyBlockId) {
    auto &builder = builderCast(build);
    llvm::Value *locals = nullptr;
    llvm::Value *numLocals = nullptr;
    auto *idType = llvm::Type::getInt64Ty(cs);
    auto *idPtrType = llvm::PointerType::getUnqual(idType);

    if (allocatesLocals(cs, irctx, md.symbol, rubyBlockId)) {
        if (!IREmitterHelpers::isFileOrClassStaticInit(cs, md.symbol)) {
            // case 1
            numLocals = llvm::ConstantInt::get(cs, llvm::APInt(32, irctx.escapedVariableIndices.size(), true));
            locals = builder.CreateAlloca(idType, numLocals, "locals");
            fillLocals(cs, builder, irctx, rubyBlockId, 0, locals);
        } else {
            // case 2

            // fetch the global that holds the pointer to locals arrays, and the current size.
            auto [staticInitLocalsPtr, staticInitLocalsSizePtr, baseSize] = getStaticInitLocals(cs);

            // store the offset to the locals for this static-init method in a fresh global with a special name
            auto *offsetGlobal = IREmitterHelpers::getStaticInitLocalsOffset(cs, md.symbol);
            offsetGlobal->setInitializer(llvm::ConstantInt::get(cs, llvm::APInt(64, baseSize, false)));

            // NOTE: we always allocate a new global to hold the resized array so that the initializer for the
            // "<static-init-locals>" global is always a pointer to another global that holds an array.
            auto escaped = irctx.escapedVariableIndices.size();
            auto newSize = baseSize + escaped;
            auto *arrayTy = llvm::ArrayType::get(llvm::Type::getInt64Ty(cs), newSize);
            auto *arrayGlobal =
                new llvm::GlobalVariable(*cs.module, arrayTy, false, llvm::GlobalVariable::InternalLinkage,
                                         llvm::ConstantAggregateZero::get(arrayTy), "<static-init-locals-storage>");

            auto *ptrTy = staticInitLocalsPtr->getType()->getElementType();
            staticInitLocalsPtr->setInitializer(llvm::ConstantExpr::getPointerCast(arrayGlobal, ptrTy));

            // update the global that keeps the number of locals allocated
            staticInitLocalsSizePtr->setInitializer(llvm::ConstantInt::get(cs, llvm::APInt(32, newSize, false)));

            {
                // fill the locals in the context of the auxiliary static-init locals setup function
                auto ip = builder.saveIP();
                builder.SetInsertPoint(cs.initializeStaticInitNamesEntry);
                auto cs1 = cs.withFunctionEntry(cs.initializeStaticInitNamesEntry);
                fillLocals(cs1, builder, irctx, rubyBlockId, baseSize,
                           builder.CreateLoad(staticInitLocalsPtr, "locals"));
                builder.restoreIP(ip);
            }

            // Finally, we only set the locals pointer to a non-null value if this is the top-level static-init, as
            // that is the function that will be responsible for allocating the frame used by all static-init methods.
            // (The top-level static-init method has a name like `<static-init>$123>` not just `<static-init>`, which is
            // why this check works)
            if (md.symbol.data(cs)->name == core::Names::staticInit()) {
                numLocals = llvm::ConstantInt::get(cs, llvm::APInt(32, 0, true));
                locals = llvm::ConstantPointerNull::get(idPtrType);
            } else {
                numLocals = builder.CreateLoad(staticInitLocalsSizePtr, "numLocals");
                locals = builder.CreateLoad(staticInitLocalsPtr, "locals");
            }
        }
    } else {
        // case 3
        numLocals = llvm::ConstantInt::get(cs, llvm::APInt(32, 0, true));
        locals = llvm::ConstantPointerNull::get(idPtrType);
    }

    return {locals, numLocals};
}

llvm::Function *allocateRubyStackFramesImpl(CompilerState &cs, const IREmitterContext &irctx, const ast::MethodDef &md,
                                            int rubyBlockId, llvm::GlobalVariable *store) {
    std::vector<llvm::Type *> argTys{llvm::Type::getInt64Ty(cs)};
    auto ft = llvm::FunctionType::get(llvm::Type::getVoidTy(cs), argTys, false);
    auto constr =
        llvm::Function::Create(ft, llvm::Function::InternalLinkage, {"Constr_", store->getName()}, *cs.module);

    auto realpath = constr->arg_begin();
    realpath->setName("realpath");

    auto bei = llvm::BasicBlock::Create(cs, "entryInitializers", constr);
    auto bb = llvm::BasicBlock::Create(cs, "constr", constr);

    llvm::IRBuilder<> builder(cs);
    builder.SetInsertPoint(bb);

    // We are building a new function. We should redefine where do function initializers go
    auto cs1 = cs.withFunctionEntry(bei);

    auto loc = IREmitterHelpers::getMethodLineBounds(cs, md.symbol, cs.file, md.loc);
    auto *iseqType = getIseqType(cs1, builder, irctx, rubyBlockId);
    auto [funcName, parent] = getIseqInfo(cs1, builder, irctx, md, rubyBlockId);
    auto funcNameId = Payload::idIntern(cs1, builder, funcName);
    auto funcNameValue = Payload::cPtrToRubyString(cs1, builder, funcName, true);
    auto filename = loc.file().data(cs).path();
    auto filenameValue = Payload::cPtrToRubyString(cs1, builder, filename, true);
    // The method might have been synthesized by Sorbet (e.g. in the case of packages).
    // Give such methods line numbers of 0.
    unsigned startLine, endLine;
    if (loc.exists()) {
        startLine = loc.position(cs).first.line;
        endLine = loc.position(cs).second.line;
    } else {
        startLine = 0;
        endLine = 0;
    }
    auto [locals, numLocals] = getLocals(cs1, builder, irctx, md, rubyBlockId);
    auto ret = builder.CreateCall(cs.getFunction("sorbet_allocateRubyStackFrame"),
                                  {funcNameValue, funcNameId, filenameValue, realpath, parent, iseqType,
                                   llvm::ConstantInt::get(cs, llvm::APInt(32, startLine)),
                                   llvm::ConstantInt::get(cs, llvm::APInt(32, endLine)), locals, numLocals});
    auto zero = llvm::ConstantInt::get(cs, llvm::APInt(64, 0));
    llvm::Constant *indices[] = {zero};
    builder.CreateStore(ret, llvm::ConstantExpr::getInBoundsGetElementPtr(store->getValueType(), store, indices));
    builder.CreateRetVoid();
    builder.SetInsertPoint(bei);
    builder.CreateBr(bb);
    return constr;
}

// The common suffix for stack frame related global names.
string getStackFrameGlobalName(CompilerState &cs, const IREmitterContext &irctx, const ast::MethodDef &md,
                               int rubyBlockId) {
    auto name = IREmitterHelpers::getFunctionName(cs, md.symbol);

    switch (irctx.rubyBlockType[rubyBlockId]) {
        case FunctionType::Method:
        case FunctionType::StaticInit:
            return name;

        case FunctionType::Block:
        case FunctionType::Rescue:
        case FunctionType::Ensure:
        case FunctionType::ExceptionBegin:
        case FunctionType::Unused:
            return name + "$block_" + std::to_string(rubyBlockId);
    }
}

llvm::Value *allocateRubyStackFrames(CompilerState &cs, llvm::IRBuilderBase &build, const IREmitterContext &irctx,
                                     const ast::MethodDef &md, int rubyBlockId) {
    auto tp = llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(cs));
    auto zero = llvm::ConstantInt::get(cs, llvm::APInt(64, 0));
    auto name = getStackFrameGlobalName(cs, irctx, md, rubyBlockId);
    llvm::Constant *indices[] = {zero};
    string rawName = "stackFramePrecomputed_" + name;
    llvm::IRBuilder<> globalInitBuilder(cs);
    auto globalDeclaration = static_cast<llvm::GlobalVariable *>(cs.module->getOrInsertGlobal(rawName, tp, [&] {
        auto nullv = llvm::ConstantPointerNull::get(tp);
        auto ret =
            new llvm::GlobalVariable(*cs.module, tp, false, llvm::GlobalVariable::InternalLinkage, nullv, rawName);
        ret->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        ret->setAlignment(8);

        // The realpath is the first argument to `sorbet_globalConstructors`
        auto realpath = cs.globalConstructorsEntry->getParent()->arg_begin();
        realpath->setName("realpath");

        // create constructor
        auto constr = allocateRubyStackFramesImpl(cs, irctx, md, rubyBlockId, ret);
        globalInitBuilder.SetInsertPoint(cs.globalConstructorsEntry);
        globalInitBuilder.CreateCall(constr, {realpath});

        return ret;
    }));

    globalInitBuilder.SetInsertPoint(cs.functionEntryInitializers);
    auto global = globalInitBuilder.CreateLoad(
        llvm::ConstantExpr::getInBoundsGetElementPtr(globalDeclaration->getValueType(), globalDeclaration, indices),
        {"stackFrame_", name});

    // todo(perf): mark these as immutable with https://llvm.org/docs/LangRef.html#llvm-invariant-start-intrinsic
    return global;
}

} // namespace

std::pair<llvm::Value *, llvm::Value *> Payload::setRubyStackFrame(CompilerState &cs, llvm::IRBuilderBase &build,
                                                                   const IREmitterContext &irctx,
                                                                   const ast::MethodDef &md, int rubyBlockId) {
    auto &builder = builderCast(build);
    auto stackFrame = allocateRubyStackFrames(cs, builder, irctx, md, rubyBlockId);
    auto *iseqType = getIseqType(cs, builder, irctx, rubyBlockId);
    auto *isClassOrModuleStaticInit =
        llvm::ConstantInt::get(cs, llvm::APInt(1, irctx.rubyBlockType[rubyBlockId] == FunctionType::StaticInit));
    auto cfp = builder.CreateCall(cs.getFunction("sorbet_setRubyStackFrame"),
                                  {isClassOrModuleStaticInit, iseqType, stackFrame});
    auto pc = builder.CreateCall(cs.getFunction("sorbet_getPc"), {cfp});
    auto iseq_encoded = builder.CreateCall(cs.getFunction("sorbet_getIseqEncoded"), {cfp});
    return {pc, iseq_encoded};
}

// Ensure that the retry singleton is present during module initialization, and store it in a module-local global.
llvm::Value *Payload::retrySingleton(CompilerState &cs, llvm::IRBuilderBase &build, const IREmitterContext &irctx) {
    auto tp = llvm::Type::getInt64Ty(cs);
    string rawName = "<retry-singleton>";
    auto *global = cs.module->getOrInsertGlobal(rawName, tp, [&] {
        auto globalInitBuilder = llvm::IRBuilder<>(cs);

        auto isConstant = false;
        auto zero = llvm::ConstantInt::get(cs, llvm::APInt(64, 0, true));
        auto global =
            new llvm::GlobalVariable(*cs.module, tp, isConstant, llvm::GlobalVariable::InternalLinkage, zero, rawName);

        globalInitBuilder.SetInsertPoint(cs.globalConstructorsEntry);
        auto *singletonValue = globalInitBuilder.CreateCall(cs.getFunction("sorbet_getTRetry"), {}, "retrySingleton");

        globalInitBuilder.CreateStore(singletonValue, global);

        return global;
    });

    return builderCast(build).CreateLoad(global, rawName);
}

core::Loc Payload::setLineNumber(CompilerState &cs, llvm::IRBuilderBase &build, core::Loc loc, core::Loc methodStart,
                                 core::Loc lastLoc, llvm::AllocaInst *iseqEncodedPtr, llvm::AllocaInst *lineNumberPtr) {
    if (!loc.exists()) {
        return lastLoc;
    }
    auto &builder = builderCast(build);
    auto lineno = loc.position(cs).first.line;
    if (lastLoc.exists() && lastLoc.position(cs).first.line == lineno) {
        return lastLoc;
    }
    if (!methodStart.exists()) {
        return lastLoc;
    }
    auto offset = lineno - methodStart.position(cs).first.line;
    builder.CreateCall(cs.getFunction("sorbet_setLineNumber"),
                       {llvm::ConstantInt::get(cs, llvm::APInt(32, offset)), builder.CreateLoad(iseqEncodedPtr),
                        builder.CreateLoad(lineNumberPtr)});
    return loc;
}

llvm::Value *Payload::readKWRestArg(CompilerState &cs, llvm::IRBuilderBase &build, llvm::Value *maybeHash) {
    auto &builder = builderCast(build);
    return builder.CreateCall(cs.getFunction("sorbet_readKWRestArgs"), {maybeHash});
}

llvm::Value *Payload::assertNoExtraKWArg(CompilerState &cs, llvm::IRBuilderBase &build, llvm::Value *maybeHash) {
    auto &builder = builderCast(build);
    return builder.CreateCall(cs.getFunction("sorbet_assertNoExtraKWArg"), {maybeHash});
}

llvm::Value *Payload::getKWArg(CompilerState &cs, llvm::IRBuilderBase &build, llvm::Value *maybeHash,
                               llvm::Value *rubySym) {
    auto &builder = builderCast(build);
    return builder.CreateCall(cs.getFunction("sorbet_getKWArg"), {maybeHash, rubySym});
}

llvm::Value *Payload::readRestArgs(CompilerState &cs, llvm::IRBuilderBase &build, int maxPositionalArgCount,
                                   llvm::Value *argCountRaw, llvm::Value *argArrayRaw) {
    auto &builder = builderCast(build);
    return builder.CreateCall(
        cs.getFunction("sorbet_readRestArgs"),
        {llvm::ConstantInt::get(cs, llvm::APInt(32, maxPositionalArgCount)), argCountRaw, argArrayRaw});
}

namespace {
llvm::Value *getClassVariableStoreClass(CompilerState &cs, llvm::IRBuilder<> &builder, const IREmitterContext &irctx) {
    auto sym = irctx.cfg.symbol.data(cs)->owner;
    ENFORCE(sym.data(cs)->isClassOrModule());

    return Payload::getRubyConstant(cs, sym.data(cs)->topAttachedClass(cs), builder);
};

// For a variable that's escaped, compute its index into the locals from its unique id in the
// closure.
tuple<llvm::Value *, llvm::Value *> indexForLocalVariable(CompilerState &cs, const IREmitterContext &irctx,
                                                          int rubyBlockId, int escapeId) {
    llvm::Value *offset = nullptr;
    auto *index = llvm::ConstantInt::get(cs, llvm::APInt(64, escapeId, true));

    if (irctx.useLocalsOffset) {
        offset = irctx.localsOffset[rubyBlockId];
    } else {
        offset = llvm::ConstantInt::get(cs, llvm::APInt(64, 0, true));
    }

    return {offset, index};
}

} // namespace

llvm::Value *Payload::varGet(CompilerState &cs, cfg::LocalRef local, llvm::IRBuilderBase &build,
                             const IREmitterContext &irctx, int rubyBlockId) {
    auto &builder = builderCast(build);
    if (irctx.aliases.contains(local)) {
        // alias to a field or constant
        auto alias = irctx.aliases.at(local);

        switch (alias.kind) {
            case Alias::AliasKind::Constant:
                return Payload::getRubyConstant(cs, alias.constantSym, builder);
            case Alias::AliasKind::GlobalField:
                return builder.CreateCall(cs.getFunction("sorbet_globalVariableGet"),
                                          {Payload::idIntern(cs, builder, alias.globalField.shortName(cs))});
            case Alias::AliasKind::ClassField:
                return builder.CreateCall(cs.getFunction("sorbet_classVariableGet"),
                                          {getClassVariableStoreClass(cs, builder, irctx),
                                           Payload::idIntern(cs, builder, alias.classField.shortName(cs))});
            case Alias::AliasKind::InstanceField:
                return builder.CreateCall(cs.getFunction("sorbet_instanceVariableGet"),
                                          {varGet(cs, cfg::LocalRef::selfVariable(), builder, irctx, rubyBlockId),
                                           Payload::idIntern(cs, builder, alias.instanceField.shortName(cs))});
        }
    }
    if (irctx.escapedVariableIndices.contains(local)) {
        auto [offset, index] = indexForLocalVariable(cs, irctx, rubyBlockId, irctx.escapedVariableIndices.at(local));
        auto level = irctx.rubyBlockLevel[rubyBlockId];
        return builder.CreateCall(cs.getFunction("sorbet_readLocal"),
                                  {offset, index, llvm::ConstantInt::get(cs, llvm::APInt(64, level, true))});
    }

    // normal local variable
    return Payload::unboxRawValue(cs, builder, irctx.llvmVariables.at(local));
}

void Payload::varSet(CompilerState &cs, cfg::LocalRef local, llvm::Value *var, llvm::IRBuilderBase &build,
                     const IREmitterContext &irctx, int rubyBlockId) {
    auto &builder = builderCast(build);
    if (irctx.aliases.contains(local)) {
        // alias to a field or constant
        auto alias = irctx.aliases.at(local);
        switch (alias.kind) {
            case Alias::AliasKind::Constant: {
                auto sym = alias.constantSym;
                auto name = sym.data(cs.gs)->name.show(cs.gs);
                auto owner = sym.data(cs.gs)->owner;
                builder.CreateCall(cs.getFunction("sorbet_setConstant"),
                                   {Payload::getRubyConstant(cs, owner, builder), Payload::toCString(cs, name, builder),
                                    llvm::ConstantInt::get(cs, llvm::APInt(64, name.length())), var});
            } break;
            case Alias::AliasKind::GlobalField:
                builder.CreateCall(cs.getFunction("sorbet_globalVariableSet"),
                                   {Payload::idIntern(cs, builder, alias.globalField.shortName(cs)), var});
                break;
            case Alias::AliasKind::ClassField:
                builder.CreateCall(cs.getFunction("sorbet_classVariableSet"),
                                   {getClassVariableStoreClass(cs, builder, irctx),
                                    Payload::idIntern(cs, builder, alias.classField.shortName(cs)), var});
                break;
            case Alias::AliasKind::InstanceField:
                builder.CreateCall(cs.getFunction("sorbet_instanceVariableSet"),
                                   {Payload::varGet(cs, cfg::LocalRef::selfVariable(), builder, irctx, rubyBlockId),
                                    Payload::idIntern(cs, builder, alias.instanceField.shortName(cs)), var});
                break;
        }
        return;
    }
    if (irctx.escapedVariableIndices.contains(local)) {
        auto [offset, index] = indexForLocalVariable(cs, irctx, rubyBlockId, irctx.escapedVariableIndices.at(local));
        auto level = irctx.rubyBlockLevel[rubyBlockId];
        builder.CreateCall(cs.getFunction("sorbet_writeLocal"),
                           {offset, index, llvm::ConstantInt::get(cs, llvm::APInt(64, level, true)), var});
        return;
    }

    // normal local variable
    Payload::boxRawValue(cs, builder, irctx.llvmVariables.at(local), var);
}

void Payload::rubyStopInDebugger(CompilerState &cs, llvm::IRBuilderBase &build) {
    auto &builder = builderCast(build);
    builder.CreateCall(cs.getFunction("sorbet_stopInDebugger"), {});
}

void Payload::dbg_p(CompilerState &cs, llvm::IRBuilderBase &build, llvm::Value *val) {
    auto &builder = builderCast(build);
    builder.CreateCall(cs.getFunction("sorbet_dbg_p"), {val});
}

}; // namespace sorbet::compiler
