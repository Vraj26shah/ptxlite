// Lexer.cpp - hand-written scanner (no regex, no generator: easy to debug and
// gives precise line/column information for every token).
#include "Lexer.h"

#include <cctype>

namespace ptxl {

namespace {
bool isIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$';
}
bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c == '.';
}
bool isDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }
bool isHex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }
}  // namespace

const char* tokName(Tok t) {
    switch (t) {
        case Tok::Eof: return "end of file";
        case Tok::Ident: return "identifier";
        case Tok::Directive: return "directive";
        case Tok::Register: return "register";
        case Tok::Integer: return "integer";
        case Tok::Float: return "float";
        case Tok::Comma: return "','";
        case Tok::Semi: return "';'";
        case Tok::Colon: return "':'";
        case Tok::LParen: return "'('";
        case Tok::RParen: return "')'";
        case Tok::LBracket: return "'['";
        case Tok::RBracket: return "']'";
        case Tok::LBrace: return "'{'";
        case Tok::RBrace: return "'}'";
        case Tok::Less: return "'<'";
        case Tok::Greater: return "'>'";
        case Tok::At: return "'@'";
        case Tok::Bang: return "'!'";
        case Tok::Plus: return "'+'";
        case Tok::Minus: return "'-'";
    }
    return "token";
}

char Lexer::advance() {
    char c = src_[pos_++];
    if (c == '\n') {
        ++line_;
        col_ = 1;
    } else {
        ++col_;
    }
    return c;
}

void Lexer::skipTrivia() {
    while (!atEnd()) {
        char c = peek();
        if (std::isspace(static_cast<unsigned char>(c))) {
            advance();
        } else if (c == '/' && peek(1) == '/') {
            while (!atEnd() && peek() != '\n') advance();
        } else if (c == '/' && peek(1) == '*') {
            SourceLoc start{line_, col_};
            advance();
            advance();
            bool closed = false;
            while (!atEnd()) {
                if (peek() == '*' && peek(1) == '/') {
                    advance();
                    advance();
                    closed = true;
                    break;
                }
                advance();
            }
            if (!closed) diag_.error(start, "unterminated block comment");
        } else {
            break;
        }
    }
}

Token Lexer::lexNumber(SourceLoc loc) {
    const size_t start = pos_;
    // PTX float literals: 0fXXXXXXXX (f32 bits) and 0dXXXXXXXXXXXXXXXX (f64 bits).
    if (peek() == '0' && (peek(1) == 'f' || peek(1) == 'F' || peek(1) == 'd' || peek(1) == 'D') &&
        isHex(peek(2))) {
        advance();
        advance();
        while (isHex(peek())) advance();
        return {Tok::Float, std::string(src_.substr(start, pos_ - start)), loc};
    }
    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        advance();
        advance();
        while (isHex(peek())) advance();
        return {Tok::Integer, std::string(src_.substr(start, pos_ - start)), loc};
    }
    bool isFloat = false;
    while (isDigit(peek())) advance();
    if (peek() == '.' && isDigit(peek(1))) {
        isFloat = true;
        advance();
        while (isDigit(peek())) advance();
    }
    if ((peek() == 'e' || peek() == 'E') &&
        (isDigit(peek(1)) || ((peek(1) == '+' || peek(1) == '-') && isDigit(peek(2))))) {
        isFloat = true;
        advance();
        if (peek() == '+' || peek() == '-') advance();
        while (isDigit(peek())) advance();
    }
    return {isFloat ? Tok::Float : Tok::Integer, std::string(src_.substr(start, pos_ - start)), loc};
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> toks;
    for (;;) {
        skipTrivia();
        SourceLoc loc{line_, col_};
        if (atEnd()) {
            toks.push_back({Tok::Eof, "", loc});
            return toks;
        }
        const size_t start = pos_;
        const char c = peek();

        auto word = [&](Tok kind) {
            advance();  // the leading '.', '%' or first letter
            while (isIdentChar(peek())) advance();
            toks.push_back({kind, std::string(src_.substr(start, pos_ - start)), loc});
        };

        if (c == '.' && isIdentStart(peek(1))) {
            word(Tok::Directive);
            continue;
        }
        if (c == '%' && isIdentStart(peek(1))) {
            word(Tok::Register);
            continue;
        }
        if (isIdentStart(c)) {
            word(Tok::Ident);
            continue;
        }
        if (isDigit(c)) {
            toks.push_back(lexNumber(loc));
            continue;
        }

        advance();
        Tok kind;
        switch (c) {
            case ',': kind = Tok::Comma; break;
            case ';': kind = Tok::Semi; break;
            case ':': kind = Tok::Colon; break;
            case '(': kind = Tok::LParen; break;
            case ')': kind = Tok::RParen; break;
            case '[': kind = Tok::LBracket; break;
            case ']': kind = Tok::RBracket; break;
            case '{': kind = Tok::LBrace; break;
            case '}': kind = Tok::RBrace; break;
            case '<': kind = Tok::Less; break;
            case '>': kind = Tok::Greater; break;
            case '@': kind = Tok::At; break;
            case '!': kind = Tok::Bang; break;
            case '+': kind = Tok::Plus; break;
            case '-': kind = Tok::Minus; break;
            default:
                diag_.error(loc, std::string("unexpected character '") + c + "'");
                continue;
        }
        toks.push_back({kind, std::string(1, c), loc});
    }
}

}  // namespace ptxl
