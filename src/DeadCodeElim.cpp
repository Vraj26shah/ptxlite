// DeadCodeElim.cpp - remove instructions whose results are never used.
//
// An instruction is dead if (a) it has no side effects (not st/bra/ret/bar)
// and (b) the register it writes is not live afterwards. Deleting one dead
// instruction can make its inputs dead too, so we iterate to a fixed point.
#include "CFG.h"
#include "Passes.h"

namespace ptxl {

unsigned runDeadCodeElimination(Function& fn, PassStats& stats) {
    unsigned removed = 0;
    for (;;) {
        const CFG cfg = CFG::build(fn);
        const Liveness live = Liveness::compute(fn, cfg);

        std::vector<bool> dead(fn.body.size(), false);
        bool any = false;
        for (size_t i = 0; i < fn.body.size(); ++i) {
            const Instruction& inst = fn.body[i];
            if (inst.isLabel()) continue;
            const OpcodeInfo& info = opcodeInfo(inst.op);
            if (!info.hasDest || info.sideEffects || inst.ops.empty() || !inst.ops[0].isReg())
                continue;
            const std::string& dest = inst.ops[0].name;
            const bool selfMove = inst.op == Opcode::Mov && inst.ops[1].isReg() &&
                                  inst.ops[1].name == dest;
            if (selfMove || live.liveAfter[i].count(dest) == 0) {
                dead[i] = true;
                any = true;
            }
        }
        if (!any) break;

        std::vector<Instruction> kept;
        kept.reserve(fn.body.size());
        for (size_t i = 0; i < fn.body.size(); ++i) {
            if (dead[i])
                ++removed;
            else
                kept.push_back(std::move(fn.body[i]));
        }
        fn.body = std::move(kept);
    }
    stats.instructionsRemoved += removed;
    return removed;
}

void runOptimizationPipeline(Function& fn, int optLevel, PassStats& stats) {
    if (optLevel <= 0) return;
    // Folding exposes dead code; removing code can't expose new folds in our
    // block-local scheme, but we loop anyway (bounded) so future passes compose.
    for (int iter = 0; iter < 8; ++iter) {
        unsigned changes = runConstantFolding(fn, stats);
        changes += runDeadCodeElimination(fn, stats);
        if (changes == 0) break;
    }
}

}  // namespace ptxl
