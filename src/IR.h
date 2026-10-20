// IR.h - the in-memory representation of a PTX module.
//
// Design choice: the IR stays close to PTX text (one Instruction per PTX
// statement, registers referenced by name). That keeps the parser simple and
// makes every pass easy to debug, because the IR can always be printed back as
// valid PTX. Register allocation later attaches physical register numbers to
// operands without throwing the virtual names away.
#pragma once

#include "Diagnostics.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace ptxl {

// ---------------------------------------------------------------- types ----
enum class Type : uint8_t {
    None, Pred,
    B8, B16, B32, B64,
    U8, U16, U32, U64,
    S8, S16, S32, S64,
    F16, F32, F64,
};

bool parseType(std::string_view name, Type& out);  // "u32" -> Type::U32
const char* typeName(Type t);                       // Type::U32 -> "u32"
unsigned typeBits(Type t);
bool isFloat(Type t);
bool isSigned(Type t);
bool isInteger(Type t);  // b*, u*, s*
Type widenType(Type t);  // s32 -> s64 (for mul.wide); None if not widenable

// Register classes: what the allocator actually hands out. A .f32 and a .u32
// virtual register both live in the 32-bit class.
enum class RegClass : uint8_t { Pred = 0, R16 = 1, R32 = 2, R64 = 3 };
constexpr unsigned kNumRegClasses = 4;
RegClass regClassFor(Type t);
const char* physRegPrefix(RegClass c);  // "%P", "%H", "%R", "%RD"
Type physRegDeclType(RegClass c);       // type used when declaring them
std::string physRegName(RegClass c, int index);

// -------------------------------------------------------------- opcodes ----
enum class Opcode : uint8_t {
    Mov, Add, Sub, Mul, Mad, Fma, Div, Rem,
    And, Or, Xor, Not, Shl, Shr, Neg, Abs, Min, Max,
    Setp, Selp, Ld, St, Cvt, Cvta, Bra, Ret, Exit, Bar,
    Label,  // pseudo-instruction marking a branch target
    Count,
};

struct OpcodeInfo {
    const char* name;
    uint8_t minOps;
    uint8_t maxOps;
    bool hasDest;       // operand 0 is written
    bool sideEffects;   // must never be deleted by DCE
    bool isTerminator;  // ends a basic block
};

const OpcodeInfo& opcodeInfo(Opcode op);
std::optional<Opcode> lookupOpcode(std::string_view name);

// Instruction modifiers (".lo", ".global", ".ge", ...). Stored as a bitmask;
// the enum order is also the canonical print order (e.g. cvta.to.global).
enum class Modifier : uint8_t {
    To, Global, Shared, Local, Param, Const,
    Eq, Ne, Lt, Le, Gt, Ge, Lo, Ls, Hi, Hs,
    Wide, Rn, Rz, Rm, Rp, Approx, Full, Ftz, Sat, Uni, Sync,
    Count,
};
bool parseModifier(std::string_view name, Modifier& out);
const char* modifierName(Modifier m);
constexpr uint32_t modBit(Modifier m) { return 1u << static_cast<unsigned>(m); }

// Read-only hardware registers such as %tid.x.
enum class SpecialReg : uint8_t {
    TidX, TidY, TidZ, NtidX, NtidY, NtidZ,
    CtaidX, CtaidY, CtaidZ, NctaidX, NctaidY, NctaidZ,
    LaneId, WarpId,
    Count,
};
bool parseSpecialReg(std::string_view name, SpecialReg& out);
const char* specialRegName(SpecialReg r);

// ------------------------------------------------------------- operands ----
enum class OperandKind : uint8_t {
    None,
    Reg,     // %r1
    Imm,     // 42, -7, 0x10
    FImm,    // 0f3F800000, 1.5
    SReg,    // %tid.x
    Addr,    // [%rd1+8] or [param_name]
    Symbol,  // a label name (branch target)
};

struct Operand {
    OperandKind kind = OperandKind::None;
    std::string name;           // register / label / base register / param name
    int64_t imm = 0;            // Imm value, or byte offset for Addr
    double fimm = 0.0;          // FImm value
    SpecialReg sreg = SpecialReg::TidX;
    bool addrIsSymbol = false;  // [param] rather than [%rd]
    SourceLoc loc;

    // Filled in by the register allocator.
    RegClass cls = RegClass::R32;
    int phys = -1;

    bool isReg() const { return kind == OperandKind::Reg; }
    bool isRegAddr() const { return kind == OperandKind::Addr && !addrIsSymbol; }
    bool usesRegister() const { return isReg() || isRegAddr(); }

    static Operand makeImm(int64_t v) {
        Operand o;
        o.kind = OperandKind::Imm;
        o.imm = v;
        return o;
    }
};

// "@%p1" or "@!%p1" in front of an instruction: predicated execution.
struct Guard {
    std::string reg;
    bool negate = false;
    int phys = -1;
    bool present() const { return !reg.empty(); }
};

struct Instruction {
    Opcode op = Opcode::Mov;
    std::vector<Type> types;  // usually 1; cvt has 2 (dest, src)
    uint32_t mods = 0;
    Guard guard;
    std::vector<Operand> ops;
    SourceLoc loc;

    bool has(Modifier m) const { return (mods & modBit(m)) != 0; }
    Type type() const { return types.empty() ? Type::None : types.back(); }
    bool isLabel() const { return op == Opcode::Label; }
    const std::string& labelName() const { return ops[0].name; }
};

struct Param {
    std::string name;
    Type type = Type::None;
    SourceLoc loc;
};

struct RegDecl {
    std::string name;
    Type type = Type::None;
    SourceLoc loc;
};

struct Function {
    std::string name;
    bool isEntry = true;  // .entry (kernel) vs .func
    bool isVisible = false;
    SourceLoc loc;
    std::vector<Param> params;
    std::map<std::string, RegDecl> regs;
    std::vector<std::string> regOrder;  // declaration order, for printing
    std::vector<Instruction> body;

    // Set by the register allocator.
    bool allocated = false;
    std::array<unsigned, kNumRegClasses> physUsed{};

    const RegDecl* findReg(const std::string& name) const;
    int findParam(const std::string& name) const;  // -1 if missing
    size_t instructionCount() const;               // labels excluded
};

struct Module {
    std::string version = "7.8";
    std::string target = "sm_80";
    unsigned addressSize = 64;
    std::vector<Function> functions;
};

// ---------------------------------------------- integer semantics ----------
// Single source of truth for integer arithmetic, shared by the constant folder
// and the simulator, so "what the optimizer assumes" == "what we execute".
uint64_t truncateTo(uint64_t v, Type t);   // keep the low typeBits(t) bits
int64_t normalizeInt(uint64_t v, Type t);  // truncate, then sign-extend if signed
// Evaluates an integer ALU op on already-known source values. Returns nullopt
// when the result is undefined/unsupported (e.g. division by zero).
std::optional<int64_t> evalIntOp(Opcode op, uint32_t mods, Type t,
                                 const std::vector<int64_t>& src);

// ------------------------------------------------------- use / def ---------
// Registers read and the register written (empty if none) by one instruction.
// The guard predicate counts as a use.
void collectUsesDefs(const Instruction& inst, std::vector<std::string>& uses,
                     std::string& def);

// ------------------------------------------------------------ printing -----
std::string formatMnemonic(const Instruction& inst);
// `type` picks the literal form for float immediates (0f.. for f32, 0d.. otherwise).
std::string formatOperand(const Operand& op, Type type = Type::None);
std::string formatInstruction(const Instruction& inst);
void printFunction(std::ostream& os, const Function& fn);
void printModule(std::ostream& os, const Module& mod);

}  // namespace ptxl
