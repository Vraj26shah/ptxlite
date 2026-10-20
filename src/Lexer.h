// Lexer.h - turns PTX source text into tokens.
//
// PTX mnemonics carry their modifiers with dots ("mad.lo.s32"), so identifiers
// may contain '.' after the first character. The parser later splits them.
#pragma once

#include "Diagnostics.h"

#include <string>
#include <string_view>
#include <vector>

namespace ptxl {

enum class Tok {
    Eof,
    Ident,      // mad.lo.s32, LOOP, vec_add
    Directive,  // .reg, .entry, .u32
    Register,   // %r1, %tid.x
    Integer,    // 42, 0x2A
    Float,      // 1.5, 0f3F800000, 0d3FF0000000000000
    Comma, Semi, Colon, LParen, RParen, LBracket, RBracket, LBrace, RBrace,
    Less, Greater, At, Bang, Plus, Minus,
};

const char* tokName(Tok t);

struct Token {
    Tok kind;
    std::string text;
    SourceLoc loc;
};

class Lexer {
public:
    Lexer(std::string_view source, DiagEngine& diag) : src_(source), diag_(diag) {}
    std::vector<Token> tokenize();

private:
    bool atEnd() const { return pos_ >= src_.size(); }
    char peek(size_t ahead = 0) const {
        return pos_ + ahead < src_.size() ? src_[pos_ + ahead] : '\0';
    }
    char advance();
    void skipTrivia();  // whitespace and // or /* */ comments
    Token lexNumber(SourceLoc loc);

    std::string_view src_;
    DiagEngine& diag_;
    size_t pos_ = 0;
    int line_ = 1;
    int col_ = 1;
};

}  // namespace ptxl
