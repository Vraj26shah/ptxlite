// Verifier.cpp - type and operand checking for the PTX subset.
#include "Verifier.h"

#include <algorithm>
#include <set>

namespace ptxl {

namespace {
std::string describe(Type t) {
    if (t == Type::Pred) return "a predicate";
    return "a " + std::to_string(typeBits(t)) + "-bit";
}

bool sizeMatches(Type regType, Type want) {
    if (regType == Type::Pred || want == Type::Pred) return regType == want;
    return typeBits(regType) == typeBits(want);
}

bool isCompareModifier(uint32_t mods) {
    for (auto m : {Modifier::Eq, Modifier::Ne, Modifier::Lt, Modifier::Le, Modifier::Gt,
                   Modifier::Ge, Modifier::Lo, Modifier::Ls, Modifier::Hi, Modifier::Hs})
        if (mods & modBit(m)) return true;
    return false;
}
}  // namespace

bool Verifier::verify(const Module& mod) {
    std::set<std::string> names;
    for (const auto& fn : mod.functions) {
        if (!names.insert(fn.name).second)
            diag_.error(fn.loc, "redefinition of function '" + fn.name + "'");
        verifyFunction(fn);
    }
    if (mod.target.rfind("sm_", 0) != 0)
        diag_.warning({}, "unrecognised target '" + mod.target + "' (expected sm_XX)");
    return !diag_.hasErrors();
}

void Verifier::verifyFunction(const Function& fn) {
    labels_.clear();
    for (const auto& inst : fn.body) {
        if (!inst.isLabel()) continue;
        if (std::find(labels_.begin(), labels_.end(), inst.labelName()) != labels_.end())
            diag_.error(inst.loc, "redefinition of label '" + inst.labelName() + "'");
        labels_.push_back(inst.labelName());
    }
    std::set<std::string> paramNames;
    for (const auto& p : fn.params)
        if (!paramNames.insert(p.name).second)
            diag_.error(p.loc, "duplicate parameter '" + p.name + "'");

    for (const auto& inst : fn.body)
        if (!inst.isLabel()) verifyInstruction(fn, inst);

    // A kernel that falls off its end is almost always a bug.
    for (auto it = fn.body.rbegin(); it != fn.body.rend(); ++it) {
        if (it->isLabel()) continue;
        if (!opcodeInfo(it->op).isTerminator || it->guard.present())
            diag_.warning(fn.loc, "function '" + fn.name + "' does not end with ret or exit");
        break;
    }
}

const RegDecl* Verifier::lookupReg(const Function& fn, const std::string& name, SourceLoc loc) {
    const RegDecl* d = fn.findReg(name);
    if (!d) diag_.error(loc, "use of undeclared register '" + name + "'");
    return d;
}

void Verifier::checkDest(const Function& fn, const Instruction& inst, size_t idx, Type want) {
    const Operand& o = inst.ops[idx];
    if (!o.isReg()) {
        diag_.error(o.loc, std::string("destination of '") + opcodeInfo(inst.op).name +
                               "' must be a register");
        return;
    }
    const RegDecl* d = lookupReg(fn, o.name, o.loc);
    if (d && !sizeMatches(d->type, want))
        diag_.error(o.loc, "register '" + o.name + "' is ." + typeName(d->type) + " but '" +
                               formatMnemonic(inst) + "' writes " + describe(want) + " value");
}

void Verifier::checkValue(const Function& fn, const Instruction& inst, size_t idx, Type want,
                          bool allowSpecialReg) {
    const Operand& o = inst.ops[idx];
    switch (o.kind) {
        case OperandKind::Reg: {
            const RegDecl* d = lookupReg(fn, o.name, o.loc);
            if (d && !sizeMatches(d->type, want))
                diag_.error(o.loc, "register '" + o.name + "' is ." + typeName(d->type) + " but '" +
                                       formatMnemonic(inst) + "' expects " + describe(want) +
                                       " operand here");
            return;
        }
        case OperandKind::Imm: {
            if (want == Type::Pred) {
                diag_.error(o.loc, "a predicate operand cannot be an immediate");
            } else if (isFloat(want)) {
                diag_.error(o.loc, "integer immediate used with floating-point type ." +
                                       std::string(typeName(want)) +
                                       " (write it as 1.0 or 0f3F800000)");
            } else if (typeBits(want) < 64) {
                const int64_t lo = -(int64_t{1} << (typeBits(want) - 1));
                const int64_t hi = (int64_t{1} << typeBits(want)) - 1;
                if (o.imm < lo || o.imm > hi)
                    diag_.error(o.loc, "immediate " + std::to_string(o.imm) + " does not fit in ." +
                                           typeName(want));
            }
            return;
        }
        case OperandKind::FImm:
            if (!isFloat(want))
                diag_.error(o.loc, "floating-point immediate used with non-float type ." +
                                       std::string(typeName(want)));
            return;
        case OperandKind::SReg:
            if (!allowSpecialReg)
                diag_.error(o.loc, std::string("special register ") + specialRegName(o.sreg) +
                                       " can only be read with mov");
            else if (typeBits(want) != 32)
                diag_.error(o.loc, std::string("special register ") + specialRegName(o.sreg) +
                                       " is 32-bit; use a .u32 mov");
            return;
        case OperandKind::Addr:
            diag_.error(o.loc, "unexpected memory operand; only ld/st access memory");
            return;
        case OperandKind::Symbol:
            diag_.error(o.loc, "unexpected symbol '" + o.name + "'");
            return;
        case OperandKind::None:
            return;
    }
}

void Verifier::checkAddress(const Function& fn, const Instruction& inst, size_t idx) {
    const Operand& o = inst.ops[idx];
    if (o.kind != OperandKind::Addr) {
        diag_.error(o.loc, std::string("'") + opcodeInfo(inst.op).name +
                               "' expects a memory operand like [%rd1]");
        return;
    }
    if (inst.has(Modifier::Param)) {
        if (!o.addrIsSymbol)
            diag_.error(o.loc, "ld.param/st.param must name a kernel parameter, e.g. [n]");
        else if (fn.findParam(o.name) < 0)
            diag_.error(o.loc, "unknown parameter '" + o.name + "'");
        return;
    }
    if (o.addrIsSymbol) {
        diag_.error(o.loc, "symbolic address [" + o.name + "] requires the .param state space");
        return;
    }
    const RegDecl* d = lookupReg(fn, o.name, o.loc);
    if (d && typeBits(d->type) != 64)
        diag_.error(o.loc, "address register '" + o.name + "' must be 64-bit (.address_size 64)");
}

void Verifier::checkLabel(const Instruction& inst, size_t idx) {
    const Operand& o = inst.ops[idx];
    if (o.kind != OperandKind::Symbol) {
        diag_.error(o.loc, "branch target must be a label");
        return;
    }
    if (std::find(labels_.begin(), labels_.end(), o.name) == labels_.end())
        diag_.error(o.loc, "branch target '" + o.name + "' is not a label in this function");
    (void)inst;
}

void Verifier::verifyInstruction(const Function& fn, const Instruction& inst) {
    const OpcodeInfo& info = opcodeInfo(inst.op);
    const std::string mn = formatMnemonic(inst);

    if (inst.guard.present()) {
        const RegDecl* d = lookupReg(fn, inst.guard.reg, inst.loc);
        if (d && d->type != Type::Pred)
            diag_.error(inst.loc, "guard '" + inst.guard.reg + "' is not a .pred register");
    }
    if (inst.ops.size() < info.minOps || inst.ops.size() > info.maxOps) {
        diag_.error(inst.loc, std::string("'") + info.name + "' expects " +
                                  std::to_string(info.minOps) + " operand(s), got " +
                                  std::to_string(inst.ops.size()));
        return;
    }

    const bool untyped = inst.op == Opcode::Bra || inst.op == Opcode::Ret ||
                         inst.op == Opcode::Exit || inst.op == Opcode::Bar;
    if (!untyped && inst.types.empty()) {
        diag_.error(inst.loc, "'" + mn + "' needs a type suffix such as .s32 or .f32");
        return;
    }
    if (inst.op == Opcode::Cvt ? inst.types.size() != 2 : inst.types.size() > 1) {
        diag_.error(inst.loc, inst.op == Opcode::Cvt
                                  ? "cvt needs a destination and a source type, e.g. cvt.f32.s32"
                                  : "'" + mn + "' has more than one type suffix");
        return;
    }
    const Type t = inst.type();

    // Type-class restrictions.
    switch (inst.op) {
        case Opcode::And: case Opcode::Or: case Opcode::Xor: case Opcode::Not:
        case Opcode::Shl: case Opcode::Shr: case Opcode::Rem:
            if (!isInteger(t)) {
                diag_.error(inst.loc, "'" + mn + "' requires an integer type");
                return;
            }
            break;
        case Opcode::Fma:
            if (!isFloat(t)) {
                diag_.error(inst.loc, "fma requires a floating-point type");
                return;
            }
            break;
        default:
            break;
    }
    if (t == Type::F16) {
        diag_.error(inst.loc, "half precision (.f16) is not supported by ptxlite");
        return;
    }

    const bool wide = inst.has(Modifier::Wide);
    if (wide && (inst.op != Opcode::Mul && inst.op != Opcode::Mad)) {
        diag_.error(inst.loc, "'.wide' is only valid on mul and mad");
        return;
    }
    if (wide && widenType(t) == Type::None) {
        diag_.error(inst.loc, "'.wide' requires a 16- or 32-bit integer type");
        return;
    }
    if ((inst.op == Opcode::Mul || inst.op == Opcode::Mad) && isInteger(t) &&
        !(inst.has(Modifier::Lo) || inst.has(Modifier::Hi) || wide)) {
        diag_.error(inst.loc, "integer '" + mn + "' must specify .lo, .hi or .wide");
        return;
    }
    const Type wt = wide ? widenType(t) : t;

    switch (inst.op) {
        case Opcode::Mov:
            checkDest(fn, inst, 0, t);
            checkValue(fn, inst, 1, t, /*allowSpecialReg=*/true);
            break;
        case Opcode::Add: case Opcode::Sub: case Opcode::Div: case Opcode::Rem:
        case Opcode::And: case Opcode::Or: case Opcode::Xor: case Opcode::Min: case Opcode::Max:
            checkDest(fn, inst, 0, t);
            checkValue(fn, inst, 1, t);
            checkValue(fn, inst, 2, t);
            break;
        case Opcode::Mul:
            checkDest(fn, inst, 0, wt);
            checkValue(fn, inst, 1, t);
            checkValue(fn, inst, 2, t);
            break;
        case Opcode::Mad:
            checkDest(fn, inst, 0, wt);
            checkValue(fn, inst, 1, t);
            checkValue(fn, inst, 2, t);
            checkValue(fn, inst, 3, wt);
            break;
        case Opcode::Fma:
            checkDest(fn, inst, 0, t);
            for (size_t i = 1; i <= 3; ++i) checkValue(fn, inst, i, t);
            break;
        case Opcode::Not: case Opcode::Neg: case Opcode::Abs:
            checkDest(fn, inst, 0, t);
            checkValue(fn, inst, 1, t);
            break;
        case Opcode::Shl: case Opcode::Shr:
            checkDest(fn, inst, 0, t);
            checkValue(fn, inst, 1, t);
            checkValue(fn, inst, 2, Type::U32);  // shift amount is always 32-bit
            break;
        case Opcode::Setp:
            if (!isCompareModifier(inst.mods)) {
                diag_.error(inst.loc, "setp needs a comparison such as .eq, .lt or .ge");
                return;
            }
            checkDest(fn, inst, 0, Type::Pred);
            checkValue(fn, inst, 1, t);
            checkValue(fn, inst, 2, t);
            break;
        case Opcode::Selp:
            checkDest(fn, inst, 0, t);
            checkValue(fn, inst, 1, t);
            checkValue(fn, inst, 2, t);
            checkValue(fn, inst, 3, Type::Pred);
            break;
        case Opcode::Ld:
            checkDest(fn, inst, 0, t);
            checkAddress(fn, inst, 1);
            break;
        case Opcode::St:
            if (inst.has(Modifier::Param)) {
                diag_.error(inst.loc, "st.param is not supported (kernel parameters are read-only)");
                return;
            }
            checkAddress(fn, inst, 0);
            checkValue(fn, inst, 1, t);
            break;
        case Opcode::Cvt:
            checkDest(fn, inst, 0, inst.types[0]);
            checkValue(fn, inst, 1, inst.types[1]);
            break;
        case Opcode::Cvta:
            if (typeBits(t) != 64) {
                diag_.error(inst.loc, "cvta operates on 64-bit addresses (.u64)");
                return;
            }
            checkDest(fn, inst, 0, t);
            checkValue(fn, inst, 1, t);
            break;
        case Opcode::Bra:
            checkLabel(inst, 0);
            break;
        case Opcode::Bar:
            if (inst.ops[0].kind != OperandKind::Imm)
                diag_.error(inst.ops[0].loc, "bar.sync takes an immediate barrier id");
            break;
        case Opcode::Ret: case Opcode::Exit: case Opcode::Label: case Opcode::Count:
            break;
    }
}

}  // namespace ptxl
