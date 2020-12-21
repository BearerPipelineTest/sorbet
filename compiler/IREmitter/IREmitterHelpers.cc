// These violate our poisons so have to happen first
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DerivedTypes.h" // FunctionType
#include "llvm/IR/IRBuilder.h"

#include "Payload.h"
#include "absl/base/casts.h"
#include "absl/strings/match.h"
#include "absl/strings/str_replace.h"
#include "ast/Helpers.h"
#include "ast/ast.h"
#include "cfg/CFG.h"
#include "common/sort.h"
#include "compiler/Core/CompilerState.h"
#include "compiler/IREmitter/CFGHelpers.h"
#include "compiler/IREmitter/IREmitterContext.h"
#include "compiler/IREmitter/IREmitterHelpers.h"
#include "compiler/Names/Names.h"

using namespace std;
namespace sorbet::compiler {

// Iterate over all instructions in the CFG, populating the alias map.
UnorderedMap<cfg::LocalRef, Alias> setupAliases(CompilerState &cs, const cfg::CFG &cfg) {
    UnorderedMap<cfg::LocalRef, Alias> aliases{};

    for (auto &bb : cfg.basicBlocks) {
        for (auto &bind : bb->exprs) {
            if (auto *i = cfg::cast_instruction<cfg::Alias>(bind.value.get())) {
                ENFORCE(aliases.find(bind.bind.variable) == aliases.end(), "Overwriting an entry in the aliases map");

                if (i->what == core::Symbols::Magic_undeclaredFieldStub()) {
                    // When `i->what` is undeclaredFieldStub, `i->name` is populated
                    auto name = i->name.shortName(cs);
                    if (name.size() > 2 && name[0] == '@' && name[1] == '@') {
                        aliases[bind.bind.variable] = Alias::forClassField(i->name);
                    } else if (name.size() > 1 && name[0] == '@') {
                        aliases[bind.bind.variable] = Alias::forInstanceField(i->name);
                    } else if (name.size() > 1 && name[0] == '$') {
                        aliases[bind.bind.variable] = Alias::forGlobalField(i->name);
                    } else {
                        ENFORCE(stoi((string)name) > 0, "'" + ((string)name) + "' is not a valid global name");
                        aliases[bind.bind.variable] = Alias::forGlobalField(i->name);
                    }
                } else {
                    // It's currently impossible in Sorbet to declare a global field with a T.let
                    // (they will all be Magic_undeclaredFieldStub)
                    auto name = i->what.data(cs)->name;
                    auto shortName = name.shortName(cs);
                    ENFORCE(!(shortName.size() > 0 && shortName[0] == '$'));

                    if (i->what.data(cs)->isField()) {
                        aliases[bind.bind.variable] = Alias::forInstanceField(name);
                    } else if (i->what.data(cs)->isStaticField()) {
                        if (shortName.size() > 2 && shortName[0] == '@' && shortName[1] == '@') {
                            aliases[bind.bind.variable] = Alias::forClassField(name);
                        } else {
                            aliases[bind.bind.variable] = Alias::forConstant(i->what);
                        }
                    } else {
                        aliases[bind.bind.variable] = Alias::forConstant(i->what);
                    }
                }
            }
        }
    }

    return aliases;
}

UnorderedMap<cfg::LocalRef, llvm::AllocaInst *>
setupLocalVariables(CompilerState &cs, cfg::CFG &cfg,
                    const UnorderedMap<cfg::LocalRef, optional<int>> &variablesPrivateToBlocks,
                    const IREmitterContext &irctx) {
    UnorderedMap<cfg::LocalRef, llvm::AllocaInst *> llvmVariables;
    llvm::IRBuilder<> builder(cs);
    {
        // nill out block local variables.
        auto valueType = cs.getValueType();
        vector<pair<cfg::LocalRef, optional<int>>> variablesPrivateToBlocksSorted;

        for (const auto &entry : variablesPrivateToBlocks) {
            variablesPrivateToBlocksSorted.emplace_back(entry);
        }
        fast_sort(variablesPrivateToBlocksSorted,
                  [](const auto &left, const auto &right) -> bool { return left.first.id() < right.first.id(); });
        for (const auto &entry : variablesPrivateToBlocksSorted) {
            auto var = entry.first;
            if (entry.second == std::nullopt) {
                continue;
            }
            auto svName = var.data(cfg)._name.shortName(cs);
            builder.SetInsertPoint(irctx.functionInitializersByFunction[entry.second.value()]);
            auto alloca = llvmVariables[var] =
                builder.CreateAlloca(valueType, nullptr, llvm::StringRef(svName.data(), svName.length()));
            auto nilValueRaw = Payload::rubyNil(cs, builder);
            Payload::boxRawValue(cs, builder, alloca, nilValueRaw);
        }
    }

    {
        // reserve the magical return value
        builder.SetInsertPoint(irctx.functionInitializersByFunction[0]);
        auto name = Names::returnValue(cs);
        auto var = cfg.enterLocal(core::LocalVariable{name, 1});
        auto nameStr = name.toString(cs);
        llvmVariables[var] =
            builder.CreateAlloca(cs.getValueType(), nullptr, llvm::StringRef(nameStr.data(), nameStr.length()));
    }

    return llvmVariables;
}

namespace {

// Bundle up a bunch of state used for capture tracking to simplify the interface in findCaptures below.
class TrackCaptures final {
public:
    const UnorderedMap<cfg::LocalRef, Alias> &aliases;

    UnorderedMap<cfg::LocalRef, optional<int>> privateUsages;
    UnorderedMap<cfg::LocalRef, int> escapedIndexes;
    int escapedIndexCounter;
    bool usesBlockArg;
    cfg::LocalRef blkArg;

    TrackCaptures(const UnorderedMap<cfg::LocalRef, Alias> &aliases)
        : aliases(aliases), privateUsages{}, escapedIndexes{}, escapedIndexCounter{0},
          usesBlockArg{false}, blkArg{cfg::LocalRef::noVariable()} {}

    void trackBlockUsage(cfg::BasicBlock *bb, cfg::LocalRef lv) {
        usesBlockArg = usesBlockArg || lv == blkArg;
        auto fnd = privateUsages.find(lv);
        if (fnd != privateUsages.end()) {
            auto &store = fnd->second;
            if (store && store.value() != bb->rubyBlockId) {
                store = nullopt;
                escapedIndexes[lv] = escapedIndexCounter;
                escapedIndexCounter += 1;
            }
        } else {
            privateUsages[lv] = bb->rubyBlockId;
        }

        // if the variable is an alias to an instance variable, <self> will get referenced because we
        // synthesize a call to `sorbet_instanceVariableGet`
        const auto alias = aliases.find(lv);
        if (alias != aliases.end()) {
            if (alias->second.kind == Alias::AliasKind::InstanceField) {
                trackBlockUsage(bb, cfg::LocalRef::selfVariable());
            }
        }
    }

    tuple<UnorderedMap<cfg::LocalRef, optional<int>>, UnorderedMap<cfg::LocalRef, int>, bool> finalize() {
        return {std::move(privateUsages), std::move(escapedIndexes), usesBlockArg};
    }
};

} // namespace

/* if local variable is only used in block X, it maps the local variable to X, otherwise, it maps local variable to a
 * negative number */
tuple<UnorderedMap<cfg::LocalRef, optional<int>>, UnorderedMap<cfg::LocalRef, int>, bool>
findCaptures(CompilerState &cs, const ast::MethodDef &mdef, cfg::CFG &cfg,
             const UnorderedMap<cfg::LocalRef, Alias> &aliases) {
    TrackCaptures usage(aliases);

    int argId = -1;
    for (auto &arg : mdef.args) {
        argId += 1;
        ast::Local const *local = nullptr;
        if (auto *opt = ast::cast_tree<ast::OptionalArg>(arg)) {
            local = ast::cast_tree<ast::Local>(opt->expr);
        } else {
            local = ast::cast_tree<ast::Local>(arg);
        }
        ENFORCE(local);
        auto localRef = cfg.enterLocal(local->localVariable);
        usage.trackBlockUsage(cfg.entry(), localRef);
        if (cfg.symbol.data(cs)->arguments()[argId].flags.isBlock) {
            usage.blkArg = localRef;
        }
    }

    for (auto &bb : cfg.basicBlocks) {
        for (cfg::Binding &bind : bb->exprs) {
            usage.trackBlockUsage(bb.get(), bind.bind.variable);
            typecase(
                bind.value.get(), [&](cfg::Ident *i) { usage.trackBlockUsage(bb.get(), i->what); },
                [&](cfg::Alias *i) { /* nothing */
                },
                [&](cfg::SolveConstraint *i) { /* nothing*/ },
                [&](cfg::Send *i) {
                    for (auto &arg : i->args) {
                        usage.trackBlockUsage(bb.get(), arg.variable);
                    }
                    usage.trackBlockUsage(bb.get(), i->recv.variable);
                },
                [&](cfg::Return *i) { usage.trackBlockUsage(bb.get(), i->what.variable); },
                [&](cfg::BlockReturn *i) { usage.trackBlockUsage(bb.get(), i->what.variable); },
                [&](cfg::LoadSelf *i) { /*nothing*/ /*todo: how does instance exec pass self?*/ },
                [&](cfg::Literal *i) { /* nothing*/ }, [&](cfg::GetCurrentException *i) { /*nothing*/ },
                [&](cfg::ArgPresent *i) { /*nothing*/ }, [&](cfg::LoadArg *i) { /*nothing*/ },
                [&](cfg::LoadYieldParams *i) { /*nothing*/ },
                [&](cfg::Cast *i) { usage.trackBlockUsage(bb.get(), i->value.variable); },
                [&](cfg::TAbsurd *i) { usage.trackBlockUsage(bb.get(), i->what.variable); });
        }

        // no need to track the condition variable if the jump is unconditional
        if (bb->bexit.thenb != bb->bexit.elseb) {
            usage.trackBlockUsage(bb.get(), bb->bexit.cond.variable);
        }
    }

    return usage.finalize();
}

int getMaxSendArgCount(cfg::CFG &cfg) {
    int maxSendArgCount = 0;
    for (auto &bb : cfg.basicBlocks) {
        for (cfg::Binding &bind : bb->exprs) {
            if (auto snd = cfg::cast_instruction<cfg::Send>(bind.value.get())) {
                int numPosArgs = snd->numPosArgs;
                int numKwArgs = snd->args.size() - numPosArgs;

                // add one for the keyword arguments hash
                if (numPosArgs < snd->args.size()) {
                    numPosArgs++;
                }

                int numArgs = std::max(numPosArgs, numKwArgs);
                if (maxSendArgCount < numArgs) {
                    maxSendArgCount = numArgs;
                }
            }
        }
    }
    return maxSendArgCount;
}

// TODO
llvm::DISubroutineType *getDebugFunctionType(CompilerState &cs, llvm::Function *func) {
    vector<llvm::Metadata *> eltTys;

    auto *valueTy = cs.debug->createBasicType("VALUE", 64, llvm::dwarf::DW_ATE_signed);
    eltTys.push_back(valueTy);

    // NOTE: the return type is always the first element in the array
    return cs.debug->createSubroutineType(cs.debug->getOrCreateTypeArray(eltTys));
}

llvm::DISubprogram *getDebugScope(CompilerState &cs, cfg::CFG &cfg, llvm::DIScope *parent, llvm::Function *func,
                                  int rubyBlockId) {
    auto debugFile = cs.debug->createFile(cs.compileUnit->getFilename(), cs.compileUnit->getDirectory());
    auto loc = cfg.symbol.data(cs)->loc();

    auto owner = cfg.symbol.data(cs)->owner;
    std::string diName(owner.data(cs)->name.shortName(cs));

    if (owner.data(cs)->isSingletonClass(cs)) {
        diName += ".";
    } else {
        diName += "#";
    }

    diName += cfg.symbol.data(cs)->name.shortName(cs);

    // Line number 0 indicates that the compiler knows the entity came from this
    // particular file, but doesn't have precise location tracking for it.  This
    // can happen with e.g. synthesized packages.
    auto lineNo = loc.exists() ? loc.position(cs).first.line : 0;

    return cs.debug->createFunction(parent, diName, func->getName(), debugFile, lineNo, getDebugFunctionType(cs, func),
                                    lineNo, llvm::DINode::FlagPrototyped, llvm::DISubprogram::SPFlagDefinition);
}

void getRubyBlocks2FunctionsMapping(CompilerState &cs, cfg::CFG &cfg, llvm::Function *func,
                                    const vector<FunctionType> &blockTypes, vector<llvm::Function *> &funcs,
                                    vector<llvm::DISubprogram *> &scopes) {
    auto *bt = cs.getRubyBlockFFIType();
    auto *et = cs.getRubyExceptionFFIType();
    for (int i = 0; i <= cfg.maxRubyBlockId; i++) {
        switch (blockTypes[i]) {
            case FunctionType::Method:
            case FunctionType::StaticInit:
                funcs[i] = func;
                break;

            case FunctionType::Block: {
                auto *fp =
                    llvm::Function::Create(bt, llvm::Function::InternalLinkage,
                                           llvm::Twine{func->getName()} + "$block_" + llvm::Twine(i), *cs.module);
                // setup argument names
                fp->arg_begin()->setName("firstYieldArgRaw");
                (fp->arg_begin() + 1)->setName("localsOffset");
                (fp->arg_begin() + 2)->setName("argc");
                (fp->arg_begin() + 3)->setName("argArray");
                (fp->arg_begin() + 4)->setName("blockArg");
                funcs[i] = fp;
                break;
            }

            // NOTE: explicitly treating Unused functions like Exception functions, as they'll be collected by llvm
            // anyway.
            case FunctionType::ExceptionBegin:
            case FunctionType::Rescue:
            case FunctionType::Ensure:
            case FunctionType::Unused: {
                auto *fp =
                    llvm::Function::Create(et, llvm::Function::InternalLinkage,
                                           llvm::Twine{func->getName()} + "$block_" + llvm::Twine(i), *cs.module);

                // argument names
                fp->arg_begin()->setName("pc");
                (fp->arg_begin() + 1)->setName("iseq_encoded");
                (fp->arg_begin() + 2)->setName("localsOffset");

                funcs[i] = fp;
                break;
            }
        }

        auto *parent = i == 0 ? static_cast<llvm::DIScope *>(cs.compileUnit) : scopes[0];
        auto *scope = getDebugScope(cs, cfg, parent, funcs[i], i);
        scopes[i] = scope;
        funcs[i]->setSubprogram(scope);

        ENFORCE(scope->describes(funcs[i]));
    }
};

// Resolve a ruby block id to one that will have a frame allocated.
int resolveParent(const vector<FunctionType> &blockTypes, const vector<int> &blockParents, int candidate) {
    while (candidate > 0) {
        if (blockTypes[candidate] != FunctionType::ExceptionBegin) {
            break;
        }
        candidate = blockParents[candidate];
    }

    return candidate;
}

// Returns the mapping of ruby block id to function type, as well as the mapping from basic block to exception handling
// body block id.
void determineBlockTypes(CompilerState &cs, cfg::CFG &cfg, vector<FunctionType> &blockTypes, vector<int> &blockParents,
                         vector<int> &exceptionHandlingBlockHeaders, vector<int> &basicBlockJumpOverrides) {
    // ruby block 0 is always the top-level of the method being compiled
    if (cfg.symbol.data(cs)->name == core::Names::staticInit()) {
        // NOTE: We're explicitly not using IREmitterHelpers::isFileOrClassStaticInit here, as we want to distinguish
        // between the file-level and class/module static-init methods.
        //
        // When ruby runs the `Init_` function to initialize the whole object for this function it pushes a c frame for
        // that function on the ruby stack, and we update that frame with the iseq that we make for tracking line
        // numbers.  However when we run our static-init methods for classes and modules we call the c functions
        // directly, and want to avoid mutating the ruby stack frame. Thus those functions get marked as StaticInit,
        // while the file-level static-init gets marked as Method.
        //
        // https://github.com/ruby/ruby/blob/a9a48e6a741f048766a2a287592098c4f6c7b7c7/load.c#L1033-L1034
        blockTypes[0] = FunctionType::StaticInit;
    } else {
        blockTypes[0] = FunctionType::Method;
    }

    for (auto &b : cfg.basicBlocks) {
        if (b->bexit.cond.variable == cfg::LocalRef::blockCall()) {
            blockTypes[b->rubyBlockId] = FunctionType::Block;

            // the else branch always points back to the original owning rubyBlockId of the block call
            blockParents[b->rubyBlockId] = resolveParent(blockTypes, blockParents, b->bexit.elseb->rubyBlockId);

        } else if (b->bexit.cond.variable.data(cfg)._name == core::Names::exceptionValue()) {
            auto *bodyBlock = b->bexit.elseb;
            auto *handlersBlock = b->bexit.thenb;

            // the relative block ids of blocks that are involved in the translation of an exception handling block.
            auto bodyBlockId = bodyBlock->rubyBlockId;
            auto handlersBlockId = bodyBlockId + cfg::CFG::HANDLERS_BLOCK_OFFSET;
            auto ensureBlockId = bodyBlockId + cfg::CFG::ENSURE_BLOCK_OFFSET;
            auto elseBlockId = bodyBlockId + cfg::CFG::ELSE_BLOCK_OFFSET;

            // `b` is the exception handling header block if the two branches from it have the sequential ids we would
            // expect for the handler and body blocks. The reason we bail out here if this isn't the case is because
            // there are other blocks within the translation that will also jump based on the value of the same
            // exception value variable.
            if (handlersBlock->rubyBlockId != handlersBlockId) {
                continue;
            }

            auto *elseBlock = CFGHelpers::findRubyBlockEntry(cfg, elseBlockId);
            auto *ensureBlock = CFGHelpers::findRubyBlockEntry(cfg, ensureBlockId);

            // Because of the way the CFG is constructed, we'll either have an else bock, and ensure block, or both
            // present. It's currently not possible to end up with neither.
            ENFORCE(elseBlock || ensureBlock);

            {
                // Find the exit block for exception handling so that we can redirect the header to it.
                auto *exit = ensureBlock == nullptr ? elseBlock : ensureBlock;
                auto exits = CFGHelpers::findRubyBlockExits(cfg, exit->rubyBlockId);

                // The ensure block should only ever jump to the code that follows the begin/end block.
                ENFORCE(exits.size() == 1);

                // Have the entry block jump over all of the exception handling machinery.
                basicBlockJumpOverrides[handlersBlock->id] = exits.front()->id;
                basicBlockJumpOverrides[bodyBlock->id] = exits.front()->id;
            }

            exceptionHandlingBlockHeaders[b->id] = bodyBlockId;

            blockTypes[bodyBlockId] = FunctionType::ExceptionBegin;
            blockTypes[handlersBlockId] = FunctionType::Rescue;

            if (elseBlock) {
                blockTypes[elseBlockId] = FunctionType::ExceptionBegin;
            }

            if (ensureBlock) {
                blockTypes[ensureBlockId] = FunctionType::Ensure;
            }

            // All exception handling blocks are children of `b`, as far as ruby iseq allocation is concerned.
            auto parent = resolveParent(blockTypes, blockParents, b->rubyBlockId);
            blockParents[bodyBlockId] = parent;
            blockParents[handlersBlockId] = parent;
            blockParents[elseBlockId] = parent;
            blockParents[ensureBlockId] = parent;
        }
    }

    return;
}

// Returns the number of scopes that must be traversed to get back out out to the top-level method frame.
int getBlockLevel(vector<int> &blockParents, vector<FunctionType> &blockTypes, int rubyBlockId) {
    auto level = 0;

    while (true) {
        switch (blockTypes[rubyBlockId]) {
            case FunctionType::Method:
            case FunctionType::StaticInit:
                return level;

            case FunctionType::Block:
            case FunctionType::Rescue:
            case FunctionType::Ensure:
                // Increment the level, as we're crossing through a non-method stack frame to get back to our parent.
                ++level;
                rubyBlockId = blockParents[rubyBlockId];
                break;

            case FunctionType::ExceptionBegin:
            case FunctionType::Unused:
                // ExceptionBegin is considered to be part of the containing frame, so there's no block present here,
                // and unused functions will never be called, so it's fine for them to have garbage values here.
                rubyBlockId = blockParents[rubyBlockId];
                break;
        }
    }
}

vector<int> getBlockLevels(vector<int> &blockParents, vector<FunctionType> &blockTypes) {
    vector<int> levels(blockTypes.size(), 0);

    for (auto i = 0; i < blockTypes.size(); ++i) {
        levels[i] = getBlockLevel(blockParents, blockTypes, i);
    }

    return levels;
}

IREmitterContext IREmitterHelpers::getSorbetBlocks2LLVMBlockMapping(CompilerState &cs, cfg::CFG &cfg,
                                                                    const ast::MethodDef &md,
                                                                    llvm::Function *mainFunc) {
    vector<int> basicBlockJumpOverrides(cfg.maxBasicBlockId);
    vector<int> basicBlockRubyBlockId(cfg.maxBasicBlockId, 0);
    llvm::IRBuilder<> builder(cs);
    {
        for (int i = 0; i < cfg.maxBasicBlockId; i++) {
            basicBlockJumpOverrides[i] = i;
        }

        for (auto &bb : cfg.basicBlocks) {
            basicBlockRubyBlockId[bb->id] = bb->rubyBlockId;
        }
    }

    vector<FunctionType> blockTypes(cfg.maxRubyBlockId + 1, FunctionType::Unused);
    vector<int> blockParents(cfg.maxRubyBlockId + 1, 0);
    vector<int> exceptionHandlingBlockHeaders(cfg.maxBasicBlockId + 1, 0);
    determineBlockTypes(cs, cfg, blockTypes, blockParents, exceptionHandlingBlockHeaders, basicBlockJumpOverrides);

    vector<llvm::Function *> rubyBlock2Function(cfg.maxRubyBlockId + 1, nullptr);
    vector<llvm::DISubprogram *> blockScopes(cfg.maxRubyBlockId + 1, nullptr);
    getRubyBlocks2FunctionsMapping(cs, cfg, mainFunc, blockTypes, rubyBlock2Function, blockScopes);

    auto blockLevels = getBlockLevels(blockParents, blockTypes);

    auto aliases = setupAliases(cs, cfg);
    const int maxSendArgCount = getMaxSendArgCount(cfg);
    auto [variablesPrivateToBlocks, escapedVariableIndices, usesBlockArgs] = findCaptures(cs, md, cfg, aliases);
    vector<llvm::BasicBlock *> functionInitializersByFunction;
    vector<llvm::BasicBlock *> argumentSetupBlocksByFunction;
    vector<llvm::BasicBlock *> userEntryBlockByFunction(rubyBlock2Function.size());
    vector<llvm::AllocaInst *> sendArgArrayByBlock;
    vector<llvm::AllocaInst *> lineNumberPtrsByFunction;
    vector<llvm::AllocaInst *> iseqEncodedPtrsByFunction;

    bool useLocalsOffset = IREmitterHelpers::isFileOrClassStaticInit(cs, md.symbol);
    vector<llvm::Value *> localsOffset;

    int i = 0;
    auto lineNumberPtrType = llvm::PointerType::getUnqual(llvm::Type::getInt64PtrTy(cs));
    auto iseqEncodedPtrType = llvm::Type::getInt64PtrTy(cs);
    for (auto &fun : rubyBlock2Function) {
        auto inits = functionInitializersByFunction.emplace_back(llvm::BasicBlock::Create(
            cs, "functionEntryInitializers",
            fun)); // we will build a link for this block later, after we finish building expressions into it
        builder.SetInsertPoint(inits);
        auto sendArgArray = builder.CreateAlloca(llvm::ArrayType::get(llvm::Type::getInt64Ty(cs), maxSendArgCount),
                                                 nullptr, "callArgs");
        llvm::Value *offsetValue = nullptr;
        switch (blockTypes[i]) {
            case FunctionType::Method:
            case FunctionType::StaticInit:
                if (useLocalsOffset) {
                    offsetValue = builder.CreateLoad(IREmitterHelpers::getStaticInitLocalsOffset(cs, md.symbol));
                } else {
                    offsetValue = llvm::ConstantInt::get(cs, llvm::APInt(64, 0, false));
                }
                break;

            case FunctionType::Block:
                offsetValue = fun->arg_begin() + 1;
                break;

            case FunctionType::ExceptionBegin:
            case FunctionType::Rescue:
            case FunctionType::Ensure:
            case FunctionType::Unused:
                offsetValue = fun->arg_begin() + 2;
                break;
        }
        ENFORCE(offsetValue != nullptr);
        localsOffset.emplace_back(offsetValue);
        sendArgArrayByBlock.emplace_back(sendArgArray);
        auto lineNumberPtr = builder.CreateAlloca(lineNumberPtrType, nullptr, "lineCountStore");
        lineNumberPtrsByFunction.emplace_back(lineNumberPtr);
        auto iseqEncodedPtr = builder.CreateAlloca(iseqEncodedPtrType, nullptr, "iseqEncodedStore");
        iseqEncodedPtrsByFunction.emplace_back(iseqEncodedPtr);
        argumentSetupBlocksByFunction.emplace_back(llvm::BasicBlock::Create(cs, "argumentSetup", fun));
        i++;
    }

    vector<llvm::BasicBlock *> blockExits(cfg.maxRubyBlockId + 1);
    for (auto rubyBlockId = 0; rubyBlockId <= cfg.maxRubyBlockId; ++rubyBlockId) {
        blockExits[rubyBlockId] =
            llvm::BasicBlock::Create(cs, llvm::Twine("blockExit"), rubyBlock2Function[rubyBlockId]);
    }

    vector<llvm::BasicBlock *> deadBlocks(cfg.maxRubyBlockId + 1);
    vector<llvm::BasicBlock *> llvmBlocks(cfg.maxBasicBlockId + 1);
    for (auto &b : cfg.basicBlocks) {
        if (b.get() == cfg.entry()) {
            llvmBlocks[b->id] = userEntryBlockByFunction[0] =
                llvm::BasicBlock::Create(cs, "userEntry", rubyBlock2Function[0]);
        } else if (b.get() == cfg.deadBlock()) {
            for (auto rubyBlockId = 0; rubyBlockId <= cfg.maxRubyBlockId; ++rubyBlockId) {
                deadBlocks[rubyBlockId] =
                    llvm::BasicBlock::Create(cs, llvm::Twine("dead"), rubyBlock2Function[rubyBlockId]);
            }

            llvmBlocks[b->id] = deadBlocks[0];
        } else {
            llvmBlocks[b->id] = llvm::BasicBlock::Create(cs, llvm::Twine("BB") + llvm::Twine(b->id),
                                                         rubyBlock2Function[b->rubyBlockId]);
        }
    }
    vector<shared_ptr<core::SendAndBlockLink>> blockLinks(rubyBlock2Function.size());
    vector<vector<cfg::LocalRef>> rubyBlockArgs(rubyBlock2Function.size());

    {
        // fill in data about args for main function
        for (auto &treeArg : md.args) {
            auto *a = ast::MK::arg2Local(treeArg);
            rubyBlockArgs[0].emplace_back(cfg.enterLocal(a->localVariable));
        }
    }

    int numArgs = md.symbol.data(cs)->arguments().size();
    vector<cfg::LocalRef> argPresentVariables(numArgs, cfg::LocalRef::noVariable());
    for (auto &b : cfg.basicBlocks) {
        if (b->bexit.cond.variable == cfg::LocalRef::blockCall()) {
            userEntryBlockByFunction[b->rubyBlockId] = llvmBlocks[b->bexit.thenb->id];
            basicBlockJumpOverrides[b->id] = b->bexit.elseb->id;
            auto backId = -1;
            for (auto bid = 0; bid < b->backEdges.size(); bid++) {
                if (b->backEdges[bid]->rubyBlockId < b->rubyBlockId) {
                    backId = bid;
                    break;
                };
            }
            ENFORCE(backId >= 0);

            cfg::Instruction *expected = nullptr;
            for (auto i = b->backEdges[backId]->exprs.rbegin(); i != b->backEdges[backId]->exprs.rend(); ++i) {
                if (i->bind.variable.data(cfg)._name == core::Names::blockPreCallTemp()) {
                    expected = i->value.get();
                    break;
                }
            }
            ENFORCE(expected);

            auto expectedSend = cfg::cast_instruction<cfg::Send>(expected);
            ENFORCE(expectedSend);
            ENFORCE(expectedSend->link);
            blockLinks[b->rubyBlockId] = expectedSend->link;

            rubyBlockArgs[b->rubyBlockId].resize(expectedSend->link->argFlags.size(), cfg::LocalRef::noVariable());
            for (auto &maybeCallOnLoadYieldArg : b->bexit.thenb->exprs) {
                auto maybeCast = cfg::cast_instruction<cfg::Send>(maybeCallOnLoadYieldArg.value.get());
                if (maybeCast == nullptr || maybeCast->recv.variable.data(cfg)._name != core::Names::blkArg() ||
                    maybeCast->fun != core::Names::squareBrackets() || maybeCast->args.size() != 1) {
                    continue;
                }
                if (!core::isa_type<core::LiteralType>(maybeCast->args[0].type)) {
                    continue;
                }
                auto litType = core::cast_type_nonnull<core::LiteralType>(maybeCast->args[0].type);
                rubyBlockArgs[b->rubyBlockId][litType.asInteger()] = maybeCallOnLoadYieldArg.bind.variable;
            }
        } else if (b->bexit.cond.variable.data(cfg)._name == core::Names::exceptionValue()) {
            if (exceptionHandlingBlockHeaders[b->id] == 0) {
                continue;
            }

            auto *bodyBlock = b->bexit.elseb;
            auto *handlersBlock = b->bexit.thenb;

            // the relative block ids of blocks that are involved in the translation of an exception handling block.
            auto bodyBlockId = bodyBlock->rubyBlockId;
            auto handlersBlockId = bodyBlockId + 1;
            auto ensureBlockId = bodyBlockId + 2;
            auto elseBlockId = bodyBlockId + 3;

            userEntryBlockByFunction[bodyBlockId] = llvmBlocks[bodyBlock->id];
            userEntryBlockByFunction[handlersBlockId] = llvmBlocks[handlersBlock->id];

            if (auto *elseBlock = CFGHelpers::findRubyBlockEntry(cfg, elseBlockId)) {
                userEntryBlockByFunction[elseBlockId] = llvmBlocks[elseBlock->id];
            }

            if (auto *ensureBlock = CFGHelpers::findRubyBlockEntry(cfg, ensureBlockId)) {
                userEntryBlockByFunction[ensureBlockId] = llvmBlocks[ensureBlock->id];
            }
        } else if (b->bexit.cond.variable.data(cfg)._name == core::Names::argPresent()) {
            // the ArgPresent instruction is always the last one generated in the block
            int argId = -1;
            auto &bind = b->exprs.back();
            typecase(
                bind.value.get(),
                [&](cfg::ArgPresent *i) {
                    ENFORCE(bind.bind.variable == b->bexit.cond.variable);
                    argId = i->argId;
                },
                [](cfg::Instruction *i) { /* do nothing */ });

            ENFORCE(argId >= 0, "Missing an index for argPresent condition variable");

            argPresentVariables[argId] = b->bexit.cond.variable;
        }
    }

    llvm::BasicBlock *postProcessBlock = llvm::BasicBlock::Create(cs, "postProcess", mainFunc);

    IREmitterContext approximation{
        cfg,
        aliases,
        functionInitializersByFunction,
        argumentSetupBlocksByFunction,
        userEntryBlockByFunction,
        llvmBlocks,
        move(basicBlockJumpOverrides),
        move(basicBlockRubyBlockId),
        move(sendArgArrayByBlock),
        useLocalsOffset,
        localsOffset,
        std::move(escapedVariableIndices),
        std::move(argPresentVariables),
        {},
        postProcessBlock,
        move(blockLinks),
        move(rubyBlockArgs),
        move(rubyBlock2Function),
        move(blockTypes),
        move(blockParents),
        move(blockLevels),
        move(lineNumberPtrsByFunction),
        move(iseqEncodedPtrsByFunction),
        usesBlockArgs,
        move(exceptionHandlingBlockHeaders),
        move(deadBlocks),
        move(blockExits),
        move(blockScopes),
    };

    approximation.llvmVariables = setupLocalVariables(cs, cfg, variablesPrivateToBlocks, approximation);

    return approximation;
}

namespace {

string getFunctionNamePrefix(CompilerState &cs, core::SymbolRef sym) {
    auto maybeAttached = sym.data(cs)->attachedClass(cs);
    if (maybeAttached.exists()) {
        return getFunctionNamePrefix(cs, maybeAttached) + ".singleton_class";
    }
    string suffix;
    auto name = sym.data(cs)->name;
    if (name.kind() == core::NameKind::CONSTANT && name.dataCnst(cs)->original.kind() == core::NameKind::UTF8) {
        suffix = (string)name.shortName(cs);
    } else {
        suffix = name.toString(cs);
    }
    string prefix =
        sym.data(cs)->owner == core::Symbols::root() ? "" : getFunctionNamePrefix(cs, sym.data(cs)->owner) + "::";

    return prefix + suffix;
}
} // namespace

string IREmitterHelpers::getFunctionName(CompilerState &cs, core::SymbolRef sym) {
    auto maybeAttachedOwner = sym.data(cs)->owner.data(cs)->attachedClass(cs);
    string prefix = "func_";
    if (maybeAttachedOwner.exists()) {
        prefix = prefix + getFunctionNamePrefix(cs, maybeAttachedOwner) + ".";
    } else {
        prefix = prefix + getFunctionNamePrefix(cs, sym.data(cs)->owner) + "#";
    }

    auto name = sym.data(cs)->name;
    string suffix;
    if (name.kind() == core::NameKind::UTF8) {
        suffix = (string)name.shortName(cs);
    } else {
        suffix = name.toString(cs);
    }

    return prefix + suffix;
}

bool IREmitterHelpers::isFileOrClassStaticInit(const core::GlobalState &cs, core::SymbolRef sym) {
    auto name = sym.data(cs)->name;
    return (name.kind() == core::NameKind::UTF8 ? name : name.dataUnique(cs)->original) == core::Names::staticInit();
}

core::Loc IREmitterHelpers::getMethodLineBounds(const core::GlobalState &gs, core::SymbolRef sym, core::FileRef file,
                                                core::LocOffsets offsets) {
    if (IREmitterHelpers::isFileOrClassStaticInit(gs, sym)) {
        return core::Loc(file, core::LocOffsets{0, offsets.endLoc});
    } else {
        return core::Loc(file, offsets);
    }
}

llvm::GlobalVariable *IREmitterHelpers::getStaticInitLocalsOffset(CompilerState &cs, core::SymbolRef sym) {
    string rawName = "offset_";
    rawName += sym.show(cs);
    return static_cast<llvm::GlobalVariable *>(cs.module->getOrInsertGlobal(rawName, llvm::Type::getInt64Ty(cs), [&] {
        return new llvm::GlobalVariable(*cs.module, llvm::Type::getInt64Ty(cs), false,
                                        llvm::GlobalVariable::InternalLinkage,
                                        llvm::ConstantInt::get(cs, llvm::APInt(64, 0, false)), rawName);
    }));
}

namespace {
llvm::GlobalValue::LinkageTypes getFunctionLinkageType(CompilerState &cs, core::SymbolRef sym) {
    if (IREmitterHelpers::isFileOrClassStaticInit(cs, sym)) {
        // this is top level code that shoudln't be callable externally.
        // Even more, sorbet reuses symbols used for these and thus if we mark them non-private we'll get link errors
        return llvm::Function::InternalLinkage;
    }
    return llvm::Function::ExternalLinkage;
}

llvm::Function *
getOrCreateFunctionWithName(CompilerState &cs, std::string name, llvm::FunctionType *ft,
                            llvm::GlobalValue::LinkageTypes linkageType = llvm::Function::InternalLinkage,
                            bool overrideLinkage = false) {
    auto func = cs.module->getFunction(name);
    if (func) {
        if (overrideLinkage) {
            func->setLinkage(linkageType);
        }
        return func;
    }
    return llvm::Function::Create(ft, linkageType, name, *cs.module);
}

}; // namespace

llvm::Function *IREmitterHelpers::lookupFunction(CompilerState &cs, core::SymbolRef sym) {
    ENFORCE(sym.data(cs)->name != core::Names::staticInit(), "use special helper instead");
    auto func = cs.getFunction(IREmitterHelpers::getFunctionName(cs, sym));
    return func;
}
llvm::Function *IREmitterHelpers::getOrCreateFunctionWeak(CompilerState &cs, core::SymbolRef sym) {
    ENFORCE(sym.data(cs)->name != core::Names::staticInit(), "use special helper instead");
    return getOrCreateFunctionWithName(cs, IREmitterHelpers::getFunctionName(cs, sym), cs.getRubyFFIType(),
                                       llvm::Function::WeakAnyLinkage);
}

llvm::Function *IREmitterHelpers::getOrCreateFunction(CompilerState &cs, core::SymbolRef sym) {
    ENFORCE(sym.data(cs)->name != core::Names::staticInit(), "use special helper instead");
    return getOrCreateFunctionWithName(cs, IREmitterHelpers::getFunctionName(cs, sym), cs.getRubyFFIType(),
                                       getFunctionLinkageType(cs, sym), true);
}

llvm::Function *IREmitterHelpers::getOrCreateStaticInit(CompilerState &cs, core::SymbolRef sym, core::LocOffsets loc) {
    ENFORCE(sym.data(cs)->name == core::Names::staticInit(), "use general helper instead");
    auto name = IREmitterHelpers::getFunctionName(cs, sym) + "L" + to_string(loc.beginPos());
    return getOrCreateFunctionWithName(cs, name, cs.getRubyFFIType(), getFunctionLinkageType(cs, sym), true);
}
llvm::Function *IREmitterHelpers::getInitFunction(CompilerState &cs, core::SymbolRef sym) {
    std::vector<llvm::Type *> NoArgs(0, llvm::Type::getVoidTy(cs));
    auto linkageType = llvm::Function::InternalLinkage;
    auto baseName = IREmitterHelpers::getFunctionName(cs, sym);

    auto ft = llvm::FunctionType::get(llvm::Type::getVoidTy(cs), NoArgs, false);
    return getOrCreateFunctionWithName(cs, "Init_" + baseName, ft, linkageType);
}

llvm::Function *IREmitterHelpers::cleanFunctionBody(CompilerState &cs, llvm::Function *func) {
    for (auto &bb : *func) {
        bb.dropAllReferences();
    }

    // Delete all basic blocks. They are now unused, except possibly by
    // blockaddresses, but BasicBlock's destructor takes care of those.
    while (!func->empty()) {
        func->begin()->eraseFromParent();
    }
    return func;
}

void IREmitterHelpers::emitDebugLoc(CompilerState &cs, llvm::IRBuilderBase &build, const IREmitterContext &irctx,
                                    int rubyBlockId, core::Loc loc) {
    auto &builder = static_cast<llvm::IRBuilder<> &>(build);

    auto *scope = irctx.blockScopes[rubyBlockId];
    unsigned line, column;

    if (!loc.exists()) {
        // This location seems less useful than no debug location, but it ensures
        // that we have a "real" debug location to set for LLVM.  LLVM verification
        // will assert if we have calls in debug-info-laden functions that can be
        // inlined and are not tagged with proper debug information.
        line = 0;
        column = 0;
    } else {
        auto start = loc.position(cs).first;
        line = start.line;
        column = start.column;
    }

    builder.SetCurrentDebugLocation(llvm::DebugLoc::get(line, column, scope));
}

void IREmitterHelpers::emitReturn(CompilerState &cs, llvm::IRBuilderBase &build, const IREmitterContext &irctx,
                                  int rubyBlockId, llvm::Value *retVal) {
    auto &builder = static_cast<llvm::IRBuilder<> &>(build);

    if (functionTypePushesFrame(irctx.rubyBlockType[rubyBlockId])) {
        builder.CreateCall(cs.getFunction("sorbet_popRubyStack"), {});
    }

    builder.CreateRet(retVal);
}

void IREmitterHelpers::emitTypeTest(CompilerState &cs, llvm::IRBuilderBase &build, llvm::Value *value,
                                    const core::TypePtr &expectedType, std::string_view description) {
    auto &builder = static_cast<llvm::IRBuilder<> &>(build);
    auto passedTypeTest = Payload::typeTest(cs, builder, value, expectedType);
    auto successBlock = llvm::BasicBlock::Create(cs, "typeTestSuccess", builder.GetInsertBlock()->getParent());

    auto failBlock = llvm::BasicBlock::Create(cs, "typeTestFail", builder.GetInsertBlock()->getParent());

    auto expected = Payload::setExpectedBool(cs, builder, passedTypeTest, true);
    builder.CreateCondBr(expected, successBlock, failBlock);
    builder.SetInsertPoint(failBlock);
    // this will throw exception
    builder.CreateCall(cs.getFunction("sorbet_cast_failure"), {value, Payload::toCString(cs, description, builder),
                                                               Payload::toCString(cs, expectedType.show(cs), builder)});
    builder.CreateUnreachable();
    builder.SetInsertPoint(successBlock);
}

llvm::Value *IREmitterHelpers::emitLiteralish(CompilerState &cs, llvm::IRBuilderBase &build, const core::TypePtr &lit) {
    auto &builder = static_cast<llvm::IRBuilder<> &>(build);
    if (lit.derivesFrom(cs, core::Symbols::FalseClass())) {
        return Payload::rubyFalse(cs, builder);
    }
    if (lit.derivesFrom(cs, core::Symbols::TrueClass())) {
        return Payload::rubyTrue(cs, builder);
    }
    if (lit.derivesFrom(cs, core::Symbols::NilClass())) {
        return Payload::rubyNil(cs, builder);
    }

    auto litType = core::cast_type_nonnull<core::LiteralType>(lit);
    switch (litType.literalKind) {
        case core::LiteralType::LiteralTypeKind::Integer:
            return Payload::longToRubyValue(cs, builder, litType.asInteger());
        case core::LiteralType::LiteralTypeKind::Float:
            return Payload::doubleToRubyValue(cs, builder, litType.asFloat());
        case core::LiteralType::LiteralTypeKind::Symbol: {
            auto str = litType.asName(cs).shortName(cs);
            auto rawId = Payload::idIntern(cs, builder, str);
            return builder.CreateCall(cs.getFunction("rb_id2sym"), {rawId}, "rawSym");
        }
        case core::LiteralType::LiteralTypeKind::String: {
            auto str = litType.asName(cs).shortName(cs);
            return Payload::cPtrToRubyString(cs, builder, str, true);
        }
    }
}

bool IREmitterHelpers::hasBlockArgument(CompilerState &cs, int blockId, core::SymbolRef method,
                                        const IREmitterContext &irctx) {
    auto ty = irctx.rubyBlockType[blockId];
    if (!(ty == FunctionType::Block || ty == FunctionType::Method || ty == FunctionType::StaticInit)) {
        return false;
    }

    if (ty == FunctionType::Block) {
        auto blockLink = irctx.blockLinks[blockId];
        if (blockLink->argFlags.empty()) {
            return false;
        }

        return blockLink->argFlags.back().isBlock;
    }

    auto &args = method.data(cs)->arguments();
    if (args.empty()) {
        return false;
    }

    return args.back().flags.isBlock;
}

core::SymbolRef IREmitterHelpers::fixupOwningSymbol(const core::GlobalState &gs, core::SymbolRef sym) {
    if (sym == core::Symbols::root() || sym == core::Symbols::rootSingleton() ||
        sym == core::Symbols::PackageRegistry() || sym.data(gs)->name == core::Names::Constants::PkgRoot_Package()) {
        // Root methods end up going on Object.
        return core::Symbols::Object();
    }

    return sym;
}

std::string IREmitterHelpers::showClassNameWithoutOwner(const core::GlobalState &gs, core::SymbolRef sym) {
    std::string withoutOwnerStr;

    auto name = sym.data(gs)->name;
    if (name.kind() == core::NameKind::UNIQUE) {
        withoutOwnerStr = name.dataUnique(gs)->original.show(gs);
    } else {
        withoutOwnerStr = name.show(gs);
    };

    // This is a little bit gross.  Symbol performs this sort of logic itself, but
    // the above calls are done inside NameRef, which doesn't have the necessary
    // symbol ownership information to do this sort of munging.  So we have to
    // duplicate the Symbol logic here.
    if (sym.data(gs)->owner != core::Symbols::PackageRegistry()) {
        return withoutOwnerStr;
    }

    constexpr string_view packageNameSuffix = "_Package"sv;
    if (!absl::EndsWith(withoutOwnerStr, packageNameSuffix)) {
        return withoutOwnerStr;
    }

    return absl::StrReplaceAll(withoutOwnerStr.substr(0, withoutOwnerStr.size() - packageNameSuffix.size()),
                               {{"_", "::"}});
}

} // namespace sorbet::compiler
