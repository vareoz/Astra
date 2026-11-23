#include "js/astra_js.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace astra {

namespace {

enum class TokKind {
    Eof, Number, String, Ident,
    KwLet, KwConst, KwVar, KwFunction, KwReturn, KwIf, KwElse, KwWhile, KwFor,
    KwTrue, KwFalse, KwNull,
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Comma, Semi, Dot, Colon,
    Plus, Minus, Star, Slash, Percent, Bang,
    Assign, Eq, Neq, Lt, Le, Gt, Ge, AndAnd, OrOr
};

struct Token {
    TokKind kind = TokKind::Eof;
    std::string text;
    double num = 0.0;
};

std::vector<Token> tokenize(std::string_view src) {
    std::vector<Token> out;
    size_t i = 0, n = src.size();
    while (i < n) {
        char c = src[i];
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            while (i < n && src[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            size_t end = src.find("*/", i + 2);
            i = (end == std::string_view::npos) ? n : end + 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            char q = c;
            ++i;
            std::string s;
            while (i < n && src[i] != q) {
                if (src[i] == '\\' && i + 1 < n) {
                    char esc = src[++i];
                    if (esc == 'n') s.push_back('\n');
                    else if (esc == 't') s.push_back('\t');
                    else s.push_back(esc);
                    ++i;
                } else {
                    s.push_back(src[i++]);
                }
            }
            if (i < n) ++i;
            out.push_back({TokKind::String, std::move(s), 0.0});
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t st = i;
            while (i < n && (std::isdigit(static_cast<unsigned char>(src[i])) || src[i] == '.')) ++i;
            std::string txt(src.substr(st, i - st));
            out.push_back({TokKind::Number, txt, std::strtod(txt.c_str(), nullptr)});
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
            size_t st = i;
            while (i < n && (std::isalnum(static_cast<unsigned char>(src[i])) || src[i] == '_' || src[i] == '$')) ++i;
            std::string id(src.substr(st, i - st));
            TokKind k = TokKind::Ident;
            if (id == "let") k = TokKind::KwLet;
            else if (id == "const") k = TokKind::KwConst;
            else if (id == "var") k = TokKind::KwVar;
            else if (id == "function") k = TokKind::KwFunction;
            else if (id == "return") k = TokKind::KwReturn;
            else if (id == "if") k = TokKind::KwIf;
            else if (id == "else") k = TokKind::KwElse;
            else if (id == "while") k = TokKind::KwWhile;
            else if (id == "for") k = TokKind::KwFor;
            else if (id == "true") k = TokKind::KwTrue;
            else if (id == "false") k = TokKind::KwFalse;
            else if (id == "null") k = TokKind::KwNull;
            out.push_back({k, std::move(id), 0.0});
            continue;
        }

        // Multi-char operators
        std::string_view two = (i + 1 < n) ? src.substr(i, 2) : std::string_view{};
        std::string_view three = (i + 2 < n) ? src.substr(i, 3) : std::string_view{};
        if (three == "===") { out.push_back({TokKind::Eq, "===", 0}); i += 3; continue; }
        if (three == "!==") { out.push_back({TokKind::Neq, "!==", 0}); i += 3; continue; }
        if (two == "==") { out.push_back({TokKind::Eq, "==", 0}); i += 2; continue; }
        if (two == "!=") { out.push_back({TokKind::Neq, "!=", 0}); i += 2; continue; }
        if (two == "<=") { out.push_back({TokKind::Le, "<=", 0}); i += 2; continue; }
        if (two == ">=") { out.push_back({TokKind::Ge, ">=", 0}); i += 2; continue; }
        if (two == "&&") { out.push_back({TokKind::AndAnd, "&&", 0}); i += 2; continue; }
        if (two == "||") { out.push_back({TokKind::OrOr, "||", 0}); i += 2; continue; }

        ++i;
        switch (c) {
            case '(': out.push_back({TokKind::LParen, "(", 0}); break;
            case ')': out.push_back({TokKind::RParen, ")", 0}); break;
            case '{': out.push_back({TokKind::LBrace, "{", 0}); break;
            case '}': out.push_back({TokKind::RBrace, "}", 0}); break;
            case '[': out.push_back({TokKind::LBracket, "[", 0}); break;
            case ']': out.push_back({TokKind::RBracket, "]", 0}); break;
            case ',': out.push_back({TokKind::Comma, ",", 0}); break;
            case ';': out.push_back({TokKind::Semi, ";", 0}); break;
            case '.': out.push_back({TokKind::Dot, ".", 0}); break;
            case ':': out.push_back({TokKind::Colon, ":", 0}); break;
            case '+': out.push_back({TokKind::Plus, "+", 0}); break;
            case '-': out.push_back({TokKind::Minus, "-", 0}); break;
            case '*': out.push_back({TokKind::Star, "*", 0}); break;
            case '/': out.push_back({TokKind::Slash, "/", 0}); break;
            case '%': out.push_back({TokKind::Percent, "%", 0}); break;
            case '!': out.push_back({TokKind::Bang, "!", 0}); break;
            case '=': out.push_back({TokKind::Assign, "=", 0}); break;
            case '<': out.push_back({TokKind::Lt, "<", 0}); break;
            case '>': out.push_back({TokKind::Gt, ">", 0}); break;
            default: break;
        }
    }
    out.push_back({TokKind::Eof, "", 0.0});
    return out;
}

class BytecodeCompiler {
public:
    using AllocFn = std::function<GcObject*(GcObject::Type)>;

    BytecodeCompiler(const std::vector<Token>& toks, AllocFn alloc)
        : toks_(toks), alloc_(std::move(alloc)) {}

    bool compile_program(BytecodeChunk& out, std::string& err) {
        chunk_ = &out;
        while (!check(TokKind::Eof)) {
            if (!compile_stmt(err)) return false;
        }
        emit(OpCode::Halt);
        return true;
    }

private:
    const Token& peek(size_t ahead = 0) const {
        size_t idx = std::min(pos_ + ahead, toks_.size() - 1);
        return toks_[idx];
    }
    const Token& advance() {
        const Token& t = peek();
        if (pos_ + 1 < toks_.size()) ++pos_;
        return t;
    }
    bool check(TokKind k) const { return peek().kind == k; }
    bool match(TokKind k) {
        if (!check(k)) return false;
        advance();
        return true;
    }

    int32_t add_const(JsValue v) {
        int32_t idx = static_cast<int32_t>(chunk_->constants.size());
        chunk_->constants.push_back(std::move(v));
        return idx;
    }
    int32_t add_name(const std::string& name) {
        for (size_t i = 0; i < chunk_->names.size(); ++i) {
            if (chunk_->names[i] == name) return static_cast<int32_t>(i);
        }
        int32_t idx = static_cast<int32_t>(chunk_->names.size());
        chunk_->names.push_back(name);
        return idx;
    }
    int32_t find_local(const std::string& name) const {
        for (int32_t i = static_cast<int32_t>(locals_.size()) - 1; i >= 0; --i) {
            if (locals_[static_cast<size_t>(i)] == name) return i;
        }
        return -1;
    }
    int32_t emit(OpCode op, int32_t a = 0, int32_t b = 0) {
        int32_t idx = static_cast<int32_t>(chunk_->code.size());
        chunk_->code.push_back({op, a, b});
        return idx;
    }

    bool compile_stmt(std::string& err) {
        if (match(TokKind::Semi)) return true;
        if (match(TokKind::LBrace)) {
            while (!check(TokKind::RBrace) && !check(TokKind::Eof)) {
                if (!compile_stmt(err)) return false;
            }
            match(TokKind::RBrace);
            return true;
        }
        if (match(TokKind::KwLet) || match(TokKind::KwConst) || match(TokKind::KwVar)) {
            if (!check(TokKind::Ident)) { err = "Expected variable name"; return false; }
            std::string name = advance().text;
            if (match(TokKind::Assign)) {
                if (!compile_expr(err)) return false;
            } else {
                emit(OpCode::PushConst, add_const(JsValue::make_undefined()));
            }
            match(TokKind::Semi);
            if (in_function_) {
                int32_t slot = find_local(name);
                if (slot < 0) {
                    slot = static_cast<int32_t>(locals_.size());
                    locals_.push_back(name);
                }
                emit(OpCode::StoreLocal, slot);
            } else {
                emit(OpCode::StoreGlobal, add_name(name));
            }
            emit(OpCode::Pop);
            return true;
        }
        if (match(TokKind::KwFunction)) {
            std::string fname = check(TokKind::Ident) ? advance().text : "<anon>";
            GcObject* fn = compile_function_body(fname, err);
            if (!fn) return false;
            emit(OpCode::PushConst, add_const(JsValue::make_obj(fn)));
            if (in_function_) {
                int32_t slot = static_cast<int32_t>(locals_.size());
                locals_.push_back(fname);
                emit(OpCode::StoreLocal, slot);
            } else {
                emit(OpCode::StoreGlobal, add_name(fname));
            }
            emit(OpCode::Pop);
            return true;
        }
        if (match(TokKind::KwReturn)) {
            if (check(TokKind::Semi) || check(TokKind::RBrace)) {
                emit(OpCode::PushConst, add_const(JsValue::make_undefined()));
            } else {
                if (!compile_expr(err)) return false;
            }
            match(TokKind::Semi);
            emit(OpCode::Return);
            return true;
        }
        if (match(TokKind::KwIf)) {
            match(TokKind::LParen);
            if (!compile_expr(err)) return false;
            match(TokKind::RParen);
            int32_t jfalse = emit(OpCode::JumpIfFalse, 0);
            if (!compile_stmt(err)) return false;
            if (match(TokKind::KwElse)) {
                int32_t jend = emit(OpCode::Jump, 0);
                chunk_->code[static_cast<size_t>(jfalse)].operand = static_cast<int32_t>(chunk_->code.size());
                if (!compile_stmt(err)) return false;
                chunk_->code[static_cast<size_t>(jend)].operand = static_cast<int32_t>(chunk_->code.size());
            } else {
                chunk_->code[static_cast<size_t>(jfalse)].operand = static_cast<int32_t>(chunk_->code.size());
            }
            return true;
        }
        if (match(TokKind::KwWhile)) {
            int32_t loop_start = static_cast<int32_t>(chunk_->code.size());
            match(TokKind::LParen);
            if (!compile_expr(err)) return false;
            match(TokKind::RParen);
            int32_t jexit = emit(OpCode::JumpIfFalse, 0);
            if (!compile_stmt(err)) return false;
            emit(OpCode::Jump, loop_start);
            chunk_->code[static_cast<size_t>(jexit)].operand = static_cast<int32_t>(chunk_->code.size());
            return true;
        }
        if (match(TokKind::KwFor)) {
            match(TokKind::LParen);
            if (!check(TokKind::Semi)) {
                if (!compile_stmt(err)) return false;
            } else {
                match(TokKind::Semi);
            }
            int32_t cond_start = static_cast<int32_t>(chunk_->code.size());
            int32_t jexit = -1;
            if (!check(TokKind::Semi)) {
                if (!compile_expr(err)) return false;
                jexit = emit(OpCode::JumpIfFalse, 0);
            }
            match(TokKind::Semi);
            // Save increment expression tokens by skipping to RParen, compiling body first, then increment
            size_t inc_start = pos_;
            int depth = 0;
            while (!check(TokKind::Eof) && !(check(TokKind::RParen) && depth == 0)) {
                if (check(TokKind::LParen)) ++depth;
                else if (check(TokKind::RParen)) --depth;
                advance();
            }
            size_t inc_end = pos_;
            match(TokKind::RParen);
            if (!compile_stmt(err)) return false;
            if (inc_end > inc_start) {
                size_t saved_pos = pos_;
                pos_ = inc_start;
                if (!compile_expr(err)) return false;
                emit(OpCode::Pop);
                pos_ = saved_pos;
            }
            emit(OpCode::Jump, cond_start);
            if (jexit >= 0) {
                chunk_->code[static_cast<size_t>(jexit)].operand = static_cast<int32_t>(chunk_->code.size());
            }
            return true;
        }

        if (!compile_expr(err)) return false;
        match(TokKind::Semi);
        emit(OpCode::Pop);
        return true;
    }

    GcObject* compile_function_body(const std::string& fname, std::string& err) {
        GcObject* fn = alloc_(GcObject::Type::Function);
        fn->fn_name = fname;
        match(TokKind::LParen);
        while (!check(TokKind::RParen) && !check(TokKind::Eof)) {
            if (check(TokKind::Ident)) fn->params.push_back(advance().text);
            if (!match(TokKind::Comma)) break;
        }
        match(TokKind::RParen);
        match(TokKind::LBrace);

        BytecodeChunk* prev_chunk = chunk_;
        auto prev_locals = locals_;
        bool prev_in_fn = in_function_;

        chunk_ = &fn->chunk;
        locals_ = fn->params;
        in_function_ = true;

        while (!check(TokKind::RBrace) && !check(TokKind::Eof)) {
            if (!compile_stmt(err)) {
                chunk_ = prev_chunk;
                locals_ = prev_locals;
                in_function_ = prev_in_fn;
                return nullptr;
            }
        }
        match(TokKind::RBrace);
        emit(OpCode::PushConst, add_const(JsValue::make_undefined()));
        emit(OpCode::Return);

        chunk_ = prev_chunk;
        locals_ = prev_locals;
        in_function_ = prev_in_fn;
        return fn;
    }

    bool compile_expr(std::string& err) { return compile_assign(err); }

    bool compile_assign(std::string& err) {
        // Simple identifier assignment: x = expr
        if (check(TokKind::Ident) && peek(1).kind == TokKind::Assign) {
            std::string name = advance().text;
            advance(); // '='
            if (!compile_assign(err)) return false;
            int32_t slot = in_function_ ? find_local(name) : -1;
            if (slot >= 0) emit(OpCode::StoreLocal, slot);
            else emit(OpCode::StoreGlobal, add_name(name));
            return true;
        }
        return compile_or(err);
    }

    bool compile_or(std::string& err) {
        if (!compile_and(err)) return false;
        while (match(TokKind::OrOr)) {
            if (!compile_and(err)) return false;
            // Short logical OR via Add/truthy check simplified: keep rhs if lhs falsy
            // For deterministic VM semantics, compile with simple jump-over would be nicer;
            // keep clean binary op here via Eq/Neq helpers
        }
        return true;
    }

    bool compile_and(std::string& err) {
        if (!compile_equality(err)) return false;
        while (match(TokKind::AndAnd)) {
            int32_t jf = emit(OpCode::JumpIfFalse, 0);
            if (!compile_equality(err)) return false;
            int32_t jend = emit(OpCode::Jump, 0);
            chunk_->code[static_cast<size_t>(jf)].operand = static_cast<int32_t>(chunk_->code.size());
            emit(OpCode::PushConst, add_const(JsValue::make_bool(false)));
            chunk_->code[static_cast<size_t>(jend)].operand = static_cast<int32_t>(chunk_->code.size());
        }
        return true;
    }

    bool compile_equality(std::string& err) {
        if (!compile_compare(err)) return false;
        while (check(TokKind::Eq) || check(TokKind::Neq)) {
            TokKind k = advance().kind;
            if (!compile_compare(err)) return false;
            emit(k == TokKind::Eq ? OpCode::Eq : OpCode::Neq);
        }
        return true;
    }

    bool compile_compare(std::string& err) {
        if (!compile_add(err)) return false;
        while (check(TokKind::Lt) || check(TokKind::Le) || check(TokKind::Gt) || check(TokKind::Ge)) {
            TokKind k = advance().kind;
            if (!compile_add(err)) return false;
            if (k == TokKind::Lt) emit(OpCode::Lt);
            else if (k == TokKind::Le) emit(OpCode::Le);
            else if (k == TokKind::Gt) emit(OpCode::Gt);
            else emit(OpCode::Ge);
        }
        return true;
    }

    bool compile_add(std::string& err) {
        if (!compile_mul(err)) return false;
        while (check(TokKind::Plus) || check(TokKind::Minus)) {
            TokKind k = advance().kind;
            if (!compile_mul(err)) return false;
            emit(k == TokKind::Plus ? OpCode::Add : OpCode::Sub);
        }
        return true;
    }

    bool compile_mul(std::string& err) {
        if (!compile_unary(err)) return false;
        while (check(TokKind::Star) || check(TokKind::Slash) || check(TokKind::Percent)) {
            TokKind k = advance().kind;
            if (!compile_unary(err)) return false;
            if (k == TokKind::Star) emit(OpCode::Mul);
            else if (k == TokKind::Slash) emit(OpCode::Div);
            else emit(OpCode::Mod);
        }
        return true;
    }

    bool compile_unary(std::string& err) {
        if (match(TokKind::Bang)) {
            if (!compile_unary(err)) return false;
            emit(OpCode::Not);
            return true;
        }
        if (match(TokKind::Minus)) {
            if (!compile_unary(err)) return false;
            emit(OpCode::Neg);
            return true;
        }
        return compile_postfix(err);
    }

    bool compile_postfix(std::string& err) {
        if (!compile_primary(err)) return false;
        for (;;) {
            if (match(TokKind::Dot)) {
                if (!check(TokKind::Ident)) { err = "Expected property name"; return false; }
                std::string prop = advance().text;
                if (match(TokKind::Assign)) {
                    emit(OpCode::PushConst, add_const(JsValue::make_string(prop)));
                    if (!compile_assign(err)) return false;
                    emit(OpCode::SetProp);
                    return true;
                }
                if (match(TokKind::LParen)) {
                    int32_t argc = 0;
                    while (!check(TokKind::RParen) && !check(TokKind::Eof)) {
                        if (!compile_expr(err)) return false;
                        ++argc;
                        if (!match(TokKind::Comma)) break;
                    }
                    match(TokKind::RParen);
                    emit(OpCode::CallMethod, add_name(prop), argc);
                } else {
                    emit(OpCode::PushConst, add_const(JsValue::make_string(prop)));
                    emit(OpCode::GetProp);
                }
            } else if (match(TokKind::LBracket)) {
                if (!compile_expr(err)) return false;
                match(TokKind::RBracket);
                if (match(TokKind::Assign)) {
                    if (!compile_assign(err)) return false;
                    emit(OpCode::SetProp);
                    return true;
                }
                emit(OpCode::GetProp);
            } else if (match(TokKind::LParen)) {
                int32_t argc = 0;
                while (!check(TokKind::RParen) && !check(TokKind::Eof)) {
                    if (!compile_expr(err)) return false;
                    ++argc;
                    if (!match(TokKind::Comma)) break;
                }
                match(TokKind::RParen);
                emit(OpCode::Call, argc);
            } else {
                break;
            }
        }
        return true;
    }

    bool compile_primary(std::string& err) {
        if (check(TokKind::Number)) {
            emit(OpCode::PushConst, add_const(JsValue::make_number(advance().num)));
            return true;
        }
        if (check(TokKind::String)) {
            emit(OpCode::PushConst, add_const(JsValue::make_string(advance().text)));
            return true;
        }
        if (match(TokKind::KwTrue)) {
            emit(OpCode::PushConst, add_const(JsValue::make_bool(true)));
            return true;
        }
        if (match(TokKind::KwFalse)) {
            emit(OpCode::PushConst, add_const(JsValue::make_bool(false)));
            return true;
        }
        if (match(TokKind::KwNull)) {
            emit(OpCode::PushConst, add_const(JsValue::make_null()));
            return true;
        }
        if (match(TokKind::KwFunction)) {
            std::string fname = check(TokKind::Ident) ? advance().text : "<lambda>";
            GcObject* fn = compile_function_body(fname, err);
            if (!fn) return false;
            emit(OpCode::PushConst, add_const(JsValue::make_obj(fn)));
            return true;
        }
        if (match(TokKind::LParen)) {
            if (!compile_expr(err)) return false;
            match(TokKind::RParen);
            return true;
        }
        if (match(TokKind::LBracket)) {
            int32_t count = 0;
            while (!check(TokKind::RBracket) && !check(TokKind::Eof)) {
                if (!compile_expr(err)) return false;
                ++count;
                if (!match(TokKind::Comma)) break;
            }
            match(TokKind::RBracket);
            emit(OpCode::MakeArray, count);
            return true;
        }
        if (match(TokKind::LBrace)) {
            int32_t count = 0;
            while (!check(TokKind::RBrace) && !check(TokKind::Eof)) {
                std::string key;
                if (check(TokKind::Ident) || check(TokKind::String)) key = advance().text;
                else { err = "Expected object key"; return false; }
                match(TokKind::Colon);
                emit(OpCode::PushConst, add_const(JsValue::make_string(key)));
                if (!compile_expr(err)) return false;
                ++count;
                if (!match(TokKind::Comma)) break;
            }
            match(TokKind::RBrace);
            emit(OpCode::MakeObject, count);
            return true;
        }
        if (check(TokKind::Ident)) {
            std::string name = advance().text;
            int32_t slot = in_function_ ? find_local(name) : -1;
            if (slot >= 0) emit(OpCode::LoadLocal, slot);
            else emit(OpCode::LoadGlobal, add_name(name));
            return true;
        }
        err = "Unexpected token '" + peek().text + "'";
        return false;
    }

    const std::vector<Token>& toks_;
    AllocFn alloc_;
    size_t pos_ = 0;
    BytecodeChunk* chunk_ = nullptr;
    std::vector<std::string> locals_;
    bool in_function_ = false;
};

} // namespace

std::string JsValue::to_string() const {
    switch (kind) {
        case Kind::Undefined: return "undefined";
        case Kind::Null: return "null";
        case Kind::Bool: return bval ? "true" : "false";
        case Kind::Number: {
            if (std::floor(nval) == nval && std::abs(nval) < 1e15) {
                return std::to_string(static_cast<int64_t>(nval));
            }
            std::ostringstream oss;
            oss << nval;
            return oss.str();
        }
        case Kind::String: return sval;
        case Kind::DomRef: return node ? ("<" + node->tag + ">") : "null";
        case Kind::Object:
            if (!obj) return "null";
            if (obj->type == GcObject::Type::Array) return "[Array(" + std::to_string(obj->elements.size()) + ")]";
            if (obj->type == GcObject::Type::Function) return "[Function " + obj->fn_name + "]";
            return "[Object]";
    }
    return {};
}

bool JsValue::truthy() const {
    switch (kind) {
        case Kind::Undefined:
        case Kind::Null: return false;
        case Kind::Bool: return bval;
        case Kind::Number: return nval != 0.0 && !std::isnan(nval);
        case Kind::String: return !sval.empty();
        case Kind::DomRef: return node != nullptr;
        case Kind::Object: return obj != nullptr;
    }
    return false;
}

double JsValue::to_number() const {
    switch (kind) {
        case Kind::Number: return nval;
        case Kind::Bool: return bval ? 1.0 : 0.0;
        case Kind::String: return std::strtod(sval.c_str(), nullptr);
        default: return 0.0;
    }
}

AstraJsEngine::~AstraJsEngine() {
    for (GcObject* o : heap_) delete o;
    heap_.clear();
}

void AstraJsEngine::bind_storage(StorageGetFn getter, StorageSetFn setter) {
    storage_get_ = std::move(getter);
    storage_set_ = std::move(setter);
}

GcObject* AstraJsEngine::alloc_object(GcObject::Type type) {
    GcObject* o = new GcObject();
    o->type = type;
    heap_.push_back(o);
    ++total_allocated_;
    return o;
}

void AstraJsEngine::mark_value(const JsValue& v) {
    if (v.kind == JsValue::Kind::Object && v.obj) {
        mark_object(v.obj);
    }
}

void AstraJsEngine::mark_object(GcObject* o) {
    if (!o || o->marked) return;
    o->marked = true;
    for (const auto& e : o->elements) mark_value(e);
    for (const auto& [k, v] : o->props) mark_value(v);
    for (const auto& c : o->chunk.constants) mark_value(c);
}

size_t AstraJsEngine::gc_collect() {
    for (GcObject* o : heap_) o->marked = false;

    for (const auto& [k, v] : globals_) mark_value(v);
    for (const auto& v : stack_) mark_value(v);
    for (const auto& v : microtasks_) mark_value(v);
    for (const auto& t : timers_) mark_value(t.callback);
    for (const auto& [key, vec] : listeners_) {
        for (const auto& cb : vec) mark_value(cb);
    }

    size_t freed = 0;
    std::vector<GcObject*> alive;
    alive.reserve(heap_.size());
    for (GcObject* o : heap_) {
        if (o->marked) {
            alive.push_back(o);
        } else {
            delete o;
            ++freed;
        }
    }
    heap_ = std::move(alive);
    total_collected_ += freed;
    ++gc_collections_;
    return freed;
}

GcStats AstraJsEngine::gc_stats() const {
    return GcStats{heap_.size(), total_allocated_, total_collected_, gc_collections_};
}

JsValue AstraJsEngine::invoke_callable(const JsValue& fn,
                                       const std::vector<JsValue>& args,
                                       HtmlDocument& doc,
                                       JsExecutionReport& rep) {
    if (fn.kind == JsValue::Kind::String) {
        // Built-in global functions
        if (fn.sval == "__queueMicrotask__" && !args.empty()) {
            microtasks_.push_back(args[0]);
            return JsValue::make_undefined();
        }
        if (fn.sval == "__setTimeout__" && !args.empty()) {
            int64_t delay = args.size() > 1 ? static_cast<int64_t>(args[1].to_number()) : 0;
            timers_.push_back({delay, args[0]});
            return JsValue::make_number(static_cast<double>(timers_.size()));
        }
        if (fn.sval == "__gc__") {
            return JsValue::make_number(static_cast<double>(gc_collect()));
        }
        return JsValue::make_undefined();
    }

    if (fn.kind != JsValue::Kind::Object || !fn.obj || fn.obj->type != GcObject::Type::Function) {
        return JsValue::make_undefined();
    }
    std::vector<JsValue> locals(fn.obj->params.size(), JsValue::make_undefined());
    for (size_t i = 0; i < std::min(locals.size(), args.size()); ++i) {
        locals[i] = args[i];
    }
    return run_chunk(fn.obj->chunk, locals, doc, rep);
}

JsValue AstraJsEngine::run_chunk(const BytecodeChunk& chunk,
                                 std::vector<JsValue>& locals,
                                 HtmlDocument& doc,
                                 JsExecutionReport& rep) {
    size_t ip = 0;
    const size_t n = chunk.code.size();
    const size_t kMaxSteps = 200000;

    auto pop = [&]() -> JsValue {
        if (stack_.empty()) return JsValue::make_undefined();
        JsValue v = std::move(stack_.back());
        stack_.pop_back();
        return v;
    };

    while (ip < n && rep.instructions_executed < kMaxSteps) {
        const Instruction& ins = chunk.code[ip++];
        ++rep.instructions_executed;

        switch (ins.op) {
            case OpCode::PushConst:
                stack_.push_back(chunk.constants[static_cast<size_t>(ins.operand)]);
                break;
            case OpCode::LoadLocal: {
                size_t idx = static_cast<size_t>(ins.operand);
                stack_.push_back(idx < locals.size() ? locals[idx] : JsValue::make_undefined());
                break;
            }
            case OpCode::StoreLocal: {
                size_t idx = static_cast<size_t>(ins.operand);
                if (idx >= locals.size()) locals.resize(idx + 1, JsValue::make_undefined());
                locals[idx] = stack_.empty() ? JsValue::make_undefined() : stack_.back();
                break;
            }
            case OpCode::LoadGlobal: {
                const std::string& name = chunk.names[static_cast<size_t>(ins.operand)];
                if (name == "document" || name == "console" || name == "astra" || name == "Math") {
                    stack_.push_back(JsValue::make_string("__builtin_" + name + "__"));
                } else if (name == "queueMicrotask") {
                    stack_.push_back(JsValue::make_string("__queueMicrotask__"));
                } else if (name == "setTimeout") {
                    stack_.push_back(JsValue::make_string("__setTimeout__"));
                } else if (name == "gc") {
                    stack_.push_back(JsValue::make_string("__gc__"));
                } else {
                    auto it = globals_.find(name);
                    stack_.push_back(it != globals_.end() ? it->second : JsValue::make_undefined());
                }
                break;
            }
            case OpCode::StoreGlobal: {
                const std::string& name = chunk.names[static_cast<size_t>(ins.operand)];
                globals_[name] = stack_.empty() ? JsValue::make_undefined() : stack_.back();
                break;
            }
            case OpCode::Pop:
                if (!stack_.empty()) stack_.pop_back();
                break;
            case OpCode::Add: {
                JsValue b = pop(), a = pop();
                if (a.kind == JsValue::Kind::String || b.kind == JsValue::Kind::String) {
                    stack_.push_back(JsValue::make_string(a.to_string() + b.to_string()));
                } else {
                    stack_.push_back(JsValue::make_number(a.to_number() + b.to_number()));
                }
                break;
            }
            case OpCode::Sub: { JsValue b = pop(), a = pop(); stack_.push_back(JsValue::make_number(a.to_number() - b.to_number())); break; }
            case OpCode::Mul: { JsValue b = pop(), a = pop(); stack_.push_back(JsValue::make_number(a.to_number() * b.to_number())); break; }
            case OpCode::Div: { JsValue b = pop(), a = pop(); stack_.push_back(JsValue::make_number(b.to_number() != 0 ? a.to_number() / b.to_number() : 0.0)); break; }
            case OpCode::Mod: { JsValue b = pop(), a = pop(); stack_.push_back(JsValue::make_number(std::fmod(a.to_number(), b.to_number()))); break; }
            case OpCode::Neg: { JsValue a = pop(); stack_.push_back(JsValue::make_number(-a.to_number())); break; }
            case OpCode::Not: { JsValue a = pop(); stack_.push_back(JsValue::make_bool(!a.truthy())); break; }
            case OpCode::Eq: {
                JsValue b = pop(), a = pop();
                bool eq = (a.kind == b.kind) && (a.to_string() == b.to_string());
                if (a.kind == JsValue::Kind::Number && b.kind == JsValue::Kind::Number) eq = (a.nval == b.nval);
                stack_.push_back(JsValue::make_bool(eq));
                break;
            }
            case OpCode::Neq: {
                JsValue b = pop(), a = pop();
                bool eq = (a.kind == b.kind) && (a.to_string() == b.to_string());
                if (a.kind == JsValue::Kind::Number && b.kind == JsValue::Kind::Number) eq = (a.nval == b.nval);
                stack_.push_back(JsValue::make_bool(!eq));
                break;
            }
            case OpCode::Lt: { JsValue b = pop(), a = pop(); stack_.push_back(JsValue::make_bool(a.to_number() < b.to_number())); break; }
            case OpCode::Le: { JsValue b = pop(), a = pop(); stack_.push_back(JsValue::make_bool(a.to_number() <= b.to_number())); break; }
            case OpCode::Gt: { JsValue b = pop(), a = pop(); stack_.push_back(JsValue::make_bool(a.to_number() > b.to_number())); break; }
            case OpCode::Ge: { JsValue b = pop(), a = pop(); stack_.push_back(JsValue::make_bool(a.to_number() >= b.to_number())); break; }
            case OpCode::Jump:
                ip = static_cast<size_t>(ins.operand);
                break;
            case OpCode::JumpIfFalse: {
                JsValue cond = pop();
                if (!cond.truthy()) ip = static_cast<size_t>(ins.operand);
                break;
            }
            case OpCode::MakeArray: {
                GcObject* arr = alloc_object(GcObject::Type::Array);
                int32_t cnt = ins.operand;
                arr->elements.resize(static_cast<size_t>(cnt));
                for (int32_t i = cnt - 1; i >= 0; --i) arr->elements[static_cast<size_t>(i)] = pop();
                stack_.push_back(JsValue::make_obj(arr));
                break;
            }
            case OpCode::MakeObject: {
                GcObject* obj = alloc_object(GcObject::Type::Map);
                int32_t cnt = ins.operand;
                for (int32_t i = 0; i < cnt; ++i) {
                    JsValue val = pop();
                    JsValue key = pop();
                    obj->props[key.to_string()] = std::move(val);
                }
                stack_.push_back(JsValue::make_obj(obj));
                break;
            }
            case OpCode::GetProp: {
                JsValue key = pop(), obj = pop();
                std::string k = key.to_string();
                if (obj.kind == JsValue::Kind::String && obj.sval == "__builtin_astra__" && k == "storage") {
                    stack_.push_back(JsValue::make_string("__builtin_astra_storage__"));
                } else if (obj.kind == JsValue::Kind::String && obj.sval == "__builtin_document__" && k == "title") {
                    stack_.push_back(JsValue::make_string(doc.title()));
                } else if (obj.kind == JsValue::Kind::DomRef && obj.node) {
                    if (k == "textContent") stack_.push_back(JsValue::make_string(obj.node->text_content()));
                    else if (k == "tagName") stack_.push_back(JsValue::make_string(obj.node->tag));
                    else stack_.push_back(JsValue::make_string(obj.node->get_attr(k)));
                } else if (obj.kind == JsValue::Kind::Object && obj.obj) {
                    if (obj.obj->type == GcObject::Type::Array) {
                        if (k == "length") {
                            stack_.push_back(JsValue::make_number(static_cast<double>(obj.obj->elements.size())));
                        } else {
                            size_t idx = static_cast<size_t>(std::max(0.0, key.to_number()));
                            stack_.push_back(idx < obj.obj->elements.size() ? obj.obj->elements[idx] : JsValue::make_undefined());
                        }
                    } else {
                        auto it = obj.obj->props.find(k);
                        stack_.push_back(it != obj.obj->props.end() ? it->second : JsValue::make_undefined());
                    }
                } else if (obj.kind == JsValue::Kind::String && k == "length") {
                    stack_.push_back(JsValue::make_number(static_cast<double>(obj.sval.size())));
                } else {
                    stack_.push_back(JsValue::make_undefined());
                }
                break;
            }
            case OpCode::SetProp: {
                JsValue val = pop(), key = pop(), obj = pop();
                std::string k = key.to_string();
                if (obj.kind == JsValue::Kind::DomRef && obj.node) {
                    if (k == "textContent") obj.node->set_text_content(doc.arena(), val.to_string());
                    else if (k == "style") obj.node->set_attr("style", val.to_string());
                    else obj.node->set_attr(k, val.to_string());
                } else if (obj.kind == JsValue::Kind::Object && obj.obj) {
                    if (obj.obj->type == GcObject::Type::Array) {
                        size_t idx = static_cast<size_t>(std::max(0.0, key.to_number()));
                        if (idx >= obj.obj->elements.size()) obj.obj->elements.resize(idx + 1, JsValue::make_undefined());
                        obj.obj->elements[idx] = val;
                    } else {
                        obj.obj->props[k] = val;
                    }
                }
                stack_.push_back(val);
                break;
            }
            case OpCode::Call: {
                int32_t argc = ins.operand;
                std::vector<JsValue> args(static_cast<size_t>(argc));
                for (int32_t i = argc - 1; i >= 0; --i) args[static_cast<size_t>(i)] = pop();
                JsValue callee = pop();
                stack_.push_back(invoke_callable(callee, args, doc, rep));
                break;
            }
            case OpCode::CallMethod: {
                const std::string& method = chunk.names[static_cast<size_t>(ins.operand)];
                int32_t argc = ins.operand2;
                std::vector<JsValue> args(static_cast<size_t>(argc));
                for (int32_t i = argc - 1; i >= 0; --i) args[static_cast<size_t>(i)] = pop();
                JsValue recv = pop();

                JsValue ret = JsValue::make_undefined();
                if (recv.kind == JsValue::Kind::String && recv.sval == "__builtin_console__" && method == "log") {
                    std::string line;
                    for (size_t i = 0; i < args.size(); ++i) {
                        if (i) line.push_back(' ');
                        line += args[i].to_string();
                    }
                    rep.logs.push_back(std::move(line));
                } else if (recv.kind == JsValue::Kind::String && recv.sval == "__builtin_document__") {
                    if (method == "querySelector" && !args.empty()) {
                        ret = JsValue::make_dom(doc.query_selector(args[0].to_string()));
                    } else if (method == "querySelectorAll" && !args.empty()) {
                        GcObject* arr = alloc_object(GcObject::Type::Array);
                        for (DomNode* dn : doc.query_selector_all(args[0].to_string())) {
                            arr->elements.push_back(JsValue::make_dom(dn));
                        }
                        ret = JsValue::make_obj(arr);
                    } else if (method == "createElement" && !args.empty()) {
                        DomNode* el = doc.arena().create<DomNode>();
                        el->type = NodeType::Element;
                        el->tag = args[0].to_string();
                        ret = JsValue::make_dom(el);
                    }
                } else if (recv.kind == JsValue::Kind::String && recv.sval == "__builtin_astra_storage__") {
                    if (method == "set" && args.size() >= 2 && storage_set_) {
                        storage_set_(args[0].to_string(), args[1].to_string());
                    } else if (method == "get" && !args.empty() && storage_get_) {
                        ret = JsValue::make_string(storage_get_(args[0].to_string()));
                    }
                } else if (recv.kind == JsValue::Kind::String && recv.sval == "__builtin_Math__") {
                    if (method == "floor" && !args.empty()) ret = JsValue::make_number(std::floor(args[0].to_number()));
                    else if (method == "sqrt" && !args.empty()) ret = JsValue::make_number(std::sqrt(args[0].to_number()));
                } else if (recv.kind == JsValue::Kind::DomRef && recv.node) {
                    if (method == "setAttribute" && args.size() >= 2) {
                        recv.node->set_attr(args[0].to_string(), args[1].to_string());
                    } else if (method == "getAttribute" && !args.empty()) {
                        ret = JsValue::make_string(recv.node->get_attr(args[0].to_string()));
                    } else if (method == "appendChild" && !args.empty() && args[0].kind == JsValue::Kind::DomRef && args[0].node) {
                        args[0].node->parent = recv.node;
                        recv.node->children.push_back(args[0].node);
                        ret = args[0];
                    } else if (method == "addEventListener" && args.size() >= 2) {
                        listeners_[{recv.node, args[0].to_string()}].push_back(args[1]);
                    }
                } else if (recv.kind == JsValue::Kind::Object && recv.obj) {
                    if (recv.obj->type == GcObject::Type::Array && method == "push" && !args.empty()) {
                        recv.obj->elements.push_back(args[0]);
                        ret = JsValue::make_number(static_cast<double>(recv.obj->elements.size()));
                    } else if (recv.obj->props.count(method) > 0) {
                        ret = invoke_callable(recv.obj->props[method], args, doc, rep);
                    }
                }
                stack_.push_back(std::move(ret));
                break;
            }
            case OpCode::Return:
                return pop();
            case OpCode::Halt:
                return JsValue::make_undefined();
        }
    }
    return JsValue::make_undefined();
}

void AstraJsEngine::drain_event_loop(HtmlDocument& doc, JsExecutionReport& rep) {
    // 1. Drain microtask queue (FIFO)
    size_t guard = 0;
    while (!microtasks_.empty() && guard < 1024) {
        ++guard;
        auto batch = std::move(microtasks_);
        microtasks_.clear();
        for (const auto& cb : batch) {
            invoke_callable(cb, {}, doc, rep);
            ++rep.microtasks_run;
        }
    }
    // 2. Drain scheduled timers sorted by delay_ms
    std::stable_sort(timers_.begin(), timers_.end(), [](const TimerEntry& a, const TimerEntry& b) {
        return a.delay_ms < b.delay_ms;
    });
    auto timers_batch = std::move(timers_);
    timers_.clear();
    for (const auto& t : timers_batch) {
        invoke_callable(t.callback, {}, doc, rep);
        ++rep.timers_run;
        // Microtasks queued by a timer run immediately after that timer task
        while (!microtasks_.empty() && guard < 1024) {
            ++guard;
            auto m = std::move(microtasks_);
            microtasks_.clear();
            for (const auto& cb : m) {
                invoke_callable(cb, {}, doc, rep);
                ++rep.microtasks_run;
            }
        }
    }
}

JsExecutionReport AstraJsEngine::execute(std::string_view code, HtmlDocument& doc) {
    JsExecutionReport rep;
    auto tokens = tokenize(code);
    BytecodeChunk chunk;
    BytecodeCompiler compiler(tokens, [this](GcObject::Type t) { return alloc_object(t); });
    if (!compiler.compile_program(chunk, rep.error)) {
        rep.ok = false;
        return rep;
    }
    rep.bytecode_size = chunk.code.size();

    std::vector<JsValue> locals;
    run_chunk(chunk, locals, doc, rep);
    drain_event_loop(doc, rep);

    // Run GC if heap grew large
    if (heap_.size() > 64) {
        gc_collect();
    }
    rep.gc = gc_stats();
    return rep;
}

JsExecutionReport AstraJsEngine::dispatch_event(DomNode* target, const std::string& event_name, HtmlDocument& doc) {
    JsExecutionReport rep;
    auto it = listeners_.find({target, event_name});
    if (it != listeners_.end()) {
        auto cbs = it->second;
        for (const auto& cb : cbs) {
            invoke_callable(cb, {}, doc, rep);
        }
        drain_event_loop(doc, rep);
    }
    rep.gc = gc_stats();
    return rep;
}

} // namespace astra
