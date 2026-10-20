// Verifier.h - semantic checks on a parsed module.
//
// The parser accepts anything that *looks* like PTX. The verifier enforces the
// rules of the ISA subset: registers must be declared, operand sizes must match
// the instruction type, integer mul needs .lo/.hi/.wide, branch targets must
// exist, and so on. It runs again after optimization (like LLVM's
// -verify-each) to catch passes that produce invalid IR.
#pragma once

#include "IR.h"

namespace ptxl {

class Verifier {
public:
    explicit Verifier(DiagEngine& diag) : diag_(diag) {}
    bool verify(const Module& mod);  // true if no errors were found

private:
    void verifyFunction(const Function& fn);
    void verifyInstruction(const Function& fn, const Instruction& inst);

    const RegDecl* lookupReg(const Function& fn, const std::string& name, SourceLoc loc);
    void checkDest(const Function& fn, const Instruction& inst, size_t idx, Type want);
    void checkValue(const Function& fn, const Instruction& inst, size_t idx, Type want,
                    bool allowSpecialReg = false);
    void checkAddress(const Function& fn, const Instruction& inst, size_t idx);
    void checkLabel(const Instruction& inst, size_t idx);

    DiagEngine& diag_;
    std::vector<std::string> labels_;  // labels of the function being verified
};

}  // namespace ptxl
