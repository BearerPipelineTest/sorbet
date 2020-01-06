// These violate our poisons so have to happen first
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include "absl/synchronization/mutex.h"
#include "ast/ast.h"
#include "cfg/CFG.h"
#include "compiler/Core/AbortCompilation.h"
#include "compiler/Core/CompilerState.h"
#include "compiler/Rewriters/DefinitionRewriter.h"
#include "compiler/Errors/Errors.h"
#include "compiler/IREmitter/IREmitter.h"
#include "compiler/IREmitter/Payload/PayloadLoader.h"
#include "compiler/ObjectFileEmitter/ObjectFileEmitter.h"
#include "core/ErrorQueue.h"
#include "main/pipeline/semantic_extension/SemanticExtension.h"
#include <cxxopts.hpp>
#include <optional>

using namespace std;
namespace sorbet::pipeline::semantic_extension {
namespace {
string objectFileName(const core::GlobalState &gs, const core::FileRef &f) {
    string sourceFile(f.data(gs).path());
    if (sourceFile[0] == '.' && sourceFile[1] == '/') {
        sourceFile = sourceFile.substr(2);
    }
    absl::c_replace(sourceFile, '/', '_');
    absl::c_replace(sourceFile, '.', '_');
    sourceFile.replace(sourceFile.find("_rb"), 3, ".rb");
    return sourceFile;
}
} // namespace

class ThreadState {
public:
    llvm::LLVMContext lctx;
    unique_ptr<llvm::Module> combinedModule;
    core::FileRef file;
    bool aborted = false;
};

class LLVMSemanticExtension : public SemanticExtension {
    optional<string> irOutputDir;
    bool forceCompiled;
    mutable struct {
        UnorderedMap<std::thread::id, shared_ptr<ThreadState>> states;
        absl::Mutex mtx;
    } mutableState;

    shared_ptr<ThreadState> getThreadState() const {
        {
            absl::ReaderMutexLock lock(&mutableState.mtx);
            if (mutableState.states.contains(std::this_thread::get_id())) {
                return mutableState.states.at(std::this_thread::get_id());
            }
        }
        {
            absl::WriterMutexLock lock(&mutableState.mtx);
            return mutableState.states[std::this_thread::get_id()] = make_shared<ThreadState>();
        }
    }

    bool shouldCompile(const core::GlobalState &gs, const core::FileRef &f) const {
        if (!irOutputDir.has_value()) {
            return false;
        }
        if (forceCompiled) {
            return true;
        }
        // TODO parse this the same way as `typed:`
        return f.data(gs).source().find("# compiled: true\n") != string_view::npos;
    }

public:
    LLVMSemanticExtension(optional<string> irOutputDir, bool forceCompiled) {
        this->irOutputDir = move(irOutputDir);
        this->forceCompiled = forceCompiled;
    }

    virtual void run(core::MutableContext &ctx, ast::ClassDef *klass) const override {
        if (!shouldCompile(ctx, klass->loc.file())) {
            return;
        }
        if (klass->loc.file().data(ctx).strictLevel < core::StrictLevel::True) {
            if (auto e = ctx.state.beginError(klass->loc, core::errors::Compiler::Untyped)) {
                e.setHeader("File must be `typed: true` or higher to be compiled");
            }
        }
        if (!ast::isa_tree<ast::EmptyTree>(klass->name.get())) {
            return;
        }

        sorbet::compiler::DefinitionRewriter::run(ctx, klass);
    };

    virtual void typecheck(const core::GlobalState &gs, cfg::CFG &cfg,
                           std::unique_ptr<ast::MethodDef> &md) const override {
        if (!shouldCompile(gs, cfg.symbol.data(gs)->loc().file())) {
            return;
        }
        auto threadState = getThreadState();
        if (threadState->aborted) {
            return;
        }
        llvm::LLVMContext &lctx = threadState->lctx;
        unique_ptr<llvm::Module> &module = threadState->combinedModule;
        // TODO: Figure out why this isn't true
        // ENFORCE(absl::c_find(cfg.symbol.data(gs)->locs(), md->loc) != cfg.symbol.data(gs)->locs().end(),
        // md->loc.toString(gs));
        ENFORCE(md->loc.file().exists());
        if (!module) {
            module = sorbet::compiler::PayloadLoader::readDefaultModule(lctx);
            threadState->file = md->loc.file();
        } else {
            ENFORCE(threadState->file == md->loc.file());
        }
        ENFORCE(threadState->file.exists());
        compiler::CompilerState state(gs, lctx, module.get());
        try {
            sorbet::compiler::IREmitter::run(state, cfg, md);
            string fileName = objectFileName(gs, cfg.symbol.data(gs)->loc().file());
            sorbet::compiler::IREmitter::buildInitFor(state, cfg.symbol, fileName);
        } catch (sorbet::compiler::AbortCompilation &) {
            threadState->aborted = true;
        } catch (...) {
            // cleanup
            module = nullptr;
            threadState->file = core::FileRef();
            throw;
        }
    };

    virtual void finishTypecheckFile(const core::GlobalState &gs, const core::FileRef &f) const override {
        if (!shouldCompile(gs, f)) {
            return;
        }
        auto threadState = getThreadState();
        llvm::LLVMContext &lctx = threadState->lctx;
        unique_ptr<llvm::Module> module = move(threadState->combinedModule);
        if (!module) {
            ENFORCE(!threadState->file.exists());
            return;
        }
        if (threadState->aborted) {
            threadState->file = core::FileRef();
            return;
        }

        ENFORCE(threadState->file.exists());
        ENFORCE(f == threadState->file);
        if (f.data(gs).minErrorLevel() >= core::StrictLevel::True) {
            string fileName = objectFileName(gs, f);
            sorbet::compiler::ObjectFileEmitter::run(gs.tracer(), lctx, move(module), irOutputDir.value(), fileName);
        }
        ENFORCE(threadState->combinedModule == nullptr);
        threadState->file = core::FileRef();
    };

    virtual void finishTypecheck(const core::GlobalState &gs) const override {}

    virtual ~LLVMSemanticExtension(){};
    virtual std::unique_ptr<SemanticExtension> deepCopy(const core::GlobalState &from, core::GlobalState &to) override {
        return make_unique<LLVMSemanticExtension>(this->irOutputDir, this->forceCompiled);
    };
    virtual void merge(const core::GlobalState &from, core::GlobalState &to, core::GlobalSubstitution &subst) override {
    }
};

class LLVMSemanticExtensionProvider : public SemanticExtensionProvider {
public:
    virtual void injectOptions(cxxopts::Options &optsBuilder) const override {
        optsBuilder.add_options("compiler")("llvm-ir-folder", "Output LLVM IR to directory", cxxopts::value<string>());
        optsBuilder.add_options("compiler")("force-compiled", "Force all files to this compiled level",
                                            cxxopts::value<bool>());
    };
    virtual std::unique_ptr<SemanticExtension> readOptions(cxxopts::ParseResult &providedOptions) const override {
        optional<string> irOutputDir;
        bool forceCompiled = false;
        if (providedOptions.count("llvm-ir-folder") > 0) {
            irOutputDir = providedOptions["llvm-ir-folder"].as<string>();
        }
        if (providedOptions.count("force-compiled") > 0) {
            forceCompiled = providedOptions["force-compiled"].as<bool>();
        }
        return make_unique<LLVMSemanticExtension>(irOutputDir, forceCompiled);
    };
    virtual ~LLVMSemanticExtensionProvider(){};
};

vector<SemanticExtensionProvider *> SemanticExtensionProvider::getProviders() {
    static LLVMSemanticExtensionProvider provider;
    return {&provider};
}
} // namespace sorbet::pipeline::semantic_extension
