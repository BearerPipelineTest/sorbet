#ifndef SORBET_COMPILER_PASSES_H
#define SORBET_COMPILER_PASSES_H
#include <string_view>
#include <vector>

namespace llvm {
class ModulePass;
}; // namespace llvm
namespace sorbet::compiler {
class Passes {
public:
    static llvm::ModulePass *createDeleteUnusedSorbetIntrinsticsPass();
    static const std::vector<llvm::ModulePass *> standardLowerings();
};
} // namespace sorbet::compiler
#endif
