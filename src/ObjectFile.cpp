// ObjectFile.cpp - lowering, binary serialization, validation and disassembly.
#include "ObjectFile.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

namespace ptxl {

// ============================================================ lowering ====
namespace {
ObjOperand lowerOperand(const Function& fn, const Operand& o,
                        const std::map<std::string, uint32_t>& labelIndex) {
    ObjOperand r;
    switch (o.kind) {
        case OperandKind::Reg:
            r.kind = ObjOperandKind::Reg;
            r.cls = static_cast<uint8_t>(o.cls);
            r.index = static_cast<uint16_t>(o.phys);
            break;
        case OperandKind::Imm:
            r.kind = ObjOperandKind::Imm;
            r.value = o.imm;
            break;
        case OperandKind::FImm:
            r.kind = ObjOperandKind::FImm;
            std::memcpy(&r.value, &o.fimm, sizeof r.value);
            break;
        case OperandKind::SReg:
            r.kind = ObjOperandKind::SReg;
            r.index = static_cast<uint16_t>(o.sreg);
            break;
        case OperandKind::Addr:
            if (o.addrIsSymbol) {
                r.kind = ObjOperandKind::AddrParam;
                r.index = static_cast<uint16_t>(fn.findParam(o.name));
            } else {
                r.kind = ObjOperandKind::AddrReg;
                r.cls = static_cast<uint8_t>(o.cls);
                r.index = static_cast<uint16_t>(o.phys);
            }
            r.value = o.imm;
            break;
        case OperandKind::Symbol:
            r.kind = ObjOperandKind::Target;
            r.value = labelIndex.at(o.name);
            break;
        case OperandKind::None:
            break;
    }
    return r;
}
}  // namespace

bool lowerToObject(const Module& mod, ObjModule& out, DiagEngine& diag) {
    out = ObjModule{};
    unsigned sm = 0;
    if (std::sscanf(mod.target.c_str(), "sm_%u", &sm) == 1) out.smVersion = static_cast<uint16_t>(sm);

    for (const Function& fn : mod.functions) {
        if (!fn.allocated) {
            diag.error(fn.loc, "internal error: '" + fn.name + "' was not register allocated");
            return false;
        }
        if (fn.params.size() > 255) {
            diag.error(fn.loc, "too many parameters for the object format (max 255)");
            return false;
        }
        ObjFunction of;
        of.name = fn.name;
        of.isEntry = fn.isEntry;
        of.isVisible = fn.isVisible;
        for (unsigned c = 0; c < kNumRegClasses; ++c) of.regs[c] = fn.physUsed[c];
        for (const auto& p : fn.params) of.params.push_back({p.name, static_cast<uint8_t>(p.type)});

        // Labels disappear in the binary; branches point at instruction indices.
        std::map<std::string, uint32_t> labelIndex;
        uint32_t idx = 0;
        for (const auto& inst : fn.body) {
            if (inst.isLabel())
                labelIndex[inst.labelName()] = idx;
            else
                ++idx;
        }

        for (const auto& inst : fn.body) {
            if (inst.isLabel()) continue;
            ObjInst oi;
            oi.opcode = static_cast<uint8_t>(inst.op);
            oi.type0 = inst.types.size() > 0 ? static_cast<uint8_t>(inst.types[0]) : 0;
            oi.type1 = inst.types.size() > 1 ? static_cast<uint8_t>(inst.types[1]) : 0;
            oi.numOps = static_cast<uint8_t>(inst.ops.size());
            if (inst.guard.present())
                oi.guard = static_cast<uint8_t>((inst.guard.negate ? 0x80 : 0) | (inst.guard.phys & 0x7F));
            oi.mods = inst.mods;
            for (size_t k = 0; k < inst.ops.size() && k < kMaxOperands; ++k)
                oi.ops[k] = lowerOperand(fn, inst.ops[k], labelIndex);
            of.code.push_back(oi);
            of.lines.push_back(static_cast<uint32_t>(inst.loc.line));
        }
        out.functions.push_back(std::move(of));
    }
    return true;
}

// ======================================================= serialization ====
namespace {
class ByteWriter {
public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void u16(uint16_t v) { for (int i = 0; i < 2; ++i) u8(static_cast<uint8_t>(v >> (8 * i))); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) u8(static_cast<uint8_t>(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) u8(static_cast<uint8_t>(v >> (8 * i))); }
    void patch32(size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) buf_[at + i] = static_cast<uint8_t>(v >> (8 * i));
    }
    size_t size() const { return buf_.size(); }
    std::vector<uint8_t>& bytes() { return buf_; }

private:
    std::vector<uint8_t> buf_;
};

// Bounds-checked reader: any out-of-range read sets ok=false instead of crashing.
class ByteReader {
public:
    explicit ByteReader(const std::vector<uint8_t>& b) : b_(b) {}
    bool ok() const { return ok_; }
    size_t pos() const { return pos_; }
    size_t remaining() const { return pos_ <= b_.size() ? b_.size() - pos_ : 0; }
    uint64_t read(int n) {
        if (!ok_ || remaining() < static_cast<size_t>(n)) {
            ok_ = false;
            return 0;
        }
        uint64_t v = 0;
        for (int i = 0; i < n; ++i) v |= static_cast<uint64_t>(b_[pos_++]) << (8 * i);
        return v;
    }
    uint8_t u8() { return static_cast<uint8_t>(read(1)); }
    uint16_t u16() { return static_cast<uint16_t>(read(2)); }
    uint32_t u32() { return static_cast<uint32_t>(read(4)); }
    uint64_t u64() { return read(8); }

private:
    const std::vector<uint8_t>& b_;
    size_t pos_ = 0;
    bool ok_ = true;
};

class StringTable {
public:
    uint32_t add(const std::string& s) {
        auto it = offsets_.find(s);
        if (it != offsets_.end()) return it->second;  // de-duplicate
        uint32_t off = static_cast<uint32_t>(data_.size());
        data_.insert(data_.end(), s.begin(), s.end());
        data_.push_back('\0');
        offsets_[s] = off;
        return off;
    }
    const std::vector<uint8_t>& data() const { return data_; }

private:
    std::vector<uint8_t> data_;
    std::map<std::string, uint32_t> offsets_;
};
}  // namespace

std::vector<uint8_t> serializeObject(const ObjModule& obj) {
    ByteWriter w;
    StringTable strtab;

    for (char c : kObjMagic) w.u8(static_cast<uint8_t>(c));
    w.u16(kObjVersion);
    w.u16(obj.smVersion);
    w.u32(static_cast<uint32_t>(obj.functions.size()));
    const size_t strtabOffsetPos = w.size();
    w.u32(0);  // string table offset, patched below
    w.u32(0);  // string table size, patched below
    w.u32(0);  // reserved

    for (const auto& fn : obj.functions) {
        w.u32(strtab.add(fn.name));
        w.u8(static_cast<uint8_t>((fn.isEntry ? 1 : 0) | (fn.isVisible ? 2 : 0)));
        w.u8(static_cast<uint8_t>(fn.params.size()));
        w.u16(0);
        for (uint32_t r : fn.regs) w.u32(r);
        w.u32(static_cast<uint32_t>(fn.code.size()));
        for (const auto& p : fn.params) {
            w.u32(strtab.add(p.name));
            w.u8(p.type);
            w.u8(0);
            w.u16(0);
        }
        for (const auto& inst : fn.code) {
            w.u8(inst.opcode);
            w.u8(inst.type0);
            w.u8(inst.type1);
            w.u8(inst.numOps);
            w.u8(inst.guard);
            w.u8(0);
            w.u16(0);
            w.u32(inst.mods);
            for (const auto& o : inst.ops) {
                w.u8(static_cast<uint8_t>(o.kind));
                w.u8(o.cls);
                w.u16(o.index);
                w.u32(0);
                w.u64(static_cast<uint64_t>(o.value));
            }
        }
        for (uint32_t line : fn.lines) w.u32(line);
    }

    w.patch32(strtabOffsetPos, static_cast<uint32_t>(w.size()));
    w.patch32(strtabOffsetPos + 4, static_cast<uint32_t>(strtab.data().size()));
    for (uint8_t b : strtab.data()) w.u8(b);
    return std::move(w.bytes());
}

bool deserializeObject(const std::vector<uint8_t>& bytes, ObjModule& out, std::string& error) {
    out = ObjModule{};
    ByteReader r(bytes);
    auto fail = [&](const std::string& why) {
        error = why;
        return false;
    };

    char magic[4];
    for (char& c : magic) c = static_cast<char>(r.u8());
    if (!r.ok() || std::memcmp(magic, kObjMagic, 4) != 0) return fail("not a ptxlite object (bad magic)");
    const uint16_t version = r.u16();
    if (version != kObjVersion) return fail("unsupported object format version " + std::to_string(version));
    out.smVersion = r.u16();
    const uint32_t numFunctions = r.u32();
    const uint32_t strtabOffset = r.u32();
    const uint32_t strtabSize = r.u32();
    r.u32();  // reserved
    if (!r.ok()) return fail("truncated header");
    if (static_cast<uint64_t>(strtabOffset) + strtabSize != bytes.size())
        return fail("string table does not end at end of file");
    if (strtabSize == 0 || bytes.back() != '\0') return fail("string table is not NUL-terminated");

    auto str = [&](uint32_t off, std::string& s) {
        if (off >= strtabSize) return false;
        s = reinterpret_cast<const char*>(bytes.data() + strtabOffset + off);
        return true;
    };

    for (uint32_t f = 0; f < numFunctions; ++f) {
        ObjFunction fn;
        const uint32_t nameOff = r.u32();
        const uint8_t flags = r.u8();
        const uint8_t numParams = r.u8();
        r.u16();
        for (auto& reg : fn.regs) reg = r.u32();
        const uint32_t numInsts = r.u32();
        if (!r.ok()) return fail("truncated function header");
        if (!str(nameOff, fn.name)) return fail("bad function name offset");
        fn.isEntry = flags & 1;
        fn.isVisible = flags & 2;

        // Check the counts against the bytes left *before* allocating anything:
        // a corrupt count must not make us reserve gigabytes.
        const uint64_t need = uint64_t{numParams} * 8 + uint64_t{numInsts} * (kEncodedInstSize + 4);
        if (need > r.remaining()) return fail("function '" + fn.name + "' extends past end of file");

        for (uint8_t p = 0; p < numParams; ++p) {
            ObjParam param;
            const uint32_t off = r.u32();
            param.type = r.u8();
            r.u8();
            r.u16();
            if (!str(off, param.name)) return fail("bad parameter name offset");
            fn.params.push_back(param);
        }
        fn.code.resize(numInsts);
        for (auto& inst : fn.code) {
            inst.opcode = r.u8();
            inst.type0 = r.u8();
            inst.type1 = r.u8();
            inst.numOps = r.u8();
            inst.guard = r.u8();
            r.u8();
            r.u16();
            inst.mods = r.u32();
            for (auto& o : inst.ops) {
                o.kind = static_cast<ObjOperandKind>(r.u8());
                o.cls = r.u8();
                o.index = r.u16();
                r.u32();
                o.value = static_cast<int64_t>(r.u64());
            }
            if (inst.opcode >= static_cast<uint8_t>(Opcode::Label)) return fail("invalid opcode");
            if (inst.numOps > kMaxOperands) return fail("invalid operand count");
            if (inst.type0 > static_cast<uint8_t>(Type::F64) || inst.type1 > static_cast<uint8_t>(Type::F64))
                return fail("invalid type");
        }
        fn.lines.resize(numInsts);
        for (auto& line : fn.lines) line = r.u32();
        if (!r.ok()) return fail("truncated function body");
        out.functions.push_back(std::move(fn));
    }
    if (r.pos() != strtabOffset) return fail("unexpected data before string table");
    return true;
}

// ======================================================== disassembly ====
namespace {
std::string hexFloat(int64_t bits, bool asF32) {
    char buf[32];
    if (asF32) {
        double d;
        std::memcpy(&d, &bits, sizeof d);
        float f = static_cast<float>(d);
        uint32_t u;
        std::memcpy(&u, &f, sizeof u);
        std::snprintf(buf, sizeof buf, "0f%08X", u);
    } else {
        std::snprintf(buf, sizeof buf, "0d%016llX", static_cast<unsigned long long>(bits));
    }
    return buf;
}

std::string targetLabel(int64_t index) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "L_%04lld", static_cast<long long>(index));
    return buf;
}

std::string formatObjOperand(const ObjFunction& fn, const ObjInst& inst, const ObjOperand& o) {
    const bool f32 = inst.type0 == static_cast<uint8_t>(Type::F32) ||
                     inst.type1 == static_cast<uint8_t>(Type::F32);
    auto reg = [](uint8_t cls, uint16_t index) {
        if (cls >= kNumRegClasses) return std::string("%?") + std::to_string(index);
        return physRegName(static_cast<RegClass>(cls), index);
    };
    auto offset = [](int64_t v) {
        if (v > 0) return "+" + std::to_string(v);
        if (v < 0) return std::to_string(v);
        return std::string();
    };
    switch (o.kind) {
        case ObjOperandKind::Reg: return reg(o.cls, o.index);
        case ObjOperandKind::Imm: return std::to_string(o.value);
        case ObjOperandKind::FImm: return hexFloat(o.value, f32);
        case ObjOperandKind::SReg:
            return o.index < static_cast<uint16_t>(SpecialReg::Count)
                       ? specialRegName(static_cast<SpecialReg>(o.index))
                       : "%sreg?";
        case ObjOperandKind::AddrReg: return "[" + reg(o.cls, o.index) + offset(o.value) + "]";
        case ObjOperandKind::AddrParam:
            return "[" + (o.index < fn.params.size() ? fn.params[o.index].name : "param?") +
                   offset(o.value) + "]";
        case ObjOperandKind::Target: return targetLabel(o.value);
        case ObjOperandKind::None: break;
    }
    return "<?>";
}
}  // namespace

std::string disassemble(const ObjModule& obj) {
    std::ostringstream os;
    os << "// ptxlite object, format v" << kObjVersion << ", target sm_" << obj.smVersion << ", "
       << obj.functions.size() << " function(s)\n";
    for (const auto& fn : obj.functions) {
        os << '\n' << (fn.isVisible ? ".visible " : "") << (fn.isEntry ? ".entry " : ".func ")
           << fn.name << '(';
        for (size_t i = 0; i < fn.params.size(); ++i)
            os << (i ? ", " : "") << ".param ." << typeName(static_cast<Type>(fn.params[i].type))
               << ' ' << fn.params[i].name;
        os << ")\n";
        os << "// registers:";
        for (unsigned c = 0; c < kNumRegClasses; ++c)
            os << ' ' << physRegPrefix(static_cast<RegClass>(c)) << '<' << fn.regs[c] << '>';
        os << ", " << fn.code.size() << " instructions\n";

        std::set<int64_t> targets;
        for (const auto& inst : fn.code)
            for (size_t k = 0; k < inst.numOps; ++k)
                if (inst.ops[k].kind == ObjOperandKind::Target) targets.insert(inst.ops[k].value);

        for (size_t i = 0; i <= fn.code.size(); ++i) {
            if (targets.count(static_cast<int64_t>(i))) os << targetLabel(static_cast<int64_t>(i)) << ":\n";
            if (i == fn.code.size()) break;
            const ObjInst& inst = fn.code[i];
            Instruction tmp;  // reuse the IR mnemonic printer
            tmp.op = static_cast<Opcode>(inst.opcode);
            tmp.mods = inst.mods;
            if (inst.type0) tmp.types.push_back(static_cast<Type>(inst.type0));
            if (inst.type1) tmp.types.push_back(static_cast<Type>(inst.type1));

            std::string text;
            if (inst.guard != 0xFF)
                text += std::string(inst.guard & 0x80 ? "@!" : "@") +
                        physRegName(RegClass::Pred, inst.guard & 0x7F) + " ";
            text += formatMnemonic(tmp);
            for (size_t k = 0; k < inst.numOps; ++k)
                text += (k ? ", " : " ") + formatObjOperand(fn, inst, inst.ops[k]);
            text += ';';

            char addr[16];
            std::snprintf(addr, sizeof addr, "/*%04zu*/", i);
            os << "  " << addr << "  " << text;
            if (text.size() < 44) os << std::string(44 - text.size(), ' ');
            os << "  // line " << fn.lines[i] << '\n';
        }
    }
    return os.str();
}

}  // namespace ptxl
