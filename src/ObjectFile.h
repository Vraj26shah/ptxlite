// ObjectFile.h - the ".pobj" binary object format, writer, reader, disassembler.
//
// Real ptxas emits an ELF "cubin" containing SASS machine code, symbol tables
// and DWARF debug info. ptxlite emits a much simpler container with the same
// *kinds* of content:
//
//   +-----------------------------+
//   | header  "PTXL", version, sm |
//   | function records:           |
//   |   name, flags, reg counts   |
//   |   params                    |
//   |   code   (fixed 76-byte     |
//   |           instructions)     |
//   |   line table (debug info)   |
//   | string table                |
//   +-----------------------------+
//
// All integers are little-endian and written byte by byte, so the file is
// identical no matter which host produced it. The reader validates every
// offset and count, because object files are untrusted input.
#pragma once

#include "IR.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ptxl {

constexpr char kObjMagic[4] = {'P', 'T', 'X', 'L'};
constexpr uint16_t kObjVersion = 1;
constexpr size_t kMaxOperands = 4;
constexpr size_t kEncodedInstSize = 12 + kMaxOperands * 16;  // 76 bytes

// Stable on-disk operand kinds (decoupled from the in-memory enum on purpose,
// so the IR can evolve without breaking old object files).
enum class ObjOperandKind : uint8_t {
    None = 0,
    Reg = 1,        // cls + index
    Imm = 2,        // value
    FImm = 3,       // value = IEEE-754 double bits
    SReg = 4,       // index = SpecialReg
    AddrReg = 5,    // [reg + value]
    AddrParam = 6,  // [param #index + value]
    Target = 7,     // branch target: value = instruction index
};

struct ObjOperand {
    ObjOperandKind kind = ObjOperandKind::None;
    uint8_t cls = 0;
    uint16_t index = 0;
    int64_t value = 0;
};

struct ObjInst {
    uint8_t opcode = 0;
    uint8_t type0 = 0;
    uint8_t type1 = 0;
    uint8_t numOps = 0;
    uint8_t guard = 0xFF;  // 0xFF: none; else bit7 = negate, bits0-6 = predicate register
    uint32_t mods = 0;
    std::array<ObjOperand, kMaxOperands> ops{};
};

struct ObjParam {
    std::string name;
    uint8_t type = 0;
};

struct ObjFunction {
    std::string name;
    bool isEntry = true;
    bool isVisible = false;
    std::array<uint32_t, kNumRegClasses> regs{};
    std::vector<ObjParam> params;
    std::vector<ObjInst> code;
    std::vector<uint32_t> lines;  // source line of each instruction
};

struct ObjModule {
    uint16_t smVersion = 0;
    std::vector<ObjFunction> functions;
};

// Module (after register allocation) -> object model.
bool lowerToObject(const Module& mod, ObjModule& out, DiagEngine& diag);
std::vector<uint8_t> serializeObject(const ObjModule& obj);
bool deserializeObject(const std::vector<uint8_t>& bytes, ObjModule& out, std::string& error);
std::string disassemble(const ObjModule& obj);

}  // namespace ptxl
