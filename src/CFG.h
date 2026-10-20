// CFG.h - basic blocks, control-flow graph and register liveness.
#pragma once

#include "IR.h"

#include <set>
#include <string>
#include <vector>

namespace ptxl {

// A basic block is a maximal straight-line range [begin, end) of instructions:
// control enters only at the top and leaves only at the bottom.
struct BasicBlock {
    size_t begin = 0;
    size_t end = 0;
    std::vector<int> succs;
    std::vector<int> preds;
};

struct CFG {
    std::vector<BasicBlock> blocks;
    std::vector<int> blockOf;  // instruction index -> block index

    static CFG build(const Function& fn);
};

// Classic backward dataflow:
//   liveOut(B) = union of liveIn(S) for successors S
//   liveIn(B)  = uses(B) U (liveOut(B) - defs(B))
// iterated to a fixed point. A *predicated* definition ("@%p mov %r1, 5") does
// not kill %r1, because when the guard is false the old value survives.
struct Liveness {
    using RegSet = std::set<std::string>;
    std::vector<RegSet> liveIn;     // per block
    std::vector<RegSet> liveOut;    // per block
    std::vector<RegSet> liveAfter;  // per instruction: live immediately after it

    static Liveness compute(const Function& fn, const CFG& cfg);
};

}  // namespace ptxl
