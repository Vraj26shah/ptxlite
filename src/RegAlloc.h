// RegAlloc.h - linear-scan register allocation.
//
// PTX uses an unlimited number of virtual registers; real GPUs have a fixed
// register file, and every register a kernel uses reduces how many warps fit
// on an SM (occupancy). The allocator maps virtual registers to as few
// physical registers as possible by reusing registers whose live ranges don't
// overlap. Same idea as ptxas reporting "Used N registers" with -v.
#pragma once

#include "IR.h"

#include <array>
#include <string>
#include <vector>

namespace ptxl {

struct LiveInterval {
    std::string reg;
    RegClass cls = RegClass::R32;
    int start = -1;  // first instruction index where the register is live
    int end = -1;    // last instruction index where it is live
    int phys = -1;   // assigned physical register number
};

struct RAResult {
    std::array<unsigned, kNumRegClasses> physUsed{};  // registers used per class
    unsigned virtualRegs = 0;                          // virtual registers actually referenced
    std::vector<LiveInterval> intervals;

    // 32-bit register equivalents: a 64-bit value occupies a register pair.
    unsigned registerCount() const {
        return physUsed[static_cast<int>(RegClass::R16)] + physUsed[static_cast<int>(RegClass::R32)] +
               2 * physUsed[static_cast<int>(RegClass::R64)];
    }
    unsigned predicateCount() const { return physUsed[static_cast<int>(RegClass::Pred)]; }
};

struct RAOptions {
    unsigned maxRegisters = 255;  // like ptxas --maxrregcount
};

// Assigns physical registers in place (Operand::phys / Guard::phys) and sets
// fn.allocated. Fails (with a diagnostic) if the budget would be exceeded,
// since spilling to local memory is not implemented.
bool allocateRegisters(Function& fn, const RAOptions& opts, DiagEngine& diag, RAResult& result);

}  // namespace ptxl
