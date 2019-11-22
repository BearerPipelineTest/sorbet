#ifndef COMPILER_NAMES_H
#define COMPILER_NAMES_H

#include "core/NameRef.h"

namespace sorbet::compiler {
class Names {
public:
    static core::NameRef defineTopClassOrModule(const core::GlobalState &gs);
    static core::NameRef defineMethod(const core::GlobalState &gs);
    static core::NameRef defineMethodSingleton(const core::GlobalState &gs);

    static void init(core::GlobalState &gs);
};
} // namespace sorbet::compiler
#endif
