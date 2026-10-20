// Diagnostics.h - error/warning collection with compiler-style output.
//
// Every stage (lexer, parser, verifier, register allocator) reports problems
// through a DiagEngine instead of throwing. That lets us report *all* errors in
// a file at once, the way real compilers do, rather than stopping at the first.
#pragma once

#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace ptxl {

struct SourceLoc {
    int line = 0;
    int col = 0;
};

enum class Severity { Error, Warning };

struct Diagnostic {
    Severity severity;
    SourceLoc loc;
    std::string message;
};

class DiagEngine {
public:
    explicit DiagEngine(std::string file = "<input>") : file_(std::move(file)) {}

    void error(SourceLoc loc, std::string msg) {
        diags_.push_back({Severity::Error, loc, std::move(msg)});
        ++errorCount_;
    }
    void warning(SourceLoc loc, std::string msg) {
        diags_.push_back({Severity::Warning, loc, std::move(msg)});
    }

    bool hasErrors() const { return errorCount_ > 0; }
    int errorCount() const { return errorCount_; }
    const std::vector<Diagnostic>& diagnostics() const { return diags_; }
    const std::string& file() const { return file_; }

    // Prints "file:line:col: error: message", the format editors and CI parse.
    void print(std::ostream& os) const {
        for (const auto& d : diags_) {
            os << file_;
            if (d.loc.line > 0) os << ':' << d.loc.line << ':' << d.loc.col;
            os << (d.severity == Severity::Error ? ": error: " : ": warning: ")
               << d.message << '\n';
        }
    }

    // True if any diagnostic message contains `needle` (used by tests).
    bool contains(const std::string& needle) const {
        for (const auto& d : diags_)
            if (d.message.find(needle) != std::string::npos) return true;
        return false;
    }

private:
    std::string file_;
    std::vector<Diagnostic> diags_;
    int errorCount_ = 0;
};

}  // namespace ptxl
