// ptxlite - command-line driver (the "ptxas" of this project).
//
//   ptxlite kernel.ptx                  -> kernel.pobj
//   ptxlite kernel.ptx -v               -> also print register usage (like ptxas -v)
//   ptxlite kernel.ptx --emit=ptx       -> print register-allocated PTX
//   ptxlite kernel.ptx --emit=opt-ptx   -> print optimized PTX (virtual registers)
#include "Compiler.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {

void usage(std::ostream& os) {
    os << "usage: ptxlite <input.ptx> [options]\n"
          "  -o <file>          output file (default: <input>.pobj, or stdout for --emit=*ptx)\n"
          "  -O0 | -O1          optimization level (default -O1)\n"
          "  -v                 print per-function resource usage\n"
          "  --emit=obj         write a binary object file (default)\n"
          "  --emit=ptx         print PTX after register allocation\n"
          "  --emit=opt-ptx     print PTX after optimization, before register allocation\n"
          "  --max-regs=<n>     register budget per function (default 255)\n"
          "  -h, --help         show this help\n";
}

std::string defaultOutput(const std::string& input) {
    auto dot = input.find_last_of('.');
    auto slash = input.find_last_of("/\\");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        return input.substr(0, dot) + ".pobj";
    return input + ".pobj";
}

}  // namespace

int main(int argc, char** argv) {
    std::string input, output, emit = "obj";
    ptxl::CompileOptions opts;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            usage(std::cout);
            return 0;
        } else if (arg == "-o" && i + 1 < argc) {
            output = argv[++i];
        } else if (arg == "-O0") {
            opts.optLevel = 0;
        } else if (arg == "-O1") {
            opts.optLevel = 1;
        } else if (arg == "-v") {
            verbose = true;
        } else if (arg.rfind("--emit=", 0) == 0) {
            emit = arg.substr(7);
            if (emit != "obj" && emit != "ptx" && emit != "opt-ptx") {
                std::cerr << "ptxlite: unknown --emit kind '" << emit << "'\n";
                return 2;
            }
        } else if (arg.rfind("--max-regs=", 0) == 0) {
            try {
                opts.maxRegisters = static_cast<unsigned>(std::stoul(arg.substr(11)));
            } catch (const std::exception&) {
                std::cerr << "ptxlite: invalid --max-regs value\n";
                return 2;
            }
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "ptxlite: unknown option '" << arg << "'\n";
            usage(std::cerr);
            return 2;
        } else if (input.empty()) {
            input = arg;
        } else {
            std::cerr << "ptxlite: only one input file is supported\n";
            return 2;
        }
    }
    if (input.empty()) {
        usage(std::cerr);
        return 2;
    }

    std::ifstream in(input, std::ios::binary);
    if (!in) {
        std::cerr << "ptxlite: cannot open '" << input << "'\n";
        return 1;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string source = buffer.str();

    opts.stopBeforeRegAlloc = emit == "opt-ptx";
    ptxl::DiagEngine diag(input);
    ptxl::CompileResult result;
    const bool ok = ptxl::compileSource(source, opts, diag, result);
    diag.print(std::cerr);
    if (!ok) {
        std::cerr << diag.errorCount() << " error(s) generated.\n";
        return 1;
    }

    if (verbose) {
        for (const auto& rep : result.reports) {
            std::cerr << "ptxlite info    : Compiling function '" << rep.name << "' for '"
                      << result.module.target << "'\n";
            std::cerr << "ptxlite info    : Function properties for " << rep.name << ": "
                      << rep.instructionsAfter << " instructions (" << rep.instructionsBefore
                      << " before -O" << opts.optLevel << ")";
            if (!opts.stopBeforeRegAlloc)
                std::cerr << ", used " << rep.regalloc.registerCount() << " registers, "
                          << rep.regalloc.predicateCount() << " predicates ("
                          << rep.regalloc.virtualRegs << " virtual registers)";
            std::cerr << '\n';
        }
        std::cerr << "ptxlite info    : optimizer: " << result.stats.operandsPropagated
                  << " constants propagated, " << result.stats.instructionsFolded << " folded, "
                  << result.stats.instructionsRemoved << " dead instructions removed\n";
    }

    if (emit == "ptx" || emit == "opt-ptx") {
        if (output.empty()) {
            ptxl::printModule(std::cout, result.module);
        } else {
            std::ofstream out(output);
            ptxl::printModule(out, result.module);
        }
        return 0;
    }

    if (output.empty()) output = defaultOutput(input);
    const std::vector<uint8_t> bytes = ptxl::serializeObject(result.object);
    std::ofstream out(output, std::ios::binary);
    if (!out) {
        std::cerr << "ptxlite: cannot write '" << output << "'\n";
        return 1;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return out ? 0 : 1;
}
