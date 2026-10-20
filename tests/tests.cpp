// tests.cpp - unit tests for every stage plus differential tests that prove
// the optimizer and register allocator preserve program behaviour.
#include "CFG.h"
#include "Compiler.h"
#include "Interpreter.h"
#include "Lexer.h"
#include "Parser.h"
#include "TestFramework.h"

#include <cstring>
#include <fstream>
#include <random>
#include <set>
#include <sstream>

using namespace ptxl;

namespace {

std::string readExample(const std::string& name) {
    std::ifstream in(std::string(PTXLITE_EXAMPLES_DIR) + "/" + name);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Wraps a kernel body in the boilerplate every test needs.
std::string kernel(const std::string& regs, const std::string& body) {
    return ".version 7.8\n.target sm_80\n.address_size 64\n"
           ".visible .entry k(.param .u64 out)\n{\n" + regs + "\n" + body + "\n}\n";
}

bool compile(const std::string& src, CompileResult& out, DiagEngine& diag, int opt = 1,
             bool stopBeforeRA = false) {
    CompileOptions opts;
    opts.optLevel = opt;
    opts.stopBeforeRegAlloc = stopBeforeRA;
    return compileSource(src, opts, diag, out);
}

std::vector<std::string> bodyText(const Function& fn) {
    std::vector<std::string> lines;
    for (const auto& inst : fn.body) lines.push_back(formatInstruction(inst));
    return lines;
}

bool hasLine(const Function& fn, const std::string& text) {
    for (const auto& line : bodyText(fn))
        if (line == text) return true;
    return false;
}

uint32_t f32bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

}  // namespace

// ================================================================== lexer ===
TEST(lexer_keeps_dotted_mnemonics_and_registers_whole) {
    DiagEngine diag;
    auto toks = Lexer("@!%p1 mad.lo.s32 %r1, %tid.x, -4; // comment", diag).tokenize();
    CHECK(!diag.hasErrors());
    CHECK_EQ(toks.size(), size_t{12});
    CHECK(toks[0].kind == Tok::At);
    CHECK(toks[1].kind == Tok::Bang);
    CHECK_EQ(toks[2].text, std::string("%p1"));
    CHECK_EQ(toks[3].text, std::string("mad.lo.s32"));
    CHECK_EQ(toks[6].text, std::string("%tid.x"));
    CHECK(toks[8].kind == Tok::Minus);
    CHECK(toks[11].kind == Tok::Eof);
}

TEST(lexer_recognises_ptx_number_formats) {
    DiagEngine diag;
    auto toks = Lexer("0f3F800000 0d3FF0000000000000 1.5 2e3 0x1F 42", diag).tokenize();
    CHECK(toks[0].kind == Tok::Float);
    CHECK(toks[1].kind == Tok::Float);
    CHECK(toks[2].kind == Tok::Float);
    CHECK(toks[3].kind == Tok::Float);
    CHECK(toks[4].kind == Tok::Integer);
    CHECK(toks[5].kind == Tok::Integer);
}

TEST(lexer_tracks_line_and_column) {
    DiagEngine diag;
    auto toks = Lexer("/* a\n b */\n   ret;", diag).tokenize();
    CHECK_EQ(toks[0].loc.line, 3);
    CHECK_EQ(toks[0].loc.col, 4);
}

TEST(lexer_reports_unterminated_comment) {
    DiagEngine diag;
    Lexer("ret; /* never closed", diag).tokenize();
    CHECK(diag.contains("unterminated block comment"));
}

// ================================================================= parser ===
TEST(parser_expands_register_ranges) {
    DiagEngine diag;
    Module mod;
    Parser p(Lexer(kernel(".reg .b32 %r<4>; .reg .pred %p;", "ret;"), diag).tokenize(), diag);
    CHECK(p.parseModule(mod));
    const Function& fn = mod.functions.at(0);
    CHECK_EQ(fn.regs.size(), size_t{5});
    CHECK(fn.findReg("%r0") && fn.findReg("%r3") && !fn.findReg("%r4"));
    CHECK(fn.findReg("%p")->type == Type::Pred);
}

TEST(parser_reads_vec_add) {
    DiagEngine diag;
    Module mod;
    Parser p(Lexer(readExample("vec_add.ptx"), diag).tokenize(), diag);
    CHECK(p.parseModule(mod));
    CHECK_EQ(mod.target, std::string("sm_80"));
    const Function& fn = mod.functions.at(0);
    CHECK_EQ(fn.name, std::string("vec_add"));
    CHECK_EQ(fn.params.size(), size_t{4});
    CHECK(fn.params[3].type == Type::U32);
    // "@%p1 bra DONE;" keeps its guard and target.
    bool foundBranch = false;
    for (const auto& inst : fn.body)
        if (inst.op == Opcode::Bra) {
            foundBranch = true;
            CHECK_EQ(inst.guard.reg, std::string("%p1"));
            CHECK_EQ(inst.ops[0].name, std::string("DONE"));
        }
    CHECK(foundBranch);
}

TEST(parser_splits_modifiers_and_types) {
    DiagEngine diag;
    Module mod;
    Parser p(Lexer(kernel(".reg .b64 %rd<2>;", "cvta.to.global.u64 %rd1, %rd0; ret;"), diag).tokenize(), diag);
    CHECK(p.parseModule(mod));
    const Instruction& inst = mod.functions[0].body[0];
    CHECK(inst.op == Opcode::Cvta);
    CHECK(inst.has(Modifier::To) && inst.has(Modifier::Global));
    CHECK(inst.type() == Type::U64);
    CHECK_EQ(formatInstruction(inst), std::string("cvta.to.global.u64 %rd1, %rd0;"));
}

TEST(parser_recovers_and_reports_multiple_errors) {
    DiagEngine diag;
    Module mod;
    Parser p(Lexer(kernel(".reg .b32 %r<2>;", "frobnicate %r1; add.s32 %r1 %r0; ret;"), diag).tokenize(), diag);
    CHECK(!p.parseModule(mod));
    CHECK(diag.contains("unknown instruction 'frobnicate'"));
    CHECK(diag.contains("expected"));
    CHECK(diag.errorCount() >= 2);
}

// =============================================================== verifier ===
TEST(verifier_rejects_size_mismatch) {
    DiagEngine diag;
    CompileResult r;
    CHECK(!compile(kernel(".reg .b32 %r<3>;", "add.u64 %r0, %r1, %r2; ret;"), r, diag));
    CHECK(diag.contains("writes a 64-bit value"));
}

TEST(verifier_rejects_undeclared_register_and_bad_label) {
    DiagEngine diag;
    CompileResult r;
    CHECK(!compile(kernel(".reg .b32 %r<2>;", "mov.u32 %r0, %r7; bra NOWHERE; ret;"), r, diag));
    CHECK(diag.contains("undeclared register '%r7'"));
    CHECK(diag.contains("'NOWHERE' is not a label"));
}

TEST(verifier_requires_lo_hi_or_wide_on_integer_mul) {
    DiagEngine diag;
    CompileResult r;
    CHECK(!compile(kernel(".reg .b32 %r<3>;", "mul.s32 %r0, %r1, %r2; ret;"), r, diag));
    CHECK(diag.contains("must specify .lo, .hi or .wide"));
}

TEST(verifier_checks_wide_destination_and_float_literals) {
    DiagEngine diag;
    CompileResult r;
    CHECK(!compile(kernel(".reg .b32 %r<3>; .reg .f32 %f;", "mul.wide.s32 %r0, %r1, 4; mov.f32 %f, 1; ret;"), r, diag));
    CHECK(diag.contains("writes a 64-bit value"));
    CHECK(diag.contains("integer immediate used with floating-point type"));
}

TEST(verifier_errors_file_reports_everything) {
    DiagEngine diag("errors.ptx");
    CompileResult r;
    CHECK(!compile(readExample("errors.ptx"), r, diag));
    CHECK_EQ(diag.errorCount(), 8);  // add.u64 alone flags all 3 operands
}

// ======================================================= CFG and liveness ===
TEST(cfg_splits_blocks_at_labels_and_branches) {
    DiagEngine diag;
    CompileResult r;
    CHECK(compile(readExample("row_sum.ptx"), r, diag, 0, true));
    const Function& fn = r.module.functions[0];
    CFG cfg = CFG::build(fn);
    CHECK_EQ(cfg.blocks.size(), size_t{3});  // entry, LOOP, STORE
    // The loop block branches back to itself and falls through to STORE.
    std::set<int> loopSuccs(cfg.blocks[1].succs.begin(), cfg.blocks[1].succs.end());
    CHECK(loopSuccs == (std::set<int>{1, 2}));
}

TEST(liveness_flows_around_loop_back_edge) {
    DiagEngine diag;
    CompileResult r;
    CHECK(compile(readExample("row_sum.ptx"), r, diag, 0, true));
    const Function& fn = r.module.functions[0];
    CFG cfg = CFG::build(fn);
    Liveness live = Liveness::compute(fn, cfg);
    // Loop-carried values must be live on entry to the loop block.
    for (const char* reg : {"%f1", "%rd6", "%r4", "%r1", "%r2", "%rd4"})
        CHECK(live.liveIn[1].count(reg) == 1);
    CHECK(live.liveIn[1].count("%f2") == 0);  // reloaded every iteration
}

TEST(liveness_guarded_definition_does_not_kill) {
    // If %p0 is false the second mov does nothing, so the first value of %r0
    // can still reach the store: the first mov must not be removed.
    DiagEngine diag;
    CompileResult r;
    const std::string src = kernel(".reg .b32 %r<2>; .reg .pred %p<1>; .reg .b64 %rd<2>;",
                                   "ld.param.u64 %rd0, [out];\n"
                                   "mov.u32 %r1, %tid.x;\n"
                                   "setp.eq.u32 %p0, %r1, 0;\n"
                                   "mov.u32 %r0, 1;\n"
                                   "@%p0 mov.u32 %r0, 2;\n"
                                   "st.global.u32 [%rd0], %r0;\n"
                                   "ret;");
    CHECK(compile(src, r, diag, 1, true));
    const Function& fn = r.module.functions[0];
    CHECK(hasLine(fn, "mov.u32 %r0, 1;"));
    CHECK(hasLine(fn, "@%p0 mov.u32 %r0, 2;"));
}

// ============================================================ optimizations ===
TEST(constfold_folds_chain_and_dce_removes_dead_code) {
    DiagEngine diag;
    CompileResult r;
    CHECK(compile(readExample("const_fold.ptx"), r, diag, 1, true));
    const Function& fn = r.module.functions[0];
    CHECK(hasLine(fn, "add.u32 %r7, %r6, 280;"));
    for (const auto& line : bodyText(fn)) {
        CHECK(line.find("%r8") == std::string::npos);  // dead chain removed
        CHECK(line.find("%r1,") == std::string::npos); // folded movs removed
    }
    CHECK_EQ(fn.instructionCount(), size_t{8});
    CHECK(r.stats.instructionsFolded >= 3);
}

TEST(constfold_respects_32bit_wraparound_and_signedness) {
    CHECK_EQ(*evalIntOp(Opcode::Add, 0, Type::U32, {0xFFFFFFFF, 1}), 0);
    CHECK_EQ(*evalIntOp(Opcode::Add, 0, Type::S32, {0x7FFFFFFF, 1}), int64_t{INT32_MIN});
    CHECK_EQ(*evalIntOp(Opcode::Shr, 0, Type::S32, {-16, 2}), -4);           // arithmetic
    CHECK_EQ(*evalIntOp(Opcode::Shr, 0, Type::U32, {0xFFFFFFF0, 2}), 0x3FFFFFFC); // logical
    CHECK_EQ(*evalIntOp(Opcode::Shl, 0, Type::B32, {1, 40}), 0);              // PTX clamps
    CHECK_EQ(*evalIntOp(Opcode::Shr, 0, Type::S32, {-1, 99}), -1);
    CHECK_EQ(*evalIntOp(Opcode::Mul, modBit(Modifier::Wide), Type::S32, {-2, 0x40000000}),
             int64_t{-0x80000000LL});
    CHECK_EQ(*evalIntOp(Opcode::Mul, modBit(Modifier::Hi), Type::U32, {0x80000000, 4}), 2);
    CHECK_EQ(*evalIntOp(Opcode::Min, 0, Type::S32, {-1, 1}), -1);
    CHECK_EQ(*evalIntOp(Opcode::Min, 0, Type::U32, {0xFFFFFFFF, 1}), 1);
    CHECK(!evalIntOp(Opcode::Div, 0, Type::S32, {5, 0}).has_value());       // never fold UB
}

TEST(constfold_leaves_float_math_alone) {
    DiagEngine diag;
    CompileResult r;
    const std::string src = kernel(".reg .f32 %f<3>; .reg .b64 %rd<1>;",
                                   "ld.param.u64 %rd0, [out];\n"
                                   "mov.f32 %f0, 0f3F800000;\n"
                                   "mov.f32 %f1, 0f40000000;\n"
                                   "add.f32 %f2, %f0, %f1;\n"
                                   "st.global.f32 [%rd0], %f2;\n"
                                   "ret;");
    CHECK(compile(src, r, diag, 1, true));
    CHECK(r.module.functions[0].body[3].op == Opcode::Add);
}

TEST(dce_keeps_stores_and_branches) {
    DiagEngine diag;
    CompileResult r;
    CHECK(compile(readExample("vec_add.ptx"), r, diag, 1, true));
    const Function& fn = r.module.functions[0];
    int stores = 0, branches = 0;
    for (const auto& inst : fn.body) {
        stores += inst.op == Opcode::St;
        branches += inst.op == Opcode::Bra;
    }
    CHECK_EQ(stores, 1);
    CHECK_EQ(branches, 1);
}

// ====================================================== register allocation ===
TEST(regalloc_reuses_registers_for_disjoint_live_ranges) {
    // Ten temporaries but at most two are ever live at once.
    std::string body = "ld.param.u64 %rd0, [out];\nmov.u32 %r0, %tid.x;\n";
    for (int i = 1; i < 10; ++i)
        body += "add.u32 %r" + std::to_string(i) + ", %r" + std::to_string(i - 1) + ", %r" +
                std::to_string(i - 1) + ";\n";
    body += "st.global.u32 [%rd0], %r9;\nret;";
    DiagEngine diag;
    CompileResult r;
    CHECK(compile(kernel(".reg .b32 %r<10>; .reg .b64 %rd<1>;", body), r, diag));
    const RAResult& ra = r.reports[0].regalloc;
    CHECK_EQ(ra.virtualRegs, 11u);
    CHECK(ra.physUsed[static_cast<int>(RegClass::R32)] <= 2u);
}

// Strongest RA test: at every program point, all simultaneously live virtual
// registers must have been given *different* physical registers.
TEST(regalloc_never_assigns_one_register_to_two_live_values) {
    for (const char* file : {"vec_add.ptx", "saxpy.ptx", "row_sum.ptx", "const_fold.ptx"}) {
        DiagEngine diag;
        CompileResult r;
        CHECK(compile(readExample(file), r, diag, 1, true));
        Function fn = r.module.functions[0];
        CFG cfg = CFG::build(fn);
        Liveness live = Liveness::compute(fn, cfg);
        RAResult ra;
        CHECK(allocateRegisters(fn, RAOptions{}, diag, ra));
        std::map<std::string, std::pair<int, int>> where;
        for (const auto& li : ra.intervals) where[li.reg] = {static_cast<int>(li.cls), li.phys};

        for (size_t i = 0; i < fn.body.size(); ++i) {
            std::vector<std::string> uses;
            std::string def;
            collectUsesDefs(fn.body[i], uses, def);
            std::set<std::string> regs = live.liveAfter[i];
            if (!def.empty()) regs.insert(def);
            std::set<std::pair<int, int>> seen;
            for (const auto& reg : regs) CHECK(seen.insert(where.at(reg)).second);
        }
    }
}

TEST(regalloc_enforces_register_budget) {
    DiagEngine diag;
    CompileResult r;
    CompileOptions opts;
    opts.maxRegisters = 2;
    CHECK(!compileSource(readExample("vec_add.ptx"), opts, diag, r));
    CHECK(diag.contains("spilling to local memory is not implemented"));
}

// ============================================================ object file ===
TEST(object_roundtrip_preserves_everything) {
    for (const char* file : {"vec_add.ptx", "saxpy.ptx", "row_sum.ptx", "const_fold.ptx"}) {
        DiagEngine diag;
        CompileResult r;
        CHECK(compile(readExample(file), r, diag));
        std::vector<uint8_t> bytes = serializeObject(r.object);
        ObjModule back;
        std::string err;
        CHECK(deserializeObject(bytes, back, err));
        CHECK_EQ(disassemble(back), disassemble(r.object));
        CHECK(serializeObject(back) == bytes);  // byte-identical re-encode
    }
}

TEST(object_reader_rejects_corrupt_input) {
    DiagEngine diag;
    CompileResult r;
    CHECK(compile(readExample("vec_add.ptx"), r, diag));
    const std::vector<uint8_t> good = serializeObject(r.object);
    ObjModule out;
    std::string err;

    std::vector<uint8_t> badMagic = good;
    badMagic[0] = 'X';
    CHECK(!deserializeObject(badMagic, out, err));

    for (size_t cut : {size_t{3}, size_t{20}, good.size() / 2, good.size() - 1}) {
        std::vector<uint8_t> truncated(good.begin(), good.begin() + static_cast<long>(cut));
        CHECK(!deserializeObject(truncated, out, err));
    }

    std::vector<uint8_t> hugeCount = good;  // numInsts of function 0 -> 0xFFFFFFFF
    const size_t numInstsOffset = 24 + 4 + 4 + 16;
    for (int i = 0; i < 4; ++i) hugeCount[numInstsOffset + i] = 0xFF;
    CHECK(!deserializeObject(hugeCount, out, err));
}

TEST(object_keeps_debug_line_numbers) {
    DiagEngine diag;
    CompileResult r;
    CHECK(compile(readExample("vec_add.ptx"), r, diag));
    const ObjFunction& fn = r.object.functions[0];
    CHECK_EQ(fn.lines.front(), 23u);  // ld.param.u64 %rd1, [a];
    CHECK_EQ(fn.lines.back(), 46u);   // ret;
}

TEST(printed_ptx_reparses_to_same_program) {
    for (const char* file : {"vec_add.ptx", "row_sum.ptx"}) {
        DiagEngine diag;
        CompileResult r;
        CHECK(compile(readExample(file), r, diag));
        std::ostringstream printed;
        printModule(printed, r.module);  // physical registers %R0, %RD1, ...
        DiagEngine diag2;
        CompileResult r2;
        CHECK(compile(printed.str(), r2, diag2, 0));
        diag2.print(std::cout);
        CHECK_EQ(r2.module.functions[0].instructionCount(), r.module.functions[0].instructionCount());
    }
}

// =========================================================== differential ===
// Run the same kernel unoptimized (virtual registers) and fully compiled
// (-O1 + register allocation) and demand identical memory afterwards.
namespace {
void runBoth(const std::string& src, const std::vector<uint64_t>& params, const Memory& initial,
             unsigned threads) {
    DiagEngine d0, d1;
    CompileResult ref, opt;
    CHECK(compile(src, ref, d0, 0, true));
    CHECK(compile(src, opt, d1, 1, false));
    if (d0.hasErrors() || d1.hasErrors()) {
        d0.print(std::cout);
        d1.print(std::cout);
        return;
    }
    Memory m0 = initial, m1 = initial;
    for (unsigned t = 0; t < threads; ++t) {
        ExecConfig cfg;
        cfg.tid = {t, 0, 0};
        cfg.ntid = {threads, 1, 1};
        ExecResult e0 = execute(ref.module.functions[0], params, m0, cfg);
        ExecResult e1 = execute(opt.module.functions[0], params, m1, cfg);
        CHECK(e0.ok);
        CHECK(e1.ok);
        if (!e0.ok) std::cout << "    reference failed: " << e0.error << '\n';
        if (!e1.ok) std::cout << "    optimized failed: " << e1.error << '\n';
    }
    CHECK(m0 == m1);
}
}  // namespace

TEST(simulator_computes_vec_add_correctly) {
    DiagEngine diag;
    CompileResult r;
    CHECK(compile(readExample("vec_add.ptx"), r, diag));
    Memory mem;
    for (int i = 0; i < 4; ++i) {
        mem.store(0x1000 + 4 * i, f32bits(1.5f * i), 4);
        mem.store(0x2000 + 4 * i, f32bits(10.0f), 4);
    }
    for (uint32_t t = 0; t < 6; ++t) {  // 6 threads, n = 4: two must do nothing
        ExecConfig cfg;
        cfg.tid = {t, 0, 0};
        cfg.ntid = {6, 1, 1};
        CHECK(execute(r.module.functions[0], {0x1000, 0x2000, 0x3000, 4}, mem, cfg).ok);
    }
    for (int i = 0; i < 4; ++i) CHECK_EQ(mem.load(0x3000 + 4 * i, 4), uint64_t{f32bits(1.5f * i + 10.0f)});
    CHECK_EQ(mem.load(0x3000 + 16, 4), uint64_t{0});  // thread 4 was out of range
}

TEST(differential_examples) {
    Memory mem;
    for (int i = 0; i < 16; ++i) {
        mem.store(0x1000 + 4 * i, f32bits(0.25f * i - 1.0f), 4);
        mem.store(0x2000 + 4 * i, f32bits(3.0f + i), 4);
    }
    runBoth(readExample("vec_add.ptx"), {0x1000, 0x2000, 0x3000, 10}, mem, 12);
    runBoth(readExample("saxpy.ptx"), {10, f32bits(2.5f), 0x1000, 0x2000}, mem, 12);
    runBoth(readExample("row_sum.ptx"), {0x1000, 0x3000, 4}, mem, 4);
    runBoth(readExample("const_fold.ptx"), {0x3000}, mem, 8);
}

// Random straight-line integer programs: a cheap fuzzer for the optimizer.
TEST(differential_random_programs) {
    std::mt19937 rng(20240601);
    auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
    const char* ops[] = {"add", "sub", "mul.lo", "and", "or", "xor", "shl", "shr", "min", "max", "div", "rem"};
    const char* types[] = {"s32", "u32"};

    for (int prog = 0; prog < 300; ++prog) {
        std::ostringstream body;
        body << "ld.param.u64 %rd0, [out];\ncvta.to.global.u64 %rd1, %rd0;\n";
        for (int k = 0; k < 8; ++k) {
            if (pick(3) == 0)
                body << "mov.u32 %r" << k << ", %tid.x;\n";
            else
                body << "mov.u32 %r" << k << ", " << (pick(4) == 0 ? rng() : rng() % 64) << ";\n";
        }
        for (int n = 0; n < 25; ++n) {
            const std::string op = ops[pick(12)];
            const std::string ty = (op == "and" || op == "or" || op == "xor" || op == "shl" || op == "shr")
                                       ? (pick(2) ? "b32" : types[pick(2)])
                                       : types[pick(2)];
            auto src = [&]() -> std::string {
                return pick(3) == 0 ? std::to_string(rng() % 100) : "%r" + std::to_string(pick(8));
            };
            std::string a = src(), b = src();
            if (op == "shl" || op == "shr") b = pick(2) ? std::to_string(pick(40)) : b;
            if (op == "div" || op == "rem") b = std::to_string(1 + pick(9));  // never divide by zero
            if (pick(6) == 0) {  // sometimes a predicated instruction
                body << "setp.lt." << types[pick(2)] << " %p0, %r" << pick(8) << ", %r" << pick(8) << ";\n@"
                     << (pick(2) ? "!" : "") << "%p0 ";
            }
            body << op << '.' << ty << " %r" << pick(8) << ", " << a << ", " << b << ";\n";
        }
        for (int k = 0; k < 8; ++k)
            if (pick(4) != 0) body << "st.global.u32 [%rd1+" << 4 * k << "], %r" << k << ";\n";
        body << "ret;";

        const std::string src =
            kernel(".reg .b32 %r<8>; .reg .b64 %rd<2>; .reg .pred %p<1>;", body.str());
        const int before = testing::failures();
        runBoth(src, {0x4000}, Memory{}, 2);
        if (testing::failures() != before) {
            std::cout << "    failing program #" << prog << ":\n" << src << '\n';
            return;
        }
    }
}

int main() { return testing::runAll(); }
