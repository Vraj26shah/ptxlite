// CFG.cpp - CFG construction and iterative liveness analysis.
#include "CFG.h"

#include <map>

namespace ptxl {

CFG CFG::build(const Function& fn) {
    CFG g;
    const size_t n = fn.body.size();
    g.blockOf.assign(n, -1);
    if (n == 0) return g;

    // 1. Find leaders: the first instruction, every label, and every
    //    instruction that follows a branch/return.
    std::vector<bool> leader(n, false);
    leader[0] = true;
    for (size_t i = 0; i < n; ++i) {
        if (fn.body[i].isLabel()) leader[i] = true;
        if (opcodeInfo(fn.body[i].op).isTerminator && i + 1 < n) leader[i + 1] = true;
    }

    // 2. Cut the instruction list into blocks.
    for (size_t i = 0; i < n; ++i) {
        if (leader[i]) {
            if (!g.blocks.empty()) g.blocks.back().end = i;
            g.blocks.push_back({i, n, {}, {}});
        }
        g.blockOf[i] = static_cast<int>(g.blocks.size()) - 1;
    }

    std::map<std::string, int> labelBlock;
    for (size_t i = 0; i < n; ++i)
        if (fn.body[i].isLabel()) labelBlock[fn.body[i].labelName()] = g.blockOf[i];

    // 3. Edges. A guarded branch/return can also fall through.
    for (size_t b = 0; b < g.blocks.size(); ++b) {
        const Instruction& last = fn.body[g.blocks[b].end - 1];
        const bool hasNext = b + 1 < g.blocks.size();
        auto& succs = g.blocks[b].succs;
        if (last.op == Opcode::Bra) {
            auto it = labelBlock.find(last.ops.empty() ? "" : last.ops[0].name);
            if (it != labelBlock.end()) succs.push_back(it->second);
            if (last.guard.present() && hasNext) succs.push_back(static_cast<int>(b + 1));
        } else if (last.op == Opcode::Ret || last.op == Opcode::Exit) {
            if (last.guard.present() && hasNext) succs.push_back(static_cast<int>(b + 1));
        } else if (hasNext) {
            succs.push_back(static_cast<int>(b + 1));
        }
    }
    for (size_t b = 0; b < g.blocks.size(); ++b)
        for (int s : g.blocks[b].succs) g.blocks[s].preds.push_back(static_cast<int>(b));
    return g;
}

namespace {
// Applies one instruction's effect backwards: live-after -> live-before.
void transfer(const Instruction& inst, Liveness::RegSet& live) {
    std::vector<std::string> uses;
    std::string def;
    collectUsesDefs(inst, uses, def);
    if (!def.empty() && !inst.guard.present()) live.erase(def);
    for (const auto& u : uses) live.insert(u);
}
}  // namespace

Liveness Liveness::compute(const Function& fn, const CFG& cfg) {
    Liveness L;
    const size_t nb = cfg.blocks.size();
    L.liveIn.assign(nb, {});
    L.liveOut.assign(nb, {});
    L.liveAfter.assign(fn.body.size(), {});

    // Iterate in reverse block order: backward problems converge fastest that way.
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t bi = nb; bi-- > 0;) {
            const BasicBlock& b = cfg.blocks[bi];
            RegSet out;
            for (int s : b.succs) out.insert(L.liveIn[s].begin(), L.liveIn[s].end());
            RegSet live = out;
            for (size_t i = b.end; i-- > b.begin;) transfer(fn.body[i], live);
            if (out != L.liveOut[bi] || live != L.liveIn[bi]) {
                L.liveOut[bi] = std::move(out);
                L.liveIn[bi] = std::move(live);
                changed = true;
            }
        }
    }

    // Final pass: record liveness after every instruction.
    for (size_t bi = 0; bi < nb; ++bi) {
        const BasicBlock& b = cfg.blocks[bi];
        RegSet live = L.liveOut[bi];
        for (size_t i = b.end; i-- > b.begin;) {
            L.liveAfter[i] = live;
            transfer(fn.body[i], live);
        }
    }
    return L;
}

}  // namespace ptxl
