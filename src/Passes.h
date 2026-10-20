// Passes.h - optimization passes over the IR.
#pragma once

#include "IR.h"

namespace ptxl {

struct PassStats {
    unsigned operandsPropagated = 0;  // register uses replaced by a known constant
    unsigned instructionsFolded = 0;  // ALU ops evaluated at compile time
    unsigned instructionsRemoved = 0; // dead instructions deleted
};

// Block-local constant propagation + folding (integer types only).
// Returns the number of changes made.
unsigned runConstantFolding(Function& fn, PassStats& stats);

// Liveness-based dead code elimination; iterates until nothing changes.
unsigned runDeadCodeElimination(Function& fn, PassStats& stats);

// -O0: nothing. -O1: fold + DCE repeated until a fixed point.
void runOptimizationPipeline(Function& fn, int optLevel, PassStats& stats);

}  // namespace ptxl
