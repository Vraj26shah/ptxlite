// Compiler.cpp - drives the pipeline stage by stage, stopping at the first
// stage that reports errors (later stages assume earlier invariants hold).
#include "Compiler.h"

#include "Lexer.h"
#include "Parser.h"
#include "Verifier.h"

namespace ptxl {

bool compileSource(std::string_view source, const CompileOptions& opts, DiagEngine& diag,
                   CompileResult& out) {
    out = CompileResult{};

    Lexer lexer(source, diag);
    std::vector<Token> tokens = lexer.tokenize();
    if (diag.hasErrors()) return false;

    Parser parser(std::move(tokens), diag);
    if (!parser.parseModule(out.module)) return false;

    if (!Verifier(diag).verify(out.module)) return false;

    for (Function& fn : out.module.functions) {
        FunctionReport rep;
        rep.name = fn.name;
        rep.instructionsBefore = fn.instructionCount();
        runOptimizationPipeline(fn, opts.optLevel, out.stats);
        rep.instructionsAfter = fn.instructionCount();
        out.reports.push_back(rep);
    }

    // Re-verify: an optimizer bug should surface as a clear internal error,
    // not as a corrupt object file.
    if (opts.optLevel > 0) {
        DiagEngine inner(diag.file());
        if (!Verifier(inner).verify(out.module)) {
            diag.error({}, "internal error: optimizer produced invalid IR");
            for (const auto& d : inner.diagnostics())
                if (d.severity == Severity::Error) diag.error(d.loc, "  " + d.message);
            return false;
        }
    }
    if (opts.stopBeforeRegAlloc) return true;

    RAOptions ra;
    ra.maxRegisters = opts.maxRegisters;
    for (size_t i = 0; i < out.module.functions.size(); ++i)
        if (!allocateRegisters(out.module.functions[i], ra, diag, out.reports[i].regalloc))
            return false;

    return lowerToObject(out.module, out.object, diag);
}

}  // namespace ptxl
