// Parser.cpp - recursive-descent parser with statement-level error recovery.
#include "Parser.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

namespace ptxl {

const Token& Parser::lookahead(size_t n) const {
    return toks_[std::min(pos_ + n, toks_.size() - 1)];
}

Token Parser::consume() {
    Token t = cur();
    if (pos_ + 1 < toks_.size()) ++pos_;  // never move past Eof
    return t;
}

bool Parser::expect(Tok k, const char* context) {
    if (is(k)) {
        consume();
        return true;
    }
    std::string found = is(Tok::Eof) ? "end of file" : "'" + cur().text + "'";
    diag_.error(cur().loc, std::string("expected ") + tokName(k) + " " + context + ", found " + found);
    return false;
}

void Parser::skipToEndOfStatement() {
    while (!is(Tok::Eof) && !is(Tok::Semi) && !is(Tok::RBrace)) consume();
    if (is(Tok::Semi)) consume();
}

bool Parser::parseInteger(const Token& tok, int64_t& out) {
    try {
        size_t used = 0;
        const bool hex = tok.text.size() > 2 && (tok.text[1] == 'x' || tok.text[1] == 'X');
        unsigned long long v = std::stoull(tok.text, &used, hex ? 16 : 10);
        if (used != tok.text.size()) throw std::invalid_argument("trailing");
        out = static_cast<int64_t>(v);  // values >= 2^63 keep their bit pattern
        return true;
    } catch (const std::exception&) {
        diag_.error(tok.loc, "invalid integer literal '" + tok.text + "'");
        return false;
    }
}

bool Parser::parseFloat(const Token& tok, double& out) {
    const std::string& t = tok.text;
    if (t.size() > 2 && (t[1] == 'f' || t[1] == 'F')) {
        if (t.size() != 10) {
            diag_.error(tok.loc, "'" + t + "': 0f literals need exactly 8 hex digits");
            return false;
        }
        uint32_t bits = static_cast<uint32_t>(std::stoul(t.substr(2), nullptr, 16));
        float f;
        std::memcpy(&f, &bits, sizeof f);
        out = f;
        return true;
    }
    if (t.size() > 2 && (t[1] == 'd' || t[1] == 'D')) {
        if (t.size() != 18) {
            diag_.error(tok.loc, "'" + t + "': 0d literals need exactly 16 hex digits");
            return false;
        }
        uint64_t bits = std::stoull(t.substr(2), nullptr, 16);
        std::memcpy(&out, &bits, sizeof out);
        return true;
    }
    try {
        out = std::stod(t);
        return true;
    } catch (const std::exception&) {
        diag_.error(tok.loc, "invalid floating-point literal '" + t + "'");
        return false;
    }
}

// ================================================================ module ===
bool Parser::parseModule(Module& mod) {
    while (!is(Tok::Eof)) {
        if (isDirective(".version")) {
            consume();
            if (is(Tok::Float) || is(Tok::Integer))
                mod.version = consume().text;
            else
                expect(Tok::Float, "after .version");
            continue;
        }
        if (isDirective(".target")) {
            consume();
            if (is(Tok::Ident))
                mod.target = consume().text;
            else
                expect(Tok::Ident, "after .target");
            while (is(Tok::Comma)) {  // ".target sm_80, debug" - extra options ignored
                consume();
                if (is(Tok::Ident)) consume();
            }
            continue;
        }
        if (isDirective(".address_size")) {
            Token d = consume();
            int64_t v = 0;
            if (is(Tok::Integer) && parseInteger(consume(), v)) {
                mod.addressSize = static_cast<unsigned>(v);
                if (v != 64) diag_.error(d.loc, "only .address_size 64 is supported");
            } else {
                expect(Tok::Integer, "after .address_size");
            }
            continue;
        }
        bool visible = false;
        if (isDirective(".visible")) {
            consume();
            visible = true;
        }
        if (isDirective(".entry") || isDirective(".func")) {
            if (!parseFunction(mod, visible)) {
                // Skip the rest of a broken function header/body.
                while (!is(Tok::Eof) && !is(Tok::RBrace)) consume();
                if (is(Tok::RBrace)) consume();
            }
            continue;
        }
        diag_.error(cur().loc, "unexpected '" + cur().text + "' at module scope");
        consume();
    }
    return !diag_.hasErrors();
}

// ============================================================== function ===
bool Parser::parseFunction(Module& mod, bool visible) {
    Function fn;
    fn.loc = cur().loc;
    fn.isEntry = consume().text == ".entry";
    fn.isVisible = visible;

    if (is(Tok::LParen)) {
        diag_.error(cur().loc, ".func return parameters are not supported");
        return false;
    }
    if (!is(Tok::Ident)) {
        expect(Tok::Ident, "for function name");
        return false;
    }
    fn.name = consume().text;

    if (is(Tok::LParen)) {
        consume();
        if (!is(Tok::RParen)) {
            do {
                if (!parseParam(fn)) return false;
            } while (is(Tok::Comma) && (consume(), true));
        }
        if (!expect(Tok::RParen, "to close the parameter list")) return false;
    }
    if (!expect(Tok::LBrace, "to open the function body")) return false;

    while (!is(Tok::RBrace) && !is(Tok::Eof)) {
        if (isDirective(".reg")) {
            parseRegDecl(fn);
        } else if (is(Tok::Ident) && lookahead().kind == Tok::Colon) {
            Instruction label;
            label.op = Opcode::Label;
            label.loc = cur().loc;
            Operand name;
            name.kind = OperandKind::Symbol;
            name.loc = cur().loc;
            name.name = consume().text;
            consume();  // ':'
            label.ops.push_back(name);
            fn.body.push_back(std::move(label));
        } else {
            parseInstruction(fn);
        }
    }
    expect(Tok::RBrace, "to close the function body");
    mod.functions.push_back(std::move(fn));
    return true;
}

bool Parser::parseParam(Function& fn) {
    if (!isDirective(".param")) {
        expect(Tok::Directive, "('.param') in parameter list");
        return false;
    }
    Param p;
    p.loc = consume().loc;
    if (!is(Tok::Directive) || !parseType(std::string_view(cur().text).substr(1), p.type)) {
        diag_.error(cur().loc, "expected a parameter type such as .u64");
        return false;
    }
    consume();
    // Optional attributes like ".ptr .global .align 4" are accepted and ignored.
    while (is(Tok::Directive)) {
        consume();
        if (is(Tok::Integer)) consume();
    }
    if (!is(Tok::Ident)) {
        expect(Tok::Ident, "for parameter name");
        return false;
    }
    p.name = consume().text;
    fn.params.push_back(std::move(p));
    return true;
}

void Parser::declareReg(Function& fn, const std::string& name, Type type, SourceLoc loc) {
    if (fn.regs.count(name)) {
        diag_.error(loc, "redefinition of register '" + name + "'");
        return;
    }
    fn.regs.emplace(name, RegDecl{name, type, loc});
    fn.regOrder.push_back(name);
}

void Parser::parseRegDecl(Function& fn) {
    consume();  // .reg
    Type type;
    if (!is(Tok::Directive) || !parseType(std::string_view(cur().text).substr(1), type)) {
        diag_.error(cur().loc, "expected a register type such as .b32 after .reg");
        skipToEndOfStatement();
        return;
    }
    consume();
    do {
        if (!is(Tok::Register)) {
            expect(Tok::Register, "in .reg declaration");
            skipToEndOfStatement();
            return;
        }
        Token reg = consume();
        if (is(Tok::Less)) {  // %r<6> declares %r0 .. %r5
            consume();
            int64_t count = 0;
            if (!is(Tok::Integer) || !parseInteger(consume(), count) || count <= 0 || count > 65536) {
                diag_.error(reg.loc, "register range count must be an integer in 1..65536");
                skipToEndOfStatement();
                return;
            }
            if (!expect(Tok::Greater, "to close register range")) {
                skipToEndOfStatement();
                return;
            }
            for (int64_t i = 0; i < count; ++i)
                declareReg(fn, reg.text + std::to_string(i), type, reg.loc);
        } else {
            declareReg(fn, reg.text, type, reg.loc);
        }
    } while (is(Tok::Comma) && (consume(), true));
    if (!expect(Tok::Semi, "after .reg declaration")) skipToEndOfStatement();
}

// =========================================================== instruction ===
void Parser::parseInstruction(Function& fn) {
    Instruction inst;
    inst.loc = cur().loc;

    if (is(Tok::At)) {  // predicate guard
        consume();
        if (is(Tok::Bang)) {
            consume();
            inst.guard.negate = true;
        }
        if (!is(Tok::Register)) {
            expect(Tok::Register, "for guard predicate");
            skipToEndOfStatement();
            return;
        }
        inst.guard.reg = consume().text;
    }

    if (!is(Tok::Ident)) {
        diag_.error(cur().loc, "expected an instruction, found '" + cur().text + "'");
        if (is(Tok::Directive) || is(Tok::Register)) consume();
        skipToEndOfStatement();
        return;
    }

    // Split "mad.lo.s32" into opcode + modifiers + types.
    Token mn = consume();
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= mn.text.size(); ++i) {
        if (i == mn.text.size() || mn.text[i] == '.') {
            parts.push_back(mn.text.substr(start, i - start));
            start = i + 1;
        }
    }
    auto op = lookupOpcode(parts[0]);
    if (!op) {
        diag_.error(mn.loc, "unknown instruction '" + parts[0] + "'");
        skipToEndOfStatement();
        return;
    }
    inst.op = *op;
    for (size_t i = 1; i < parts.size(); ++i) {
        Type t;
        Modifier m;
        if (parts[i].empty()) {
            diag_.error(mn.loc, "empty modifier in '" + mn.text + "'");
        } else if (parseType(parts[i], t)) {
            inst.types.push_back(t);
        } else if (parseModifier(parts[i], m)) {
            if (inst.mods & modBit(m))
                diag_.error(mn.loc, "duplicate modifier '." + parts[i] + "'");
            inst.mods |= modBit(m);
        } else {
            diag_.error(mn.loc, "unknown modifier '." + parts[i] + "' on '" + parts[0] + "'");
        }
    }

    if (!is(Tok::Semi)) {
        do {
            Operand o;
            if (!parseOperand(o)) {
                skipToEndOfStatement();
                return;
            }
            inst.ops.push_back(std::move(o));
        } while (is(Tok::Comma) && (consume(), true));
    }
    if (!expect(Tok::Semi, "at end of instruction")) {
        skipToEndOfStatement();
        return;
    }
    fn.body.push_back(std::move(inst));
}

bool Parser::parseOperand(Operand& o) {
    o.loc = cur().loc;
    switch (cur().kind) {
        case Tok::Register: {
            Token r = consume();
            if (parseSpecialReg(r.text, o.sreg)) {
                o.kind = OperandKind::SReg;
            } else {
                o.kind = OperandKind::Reg;
                o.name = r.text;
            }
            return true;
        }
        case Tok::Minus:
        case Tok::Integer:
        case Tok::Float: {
            bool negative = false;
            if (is(Tok::Minus)) {
                consume();
                negative = true;
            }
            if (is(Tok::Integer)) {
                int64_t v;
                if (!parseInteger(consume(), v)) return false;
                o.kind = OperandKind::Imm;
                o.imm = negative ? static_cast<int64_t>(0 - static_cast<uint64_t>(v)) : v;
                return true;
            }
            if (is(Tok::Float)) {
                double d;
                if (!parseFloat(consume(), d)) return false;
                o.kind = OperandKind::FImm;
                o.fimm = negative ? -d : d;
                return true;
            }
            diag_.error(cur().loc, "expected a number after '-'");
            return false;
        }
        case Tok::Ident:
            o.kind = OperandKind::Symbol;
            o.name = consume().text;
            return true;
        case Tok::LBracket: {
            consume();
            o.kind = OperandKind::Addr;
            if (is(Tok::Register)) {
                o.name = consume().text;
            } else if (is(Tok::Ident)) {
                o.name = consume().text;
                o.addrIsSymbol = true;
            } else {
                diag_.error(cur().loc, "expected a register or parameter name inside [...]");
                return false;
            }
            if (is(Tok::Plus) || is(Tok::Minus)) {
                bool negative = consume().kind == Tok::Minus;
                int64_t off;
                if (!is(Tok::Integer)) {
                    diag_.error(cur().loc, "expected an integer address offset");
                    return false;
                }
                if (!parseInteger(consume(), off)) return false;
                o.imm = negative ? -off : off;
            }
            return expect(Tok::RBracket, "to close the address");
        }
        default:
            diag_.error(cur().loc, "expected an operand, found '" + cur().text + "'");
            return false;
    }
}

}  // namespace ptxl
