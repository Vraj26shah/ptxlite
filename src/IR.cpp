// IR.cpp - tables, integer semantics and the PTX printer.
#include "IR.h"

#include <cctype>
#include <cstring>
#include <iomanip>
#include <set>
#include <sstream>

namespace ptxl {

// ================================================================ types ====
namespace {
struct TypeEntry {
    const char* name;
    Type type;
    unsigned bits;
};
constexpr TypeEntry kTypes[] = {
    {"pred", Type::Pred, 1},
    {"b8", Type::B8, 8},   {"b16", Type::B16, 16}, {"b32", Type::B32, 32}, {"b64", Type::B64, 64},
    {"u8", Type::U8, 8},   {"u16", Type::U16, 16}, {"u32", Type::U32, 32}, {"u64", Type::U64, 64},
    {"s8", Type::S8, 8},   {"s16", Type::S16, 16}, {"s32", Type::S32, 32}, {"s64", Type::S64, 64},
    {"f16", Type::F16, 16}, {"f32", Type::F32, 32}, {"f64", Type::F64, 64},
};
}  // namespace

bool parseType(std::string_view name, Type& out) {
    for (const auto& e : kTypes)
        if (name == e.name) {
            out = e.type;
            return true;
        }
    return false;
}

const char* typeName(Type t) {
    for (const auto& e : kTypes)
        if (e.type == t) return e.name;
    return "none";
}

unsigned typeBits(Type t) {
    for (const auto& e : kTypes)
        if (e.type == t) return e.bits;
    return 0;
}

bool isFloat(Type t) { return t == Type::F16 || t == Type::F32 || t == Type::F64; }
bool isSigned(Type t) { return t == Type::S8 || t == Type::S16 || t == Type::S32 || t == Type::S64; }
bool isInteger(Type t) { return t != Type::None && t != Type::Pred && !isFloat(t); }

Type widenType(Type t) {
    switch (t) {
        case Type::U16: return Type::U32;
        case Type::U32: return Type::U64;
        case Type::S16: return Type::S32;
        case Type::S32: return Type::S64;
        case Type::B16: return Type::B32;
        case Type::B32: return Type::B64;
        default: return Type::None;
    }
}

RegClass regClassFor(Type t) {
    if (t == Type::Pred) return RegClass::Pred;
    switch (typeBits(t)) {
        case 8:
        case 16: return RegClass::R16;
        case 64: return RegClass::R64;
        default: return RegClass::R32;
    }
}

const char* physRegPrefix(RegClass c) {
    switch (c) {
        case RegClass::Pred: return "%P";
        case RegClass::R16: return "%H";
        case RegClass::R32: return "%R";
        case RegClass::R64: return "%RD";
    }
    return "%?";
}

Type physRegDeclType(RegClass c) {
    switch (c) {
        case RegClass::Pred: return Type::Pred;
        case RegClass::R16: return Type::B16;
        case RegClass::R32: return Type::B32;
        case RegClass::R64: return Type::B64;
    }
    return Type::None;
}

std::string physRegName(RegClass c, int index) {
    return std::string(physRegPrefix(c)) + std::to_string(index);
}

// ============================================================== opcodes ====
namespace {
// Keep in the same order as enum class Opcode.
//                         name     min max dest   side   term
constexpr OpcodeInfo kOpcodes[] = {
    {"mov", 2, 2, true, false, false},
    {"add", 3, 3, true, false, false},
    {"sub", 3, 3, true, false, false},
    {"mul", 3, 3, true, false, false},
    {"mad", 4, 4, true, false, false},
    {"fma", 4, 4, true, false, false},
    {"div", 3, 3, true, false, false},
    {"rem", 3, 3, true, false, false},
    {"and", 3, 3, true, false, false},
    {"or", 3, 3, true, false, false},
    {"xor", 3, 3, true, false, false},
    {"not", 2, 2, true, false, false},
    {"shl", 3, 3, true, false, false},
    {"shr", 3, 3, true, false, false},
    {"neg", 2, 2, true, false, false},
    {"abs", 2, 2, true, false, false},
    {"min", 3, 3, true, false, false},
    {"max", 3, 3, true, false, false},
    {"setp", 3, 3, true, false, false},
    {"selp", 4, 4, true, false, false},
    {"ld", 2, 2, true, false, false},
    {"st", 2, 2, false, true, false},
    {"cvt", 2, 2, true, false, false},
    {"cvta", 2, 2, true, false, false},
    {"bra", 1, 1, false, true, true},
    {"ret", 0, 0, false, true, true},
    {"exit", 0, 0, false, true, true},
    {"bar", 1, 1, false, true, false},
    {"<label>", 1, 1, false, true, false},
};
static_assert(sizeof(kOpcodes) / sizeof(kOpcodes[0]) == static_cast<size_t>(Opcode::Count),
              "opcode table out of sync with enum");

constexpr const char* kModifierNames[] = {
    "to", "global", "shared", "local", "param", "const",
    "eq", "ne", "lt", "le", "gt", "ge", "lo", "ls", "hi", "hs",
    "wide", "rn", "rz", "rm", "rp", "approx", "full", "ftz", "sat", "uni", "sync",
};
static_assert(sizeof(kModifierNames) / sizeof(kModifierNames[0]) ==
                  static_cast<size_t>(Modifier::Count),
              "modifier table out of sync with enum");
static_assert(static_cast<unsigned>(Modifier::Count) <= 32, "modifiers must fit in a uint32_t");

constexpr const char* kSpecialRegNames[] = {
    "%tid.x",   "%tid.y",   "%tid.z",   "%ntid.x",   "%ntid.y",   "%ntid.z",   "%ctaid.x",
    "%ctaid.y", "%ctaid.z", "%nctaid.x", "%nctaid.y", "%nctaid.z", "%laneid", "%warpid",
};
static_assert(sizeof(kSpecialRegNames) / sizeof(kSpecialRegNames[0]) ==
                  static_cast<size_t>(SpecialReg::Count),
              "special register table out of sync with enum");
}  // namespace

const OpcodeInfo& opcodeInfo(Opcode op) { return kOpcodes[static_cast<size_t>(op)]; }

std::optional<Opcode> lookupOpcode(std::string_view name) {
    for (size_t i = 0; i < static_cast<size_t>(Opcode::Label); ++i)
        if (name == kOpcodes[i].name) return static_cast<Opcode>(i);
    return std::nullopt;
}

bool parseModifier(std::string_view name, Modifier& out) {
    for (size_t i = 0; i < static_cast<size_t>(Modifier::Count); ++i)
        if (name == kModifierNames[i]) {
            out = static_cast<Modifier>(i);
            return true;
        }
    return false;
}

const char* modifierName(Modifier m) { return kModifierNames[static_cast<size_t>(m)]; }

bool parseSpecialReg(std::string_view name, SpecialReg& out) {
    for (size_t i = 0; i < static_cast<size_t>(SpecialReg::Count); ++i)
        if (name == kSpecialRegNames[i]) {
            out = static_cast<SpecialReg>(i);
            return true;
        }
    return false;
}

const char* specialRegName(SpecialReg r) { return kSpecialRegNames[static_cast<size_t>(r)]; }

// ============================================================ function ====
const RegDecl* Function::findReg(const std::string& n) const {
    auto it = regs.find(n);
    return it == regs.end() ? nullptr : &it->second;
}

int Function::findParam(const std::string& n) const {
    for (size_t i = 0; i < params.size(); ++i)
        if (params[i].name == n) return static_cast<int>(i);
    return -1;
}

size_t Function::instructionCount() const {
    size_t n = 0;
    for (const auto& inst : body)
        if (!inst.isLabel()) ++n;
    return n;
}

// =================================================== integer semantics ====
uint64_t truncateTo(uint64_t v, Type t) {
    unsigned bits = typeBits(t);
    if (bits == 0 || bits >= 64) return v;
    return v & ((uint64_t{1} << bits) - 1);
}

int64_t normalizeInt(uint64_t v, Type t) {
    unsigned bits = typeBits(t);
    v = truncateTo(v, t);
    if (isSigned(t) && bits < 64 && (v >> (bits - 1)) & 1) v |= ~((uint64_t{1} << bits) - 1);
    return static_cast<int64_t>(v);
}

std::optional<int64_t> evalIntOp(Opcode op, uint32_t mods, Type t,
                                 const std::vector<int64_t>& s) {
    if (!isInteger(t)) return std::nullopt;
    const unsigned bits = typeBits(t);
    const bool sgn = isSigned(t);
    auto N = [&](uint64_t v) { return normalizeInt(v, t); };   // value as typed
    auto U = [&](int64_t v) { return truncateTo(static_cast<uint64_t>(v), t); };  // raw bits
    auto need = [&](size_t n) { return s.size() >= n; };
    const bool wide = mods & modBit(Modifier::Wide);
    const bool hi = mods & modBit(Modifier::Hi);

    switch (op) {
        case Opcode::Mov:
            if (!need(1)) return std::nullopt;
            return N(static_cast<uint64_t>(s[0]));
        case Opcode::Add:
            if (!need(2)) return std::nullopt;
            return N(static_cast<uint64_t>(s[0]) + static_cast<uint64_t>(s[1]));
        case Opcode::Sub:
            if (!need(2)) return std::nullopt;
            return N(static_cast<uint64_t>(s[0]) - static_cast<uint64_t>(s[1]));
        case Opcode::Mul:
        case Opcode::Mad: {
            if (!need(op == Opcode::Mad ? 3 : 2)) return std::nullopt;
            const int64_t a = N(static_cast<uint64_t>(s[0]));
            const int64_t b = N(static_cast<uint64_t>(s[1]));
            if (wide) {
                // Operands are sign/zero extended to 2x width, so the product fits.
                Type wt = widenType(t);
                if (wt == Type::None) return std::nullopt;
                uint64_t p = static_cast<uint64_t>(a) * static_cast<uint64_t>(b);
                if (op == Opcode::Mad) p += static_cast<uint64_t>(s[2]);
                return normalizeInt(p, wt);
            }
            if (hi) {
                __extension__ typedef __int128 i128;
                __extension__ typedef unsigned __int128 u128;
                uint64_t high;
                if (sgn)
                    high = static_cast<uint64_t>((static_cast<i128>(a) * static_cast<i128>(b)) >> bits);
                else
                    high = static_cast<uint64_t>((static_cast<u128>(U(a)) * static_cast<u128>(U(b))) >> bits);
                if (op == Opcode::Mad) high += static_cast<uint64_t>(s[2]);
                return N(high);
            }
            uint64_t p = static_cast<uint64_t>(a) * static_cast<uint64_t>(b);
            if (op == Opcode::Mad) p += static_cast<uint64_t>(s[2]);
            return N(p);
        }
        case Opcode::Div:
        case Opcode::Rem: {
            if (!need(2)) return std::nullopt;
            if (sgn) {
                const int64_t a = N(static_cast<uint64_t>(s[0])), b = N(static_cast<uint64_t>(s[1]));
                if (b == 0) return std::nullopt;
                if (bits == 64 && a == INT64_MIN && b == -1) return std::nullopt;
                return N(static_cast<uint64_t>(op == Opcode::Div ? a / b : a % b));
            }
            const uint64_t a = U(s[0]), b = U(s[1]);
            if (b == 0) return std::nullopt;
            return N(op == Opcode::Div ? a / b : a % b);
        }
        case Opcode::And:
            if (!need(2)) return std::nullopt;
            return N(static_cast<uint64_t>(s[0] & s[1]));
        case Opcode::Or:
            if (!need(2)) return std::nullopt;
            return N(static_cast<uint64_t>(s[0] | s[1]));
        case Opcode::Xor:
            if (!need(2)) return std::nullopt;
            return N(static_cast<uint64_t>(s[0] ^ s[1]));
        case Opcode::Not:
            if (!need(1)) return std::nullopt;
            return N(~static_cast<uint64_t>(s[0]));
        case Opcode::Neg:
            if (!need(1)) return std::nullopt;
            return N(0 - static_cast<uint64_t>(s[0]));
        case Opcode::Abs: {
            if (!need(1)) return std::nullopt;
            const int64_t a = N(static_cast<uint64_t>(s[0]));
            return N(a < 0 ? 0 - static_cast<uint64_t>(a) : static_cast<uint64_t>(a));
        }
        case Opcode::Shl: {
            // PTX: shift amounts larger than the operand width are clamped,
            // so the result is 0 (C++ would be undefined behaviour here!).
            if (!need(2)) return std::nullopt;
            const uint64_t amt = static_cast<uint32_t>(s[1]);
            if (amt >= bits) return 0;
            return N(static_cast<uint64_t>(s[0]) << amt);
        }
        case Opcode::Shr: {
            if (!need(2)) return std::nullopt;
            const uint64_t amt = static_cast<uint32_t>(s[1]);
            if (sgn) {  // arithmetic shift: fills with the sign bit
                const int64_t a = N(static_cast<uint64_t>(s[0]));
                if (amt >= bits) return a < 0 ? -1 : 0;
                return N(static_cast<uint64_t>(a >> amt));
            }
            const uint64_t a = U(s[0]);  // .u and .b: logical shift
            if (amt >= bits) return 0;
            return N(a >> amt);
        }
        case Opcode::Min:
        case Opcode::Max: {
            if (!need(2)) return std::nullopt;
            bool pickFirst;
            if (sgn) {
                const int64_t a = N(static_cast<uint64_t>(s[0])), b = N(static_cast<uint64_t>(s[1]));
                pickFirst = op == Opcode::Min ? a <= b : a >= b;
            } else {
                const uint64_t a = U(s[0]), b = U(s[1]);
                pickFirst = op == Opcode::Min ? a <= b : a >= b;
            }
            return N(static_cast<uint64_t>(pickFirst ? s[0] : s[1]));
        }
        default:
            return std::nullopt;
    }
}

// ============================================================ use / def ===
void collectUsesDefs(const Instruction& inst, std::vector<std::string>& uses,
                     std::string& def) {
    uses.clear();
    def.clear();
    if (inst.isLabel()) return;
    const bool hasDest = opcodeInfo(inst.op).hasDest;
    for (size_t i = 0; i < inst.ops.size(); ++i) {
        const Operand& o = inst.ops[i];
        if (i == 0 && hasDest && o.isReg()) {
            def = o.name;
            continue;
        }
        if (o.usesRegister()) uses.push_back(o.name);
    }
    if (inst.guard.present()) uses.push_back(inst.guard.reg);
}

// ============================================================= printing ===
std::string formatMnemonic(const Instruction& inst) {
    std::string s = opcodeInfo(inst.op).name;
    for (size_t m = 0; m < static_cast<size_t>(Modifier::Count); ++m)
        if (inst.mods & (1u << m)) {
            s += '.';
            s += kModifierNames[m];
        }
    for (Type t : inst.types) {
        s += '.';
        s += typeName(t);
    }
    return s;
}

namespace {
std::string regText(const std::string& name, RegClass cls, int phys) {
    return phys >= 0 ? physRegName(cls, phys) : name;
}

// Hex forms ("0f3F800000", "0d3FF0000000000000") round-trip exactly.
std::string formatFloat(double d, Type type) {
    if (type == Type::F32) {
        const float f = static_cast<float>(d);
        uint32_t bits32;
        std::memcpy(&bits32, &f, sizeof bits32);
        std::ostringstream os;
        os << "0f" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << bits32;
        return os.str();
    }
    uint64_t bits;
    std::memcpy(&bits, &d, sizeof bits);
    std::ostringstream os;
    os << "0d" << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << bits;
    return os.str();
}
}  // namespace

std::string formatOperand(const Operand& o, Type type) {
    switch (o.kind) {
        case OperandKind::Reg: return regText(o.name, o.cls, o.phys);
        case OperandKind::Imm: return std::to_string(o.imm);
        case OperandKind::FImm: return formatFloat(o.fimm, type);
        case OperandKind::SReg: return specialRegName(o.sreg);
        case OperandKind::Symbol: return o.name;
        case OperandKind::Addr: {
            std::string s = "[";
            s += o.addrIsSymbol ? o.name : regText(o.name, o.cls, o.phys);
            if (o.imm > 0) s += "+" + std::to_string(o.imm);
            if (o.imm < 0) s += std::to_string(o.imm);
            return s + "]";
        }
        case OperandKind::None: break;
    }
    return "<none>";
}

std::string formatInstruction(const Instruction& inst) {
    if (inst.isLabel()) return inst.labelName() + ":";
    std::string s;
    if (inst.guard.present()) {
        s += inst.guard.negate ? "@!" : "@";
        s += regText(inst.guard.reg, RegClass::Pred, inst.guard.phys);
        s += ' ';
    }
    s += formatMnemonic(inst);
    for (size_t i = 0; i < inst.ops.size(); ++i) {
        s += i == 0 ? " " : ", ";
        const Type t = (inst.op == Opcode::Cvt && i == 1) ? inst.types[1] : inst.type();
        s += formatOperand(inst.ops[i], t);
    }
    return s + ";";
}

namespace {
// Groups "%r0..%r5" back into "%r<6>" when the numbering is dense.
void printVirtualRegDecls(std::ostream& os, const Function& fn) {
    struct Group {
        Type type;
        std::string prefix;
        std::set<int> numbers;
    };
    std::vector<Group> groups;
    std::vector<const RegDecl*> singles;
    for (const auto& name : fn.regOrder) {
        const RegDecl& d = fn.regs.at(name);
        size_t p = name.size();
        while (p > 0 && std::isdigit(static_cast<unsigned char>(name[p - 1]))) --p;
        if (p == name.size() || p == 0 || name.size() - p > 6) {
            singles.push_back(&d);
            continue;
        }
        std::string prefix = name.substr(0, p);
        int num = std::stoi(name.substr(p));
        Group* g = nullptr;
        for (auto& cand : groups)
            if (cand.type == d.type && cand.prefix == prefix) g = &cand;
        if (!g) {
            groups.push_back({d.type, prefix, {}});
            g = &groups.back();
        }
        g->numbers.insert(num);
    }
    for (const auto& g : groups) {
        const int maxNum = *g.numbers.rbegin();
        if (static_cast<int>(g.numbers.size()) == maxNum + 1) {
            os << "\t.reg ." << typeName(g.type) << ' ' << g.prefix << '<' << maxNum + 1 << ">;\n";
        } else {
            for (int n : g.numbers)
                os << "\t.reg ." << typeName(g.type) << ' ' << g.prefix << n << ";\n";
        }
    }
    for (const RegDecl* d : singles) os << "\t.reg ." << typeName(d->type) << ' ' << d->name << ";\n";
}
}  // namespace

void printFunction(std::ostream& os, const Function& fn) {
    if (fn.isVisible) os << ".visible ";
    os << (fn.isEntry ? ".entry " : ".func ") << fn.name << "(";
    for (size_t i = 0; i < fn.params.size(); ++i) {
        os << (i ? ",\n" : "\n") << "\t.param ." << typeName(fn.params[i].type) << ' '
           << fn.params[i].name;
    }
    os << (fn.params.empty() ? ")\n{\n" : "\n)\n{\n");
    if (fn.allocated) {
        for (unsigned c = 0; c < kNumRegClasses; ++c) {
            if (!fn.physUsed[c]) continue;
            auto rc = static_cast<RegClass>(c);
            os << "\t.reg ." << typeName(physRegDeclType(rc)) << ' ' << physRegPrefix(rc) << '<'
               << fn.physUsed[c] << ">;\n";
        }
    } else {
        printVirtualRegDecls(os, fn);
    }
    os << '\n';
    for (const auto& inst : fn.body) {
        if (inst.isLabel())
            os << formatInstruction(inst) << '\n';
        else
            os << '\t' << formatInstruction(inst) << '\n';
    }
    os << "}\n";
}

void printModule(std::ostream& os, const Module& mod) {
    os << ".version " << mod.version << '\n'
       << ".target " << mod.target << '\n'
       << ".address_size " << mod.addressSize << '\n';
    for (const auto& fn : mod.functions) {
        os << '\n';
        printFunction(os, fn);
    }
}

}  // namespace ptxl
