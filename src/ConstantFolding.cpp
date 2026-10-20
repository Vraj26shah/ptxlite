// ConstantFolding.cpp - block-local constant propagation and folding.
//
// Walks each basic block top to bottom remembering registers that currently
// hold a known integer constant ("mov.u32 %r1, 16"). Later uses of those
// registers are replaced by the immediate, and an ALU instruction whose
// sources are all immediates is evaluated now and turned into a mov.
//
//   mov.u32    %r1, 16;               mov.u32 %r1, 16;
//   mov.u32    %r2, 4;        ==>     mov.u32 %r2, 4;
//   mul.lo.u32 %r3, %r1, %r2;         mov.u32 %r3, 64;
//
// The now-unused movs are removed afterwards by DCE.
//
// Why block-local? Across blocks a register can have different values on
// different incoming paths; handling that properly needs SSA or a lattice
// dataflow (SCCP). Block-local is simple and always correct.
//
// Why not floats? Folding f32 math on the host risks differences in rounding
// modes, FTZ (flush-to-zero) and NaN payloads versus the GPU - a compiler must
// never change program results, so we leave float arithmetic alone.
#include "CFG.h"
#include "Passes.h"

#include <map>

namespace ptxl {

namespace {
bool isFoldable(Opcode op) {
    switch (op) {
        case Opcode::Mov: case Opcode::Add: case Opcode::Sub: case Opcode::Mul:
        case Opcode::Mad: case Opcode::Div: case Opcode::Rem: case Opcode::And:
        case Opcode::Or: case Opcode::Xor: case Opcode::Not: case Opcode::Shl:
        case Opcode::Shr: case Opcode::Neg: case Opcode::Abs: case Opcode::Min:
        case Opcode::Max:
            return true;
        default:
            return false;
    }
}

// Which operand positions may have a known register replaced by an immediate?
bool canSubstitute(const Instruction& inst, size_t idx) {
    if (idx == 0) return false;  // destination
    if (inst.op == Opcode::Setp) return isInteger(inst.type());
    if (inst.op == Opcode::Selp) return isInteger(inst.type()) && idx <= 2;  // not the predicate
    return isFoldable(inst.op) && isInteger(inst.type());
}
}  // namespace

unsigned runConstantFolding(Function& fn, PassStats& stats) {
    unsigned changes = 0;
    const CFG cfg = CFG::build(fn);

    for (const BasicBlock& block : cfg.blocks) {
        std::map<std::string, int64_t> known;  // register -> constant value

        for (size_t i = block.begin; i < block.end; ++i) {
            Instruction& inst = fn.body[i];
            if (inst.isLabel()) continue;

            // 1. Propagate: replace known registers with immediates.
            for (size_t k = 0; k < inst.ops.size(); ++k) {
                Operand& o = inst.ops[k];
                if (!o.isReg() || !canSubstitute(inst, k)) continue;
                auto it = known.find(o.name);
                if (it == known.end()) continue;
                SourceLoc loc = o.loc;
                o = Operand::makeImm(it->second);
                o.loc = loc;
                ++stats.operandsPropagated;
                ++changes;
            }

            // 2. Fold: every source is now an immediate -> compute it.
            if (isFoldable(inst.op) && inst.op != Opcode::Mov && isInteger(inst.type())) {
                bool allImm = true;
                std::vector<int64_t> src;
                for (size_t k = 1; k < inst.ops.size(); ++k) {
                    if (inst.ops[k].kind != OperandKind::Imm) allImm = false;
                    src.push_back(inst.ops[k].imm);
                }
                if (allImm) {
                    if (auto v = evalIntOp(inst.op, inst.mods, inst.type(), src)) {
                        const Type resultType =
                            inst.has(Modifier::Wide) ? widenType(inst.type()) : inst.type();
                        Operand dest = inst.ops[0];
                        Operand value = Operand::makeImm(*v);
                        value.loc = inst.loc;
                        inst.op = Opcode::Mov;
                        inst.mods = 0;
                        inst.types = {resultType};
                        inst.ops = {dest, value};
                        ++stats.instructionsFolded;
                        ++changes;
                    }
                }
            }

            // 3. Update what we know about the destination register.
            std::vector<std::string> uses;
            std::string def;
            collectUsesDefs(inst, uses, def);
            if (def.empty()) continue;
            if (inst.op == Opcode::Mov && !inst.guard.present() && isInteger(inst.type()) &&
                inst.ops[1].kind == OperandKind::Imm)
                known[def] = normalizeInt(static_cast<uint64_t>(inst.ops[1].imm), inst.type());
            else
                known.erase(def);  // overwritten (or only maybe-overwritten if guarded)
        }
    }
    return changes;
}

}  // namespace ptxl
