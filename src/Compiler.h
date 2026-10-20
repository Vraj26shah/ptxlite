// Compiler.h - the full pipeline as one call, shared by the CLI and the tests.
//
//   source --Lexer--> tokens --Parser--> IR --Verifier--> IR
//          --Optimizer (fold, DCE)--> IR --Verifier (again)-->
//          --Register allocator--> IR --Lowering--> object
#pragma once

#include "IR.h"
#include "ObjectFile.h"
#include "Passes.h"
#include "RegAlloc.h"

#include <string>
#include <string_view>
#include <vector>

namespace ptxl {

struct CompileOptions {
    int optLevel = 1;                 // -O0 / -O1
    unsigned maxRegisters = 255;      // --max-regs
    bool stopBeforeRegAlloc = false;  // for --emit=opt-ptx and differential tests
};

struct FunctionReport {
    std::string name;
    size_t instructionsBefore = 0;
    size_t instructionsAfter = 0;
    RAResult regalloc;
};

struct CompileResult {
    Module module;
    ObjModule object;
    PassStats stats;
    std::vector<FunctionReport> reports;
};

bool compileSource(std::string_view source, const CompileOptions& opts, DiagEngine& diag,
                   CompileResult& out);

}  // namespace ptxl
