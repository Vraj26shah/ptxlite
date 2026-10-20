// Interpreter.cpp - executes IR one instruction at a time.
//
// Registers hold raw bits (uint64_t). Each instruction reinterprets those bits
// according to its own type, which is exactly how PTX's untyped .b32/.b64
// registers behave.
#include "Interpreter.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

namespace ptxl {

void Memory::store(uint64_t addr, uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) bytes_[addr + i] = static_cast<uint8_t>(value >> (8 * i));
}

uint64_t Memory::load(uint64_t addr, unsigned bytes) const {
    uint64_t v = 0;
    for (unsigned i = 0; i < bytes; ++i) {
        auto it = bytes_.find(addr + i);
        if (it != bytes_.end()) v |= static_cast<uint64_t>(it->second) << (8 * i);
    }
    return v;
}

std::string Memory::dump() const {
    std::ostringstream os;
    for (const auto& [addr, byte] : bytes_) os << std::hex << addr << ':' << int(byte) << ' ';
    return os.str();
}

namespace {
uint64_t bitsOfF32(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
float asF32(uint64_t v) {
    uint32_t u = static_cast<uint32_t>(v);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
uint64_t bitsOfF64(double d) {
    uint64_t u;
    std::memcpy(&u, &d, 8);
    return u;
}
double asF64(uint64_t v) {
    double d;
    std::memcpy(&d, &v, 8);
    return d;
}

double toDouble(uint64_t raw, Type t) {
    if (t == Type::F32) return asF32(raw);
    if (t == Type::F64) return asF64(raw);
    if (isSigned(t)) return static_cast<double>(normalizeInt(raw, t));
    return static_cast<double>(truncateTo(raw, t));
}

uint64_t fromDouble(double d, Type t) {
    return t == Type::F32 ? bitsOfF32(static_cast<float>(d)) : bitsOfF64(d);
}

// float -> integer conversion: round toward zero, saturate, NaN -> 0 (as PTX cvt does).
uint64_t floatToInt(double d, Type t) {
    if (std::isnan(d)) return 0;
    d = std::trunc(d);
    const unsigned bits = typeBits(t);
    if (isSigned(t)) {
        const double lo = -std::ldexp(1.0, bits - 1), hi = std::ldexp(1.0, bits - 1) - 1;
        if (d <= lo) return truncateTo(static_cast<uint64_t>(static_cast<int64_t>(lo)), t);
        if (d >= hi) return truncateTo(static_cast<uint64_t>(bits == 64 ? INT64_MAX : static_cast<int64_t>(hi)), t);
        return truncateTo(static_cast<uint64_t>(static_cast<int64_t>(d)), t);
    }
    if (d <= 0) return 0;
    if (d >= std::ldexp(1.0, bits)) return bits == 64 ? UINT64_MAX : truncateTo(~uint64_t{0}, t);
    return static_cast<uint64_t>(d);
}

class Executor {
public:
    Executor(const Function& fn, const std::vector<uint64_t>& params, Memory& mem,
             const ExecConfig& cfg)
        : fn_(fn), params_(params), mem_(mem), cfg_(cfg) {
        for (size_t i = 0; i < fn.body.size(); ++i)
            if (fn.body[i].isLabel()) labels_[fn.body[i].labelName()] = i;
    }

    ExecResult run() {
        ExecResult res;
        size_t pc = 0;
        while (pc < fn_.body.size() && !done_) {
            if (++res.steps > cfg_.maxSteps) return fail(res, "step limit exceeded (infinite loop?)");
            if (!step(fn_.body[pc], pc)) return fail(res, error_);
        }
        return res;
    }

private:
    ExecResult fail(ExecResult res, const std::string& msg) {
        res.ok = false;
        res.error = msg;
        return res;
    }
    bool error(const std::string& msg) {
        error_ = msg;
        return false;
    }

    std::string key(const Operand& o) const { return o.phys >= 0 ? physRegName(o.cls, o.phys) : o.name; }

    uint64_t sreg(SpecialReg r) const {
        const unsigned i = static_cast<unsigned>(r);
        if (r <= SpecialReg::TidZ) return cfg_.tid[i];
        if (r <= SpecialReg::NtidZ) return cfg_.ntid[i - 3];
        if (r <= SpecialReg::CtaidZ) return cfg_.ctaid[i - 6];
        if (r <= SpecialReg::NctaidZ) return cfg_.nctaid[i - 9];
        if (r == SpecialReg::LaneId) return cfg_.tid[0] % 32;
        return cfg_.tid[0] / 32;  // warpid
    }

    // Raw bits of a source operand interpreted with type t.
    uint64_t value(const Operand& o, Type t) {
        switch (o.kind) {
            case OperandKind::Reg: return regs_[key(o)];
            case OperandKind::Imm: return static_cast<uint64_t>(o.imm);
            case OperandKind::FImm: return t == Type::F32 ? bitsOfF32(static_cast<float>(o.fimm)) : bitsOfF64(o.fimm);
            case OperandKind::SReg: return sreg(o.sreg);
            default: return 0;
        }
    }

    void write(const Operand& dest, uint64_t raw, Type t) { regs_[key(dest)] = truncateTo(raw, t); }

    bool guardPasses(const Instruction& inst) {
        if (!inst.guard.present()) return true;
        const std::string k = inst.guard.phys >= 0 ? physRegName(RegClass::Pred, inst.guard.phys) : inst.guard.reg;
        const bool p = regs_[k] & 1;
        return inst.guard.negate ? !p : p;
    }

    bool compare(const Instruction& inst, uint64_t ra, uint64_t rb, Type t) {
        if (isFloat(t)) {
            const double a = toDouble(ra, t), b = toDouble(rb, t);
            if (std::isnan(a) || std::isnan(b)) return false;  // ordered comparisons
            if (inst.has(Modifier::Eq)) return a == b;
            if (inst.has(Modifier::Ne)) return a != b;
            if (inst.has(Modifier::Lt)) return a < b;
            if (inst.has(Modifier::Le)) return a <= b;
            if (inst.has(Modifier::Gt)) return a > b;
            return a >= b;
        }
        const uint64_t ua = truncateTo(ra, t), ub = truncateTo(rb, t);
        const int64_t sa = normalizeInt(ra, t), sb = normalizeInt(rb, t);
        const bool sgn = isSigned(t);
        if (inst.has(Modifier::Eq)) return ua == ub;
        if (inst.has(Modifier::Ne)) return ua != ub;
        if (inst.has(Modifier::Lt)) return sgn ? sa < sb : ua < ub;
        if (inst.has(Modifier::Le)) return sgn ? sa <= sb : ua <= ub;
        if (inst.has(Modifier::Gt)) return sgn ? sa > sb : ua > ub;
        if (inst.has(Modifier::Ge)) return sgn ? sa >= sb : ua >= ub;
        if (inst.has(Modifier::Lo)) return ua < ub;
        if (inst.has(Modifier::Ls)) return ua <= ub;
        if (inst.has(Modifier::Hi)) return ua > ub;
        return ua >= ub;  // hs
    }

    bool step(const Instruction& inst, size_t& pc) {
        if (inst.isLabel() || !guardPasses(inst)) {
            ++pc;
            return true;
        }
        const Type t = inst.type();
        const auto& ops = inst.ops;

        switch (inst.op) {
            case Opcode::Ret:
            case Opcode::Exit:
                done_ = true;
                return true;
            case Opcode::Bra:
                pc = labels_.at(ops[0].name);
                return true;
            case Opcode::Bar:
                break;  // single thread: barrier is a no-op
            case Opcode::Ld: {
                if (inst.has(Modifier::Param)) {
                    const int idx = fn_.findParam(ops[1].name);
                    if (idx < 0 || static_cast<size_t>(idx) >= params_.size())
                        return error("missing value for parameter '" + ops[1].name + "'");
                    write(ops[0], params_[idx], t);
                } else {
                    write(ops[0], mem_.load(regs_[key(ops[1])] + ops[1].imm, typeBits(t) / 8), t);
                }
                break;
            }
            case Opcode::St:
                mem_.store(regs_[key(ops[0])] + ops[0].imm, value(ops[1], t), typeBits(t) / 8);
                break;
            case Opcode::Cvta:  // flat model: generic and global addresses coincide
                write(ops[0], value(ops[1], t), t);
                break;
            case Opcode::Setp:
                write(ops[0], compare(inst, value(ops[1], t), value(ops[2], t), t) ? 1 : 0, Type::Pred);
                break;
            case Opcode::Selp:
                write(ops[0], (value(ops[3], Type::Pred) & 1) ? value(ops[1], t) : value(ops[2], t), t);
                break;
            case Opcode::Cvt: {
                const Type dt = inst.types[0], st = inst.types[1];
                const uint64_t raw = value(ops[1], st);
                uint64_t out;
                if (isFloat(dt))
                    out = fromDouble(toDouble(raw, st), dt);
                else if (isFloat(st))
                    out = floatToInt(toDouble(raw, st), dt);
                else
                    out = static_cast<uint64_t>(normalizeInt(raw, st));  // sign/zero extend, then truncate
                write(ops[0], out, dt);
                break;
            }
            default:
                if (isFloat(t)) {
                    if (!floatOp(inst)) return false;
                } else {
                    std::vector<int64_t> src;
                    for (size_t k = 1; k < ops.size(); ++k)
                        src.push_back(static_cast<int64_t>(value(ops[k], k == 2 && (inst.op == Opcode::Shl || inst.op == Opcode::Shr) ? Type::U32 : t)));
                    auto r = evalIntOp(inst.op, inst.mods, t, src);
                    if (!r) return error("undefined integer operation (e.g. division by zero) at line " +
                                         std::to_string(inst.loc.line));
                    write(ops[0], static_cast<uint64_t>(*r),
                          inst.has(Modifier::Wide) ? widenType(t) : t);
                }
        }
        ++pc;
        return true;
    }

    bool floatOp(const Instruction& inst) {
        const Type t = inst.type();
        auto src = [&](size_t k) { return toDouble(value(inst.ops[k], t), t); };
        // Compute in the instruction's precision so f32 rounding matches.
        double r;
        if (t == Type::F32) {
            const float a = static_cast<float>(src(1));
            const float b = inst.ops.size() > 2 ? static_cast<float>(src(2)) : 0.0f;
            switch (inst.op) {
                case Opcode::Mov: r = a; break;
                case Opcode::Add: r = a + b; break;
                case Opcode::Sub: r = a - b; break;
                case Opcode::Mul: r = a * b; break;
                case Opcode::Div: r = a / b; break;
                case Opcode::Mad:
                case Opcode::Fma: r = std::fma(a, b, static_cast<float>(src(3))); break;
                case Opcode::Min: r = std::fmin(a, b); break;
                case Opcode::Max: r = std::fmax(a, b); break;
                case Opcode::Neg: r = -a; break;
                case Opcode::Abs: r = std::fabs(a); break;
                default: return error(std::string("unsupported float op '") + formatMnemonic(inst) + "'");
            }
        } else {
            const double a = src(1), b = inst.ops.size() > 2 ? src(2) : 0.0;
            switch (inst.op) {
                case Opcode::Mov: r = a; break;
                case Opcode::Add: r = a + b; break;
                case Opcode::Sub: r = a - b; break;
                case Opcode::Mul: r = a * b; break;
                case Opcode::Div: r = a / b; break;
                case Opcode::Mad:
                case Opcode::Fma: r = std::fma(a, b, src(3)); break;
                case Opcode::Min: r = std::fmin(a, b); break;
                case Opcode::Max: r = std::fmax(a, b); break;
                case Opcode::Neg: r = -a; break;
                case Opcode::Abs: r = std::fabs(a); break;
                default: return error(std::string("unsupported float op '") + formatMnemonic(inst) + "'");
            }
        }
        write(inst.ops[0], fromDouble(r, t), t);
        return true;
    }

    const Function& fn_;
    const std::vector<uint64_t>& params_;
    Memory& mem_;
    const ExecConfig& cfg_;
    std::map<std::string, uint64_t> regs_;
    std::map<std::string, size_t> labels_;
    std::string error_;
    bool done_ = false;
};
}  // namespace

ExecResult execute(const Function& fn, const std::vector<uint64_t>& params, Memory& mem,
                   const ExecConfig& cfg) {
    return Executor(fn, params, mem, cfg).run();
}

}  // namespace ptxl
