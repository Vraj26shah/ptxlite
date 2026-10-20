// Parser.h - recursive-descent parser: tokens -> Module.
//
// Grammar (subset of PTX ISA):
//   module      := ( '.version' NUM | '.target' IDENT | '.address_size' INT | function )*
//   function    := ['.visible'] ('.entry' | '.func') IDENT [ '(' param (',' param)* ')' ]
//                  '{' ( regdecl | label | instruction )* '}'
//   param       := '.param' TYPE IDENT
//   regdecl     := '.reg' TYPE REG ['<' INT '>'] (',' REG ['<' INT '>'])* ';'
//   label       := IDENT ':'
//   instruction := [ '@' ['!'] REG ] MNEMONIC [ operand (',' operand)* ] ';'
//   operand     := REG | ['-'] INT | ['-'] FLOAT | IDENT | '[' (REG|IDENT) [('+'|'-') INT] ']'
//
// The parser only checks *syntax*. Whether "add.u64 %r1, ..." makes sense is
// the Verifier's job; keeping the two apart keeps both simple.
#pragma once

#include "IR.h"
#include "Lexer.h"

namespace ptxl {

class Parser {
public:
    Parser(std::vector<Token> tokens, DiagEngine& diag) : toks_(std::move(tokens)), diag_(diag) {}
    // Returns false if any syntax error was reported.
    bool parseModule(Module& mod);

private:
    const Token& cur() const { return toks_[pos_]; }
    const Token& lookahead(size_t n = 1) const;
    bool is(Tok k) const { return cur().kind == k; }
    bool isDirective(std::string_view d) const { return is(Tok::Directive) && cur().text == d; }
    Token consume();
    bool expect(Tok k, const char* context);
    void skipToEndOfStatement();  // error recovery: jump past the next ';'

    bool parseFunction(Module& mod, bool visible);
    bool parseParam(Function& fn);
    void parseRegDecl(Function& fn);
    void declareReg(Function& fn, const std::string& name, Type type, SourceLoc loc);
    void parseInstruction(Function& fn);
    bool parseOperand(Operand& op);
    bool parseInteger(const Token& tok, int64_t& out);
    bool parseFloat(const Token& tok, double& out);

    std::vector<Token> toks_;
    DiagEngine& diag_;
    size_t pos_ = 0;
};

}  // namespace ptxl
