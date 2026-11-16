// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Query lexer and parser.

#include <algorithm>
#include <array>

#include "logsift/eval.hpp"
#include "logsift/query.hpp"
#include "logsift/text.hpp"

namespace logsift {

// ---- Shared helpers ------------------------------------------------------------

ExprPtr Expr::make_literal(Value v, std::size_t offset) {
    auto e = std::make_unique<Expr>();
    e->kind = Kind::Literal;
    e->literal = std::move(v);
    e->offset = offset;
    return e;
}

ExprPtr Expr::make_field(std::string name, std::size_t offset) {
    auto e = std::make_unique<Expr>();
    e->kind = Kind::Field;
    e->name = std::move(name);
    e->offset = offset;
    return e;
}

std::string_view binary_op_text(BinaryOp op) noexcept {
    switch (op) {
        case BinaryOp::Or: return "or";
        case BinaryOp::And: return "and";
        case BinaryOp::Eq: return "==";
        case BinaryOp::Ne: return "!=";
        case BinaryOp::Lt: return "<";
        case BinaryOp::Le: return "<=";
        case BinaryOp::Gt: return ">";
        case BinaryOp::Ge: return ">=";
        case BinaryOp::Match: return "=~";
        case BinaryOp::NotMatch: return "!~";
        case BinaryOp::Add: return "+";
        case BinaryOp::Sub: return "-";
        case BinaryOp::Mul: return "*";
        case BinaryOp::Div: return "/";
        case BinaryOp::Mod: return "%";
    }
    return "?";
}

std::string_view stage_name(Stage::Kind kind) noexcept {
    switch (kind) {
        case Stage::Kind::Where: return "where";
        case Stage::Kind::Eval: return "eval";
        case Stage::Kind::Fields: return "fields";
        case Stage::Kind::Rename: return "rename";
        case Stage::Kind::Sort: return "sort";
        case Stage::Kind::Head: return "head";
        case Stage::Kind::Tail: return "tail";
        case Stage::Kind::Dedup: return "dedup";
        case Stage::Kind::Stats: return "stats";
        case Stage::Kind::Bin: return "bin";
    }
    return "?";
}

namespace {

int precedence(const Expr& e) {
    switch (e.kind) {
        case Expr::Kind::Binary:
            switch (e.binary_op) {
                case BinaryOp::Or: return 1;
                case BinaryOp::And: return 2;
                case BinaryOp::Add:
                case BinaryOp::Sub: return 5;
                case BinaryOp::Mul:
                case BinaryOp::Div:
                case BinaryOp::Mod: return 6;
                default: return 4;
            }
        case Expr::Kind::In: return 4;
        case Expr::Kind::Unary: return e.unary_op == UnaryOp::Not ? 3 : 7;
        default: return 8;
    }
}

bool is_identifier_text(std::string_view s) {
    if (s.empty()) return false;
    const auto start_ok = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '@';
    };
    if (!start_ok(s[0])) return false;
    for (char c : s) {
        if (!(start_ok(c) || (c >= '0' && c <= '9') || c == '.')) return false;
    }
    static constexpr std::array<std::string_view, 9> kKeywords{"and", "or", "not", "in", "true", "false",
                                                               "null", "as", "by"};
    const std::string lower = to_lower(s);
    return std::find(kKeywords.begin(), kKeywords.end(), lower) == kKeywords.end();
}

std::string quote_string(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    return out + "\"";
}

std::string field_text(const std::string& name) {
    return is_identifier_text(name) ? name : "`" + name + "`";
}

void write_expr(std::string& out, const Expr& e);

void write_child(std::string& out, const Expr& child, bool parens) {
    if (parens) out += '(';
    write_expr(out, child);
    if (parens) out += ')';
}

void write_expr(std::string& out, const Expr& e) {
    switch (e.kind) {
        case Expr::Kind::Literal:
            if (e.is_duration && e.literal.is_int()) {
                out += format_duration(e.literal.as_int());
            } else if (e.literal.is_string()) {
                out += quote_string(e.literal.as_string());
            } else if (e.literal.is_null()) {
                out += "null";
            } else if (e.literal.is_time()) {
                out += "totime(" + quote_string(format_time(e.literal.as_time())) + ")";
            } else {
                out += e.literal.to_string();
            }
            return;
        case Expr::Kind::Field: out += field_text(e.name); return;
        case Expr::Kind::Unary:
            out += e.unary_op == UnaryOp::Not ? "not " : "-";
            write_child(out, *e.children[0], precedence(*e.children[0]) < precedence(e));
            return;
        case Expr::Kind::Binary: {
            const int p = precedence(e);
            write_child(out, *e.children[0], precedence(*e.children[0]) < p);
            out += ' ';
            out += binary_op_text(e.binary_op);
            out += ' ';
            write_child(out, *e.children[1], precedence(*e.children[1]) <= p);
            return;
        }
        case Expr::Kind::Call:
            out += e.name;
            out += '(';
            for (std::size_t i = 0; i < e.children.size(); ++i) {
                if (i) out += ", ";
                write_expr(out, *e.children[i]);
            }
            out += ')';
            return;
        case Expr::Kind::In:
            write_child(out, *e.children[0], precedence(*e.children[0]) <= 4);
            out += e.negated ? " not in (" : " in (";
            for (std::size_t i = 1; i < e.children.size(); ++i) {
                if (i > 1) out += ", ";
                write_expr(out, *e.children[i]);
            }
            out += ')';
            return;
    }
}

// ---- Lexer ------------------------------------------------------------------------

enum class Tok { Ident, Number, Duration, String, Op, Pipe, Comma, LParen, RParen, End };

struct Token {
    Tok kind = Tok::End;
    std::string text;   // identifier, operator, decoded string, or number text
    Value value;        // Number / Duration literal
    bool quoted = false;  // `backtick` identifier
    std::size_t offset = 0;
    std::size_t end = 0;
};

bool ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '@'; }
bool ident_char(char c) { return ident_start(c) || (c >= '0' && c <= '9') || c == '.'; }
bool digit(char c) { return c >= '0' && c <= '9'; }

std::vector<Token> lex(std::string_view s) {
    std::vector<Token> out;
    std::size_t i = 0;
    auto push = [&](Tok k, std::size_t start, std::string text) {
        Token t;
        t.kind = k;
        t.text = std::move(text);
        t.offset = start;
        t.end = i;
        out.push_back(std::move(t));
        return &out.back();
    };
    while (i < s.size()) {
        const char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++i;
            continue;
        }
        const std::size_t start = i;
        if (ident_start(c)) {
            while (i < s.size() && ident_char(s[i])) ++i;
            push(Tok::Ident, start, std::string(s.substr(start, i - start)));
        } else if (c == '`') {
            const std::size_t close = s.find('`', i + 1);
            if (close == std::string_view::npos) throw QueryError("unterminated `field name`", start);
            i = close + 1;
            Token* t = push(Tok::Ident, start, std::string(s.substr(start + 1, close - start - 1)));
            t->quoted = true;
            if (t->text.empty()) throw QueryError("empty `field name`", start);
        } else if (digit(c) || (c == '.' && i + 1 < s.size() && digit(s[i + 1]))) {
            while (i < s.size() && (digit(s[i]) || s[i] == '.')) ++i;
            if (i < s.size() && (s[i] == 'e' || s[i] == 'E') && i + 1 < s.size() &&
                (digit(s[i + 1]) || ((s[i + 1] == '+' || s[i + 1] == '-') && i + 2 < s.size() && digit(s[i + 2])))) {
                i += 2;
                while (i < s.size() && digit(s[i])) ++i;
            }
            if (i < s.size() && ((s[i] >= 'a' && s[i] <= 'z') || static_cast<unsigned char>(s[i]) >= 0x80)) {
                // A duration such as 5m, 250ms or 1h30m.
                while (i < s.size() && ((s[i] >= 'a' && s[i] <= 'z') || digit(s[i]) || s[i] == '.' ||
                                        static_cast<unsigned char>(s[i]) >= 0x80)) {
                    ++i;
                }
                const std::string text(s.substr(start, i - start));
                const auto d = parse_duration(text);
                if (!d) throw QueryError("invalid duration '" + text + "'", start);
                push(Tok::Duration, start, text)->value = Value::integer(*d);
                continue;
            }
            const std::string text(s.substr(start, i - start));
            Value v;
            if (auto n = parse_int(text)) {
                v = Value::integer(*n);
            } else if (auto d = parse_double(text)) {
                v = Value::real(*d);
            } else {
                throw QueryError("invalid number '" + text + "'", start);
            }
            push(Tok::Number, start, text)->value = std::move(v);
        } else if (c == '"' || c == '\'') {
            ++i;
            std::string text;
            bool closed = false;
            while (i < s.size()) {
                const char ch = s[i++];
                if (ch == c) {
                    closed = true;
                    break;
                }
                if (ch == '\\' && i < s.size()) {
                    const char e = s[i++];
                    switch (e) {
                        case 'n': text += '\n'; break;
                        case 't': text += '\t'; break;
                        case 'r': text += '\r'; break;
                        default: text += e; break;
                    }
                    continue;
                }
                text += ch;
            }
            if (!closed) throw QueryError("unterminated string", start);
            push(Tok::String, start, std::move(text));
        } else if (c == '|' && !(i + 1 < s.size() && s[i + 1] == '|')) {
            ++i;
            push(Tok::Pipe, start, "|");
        } else if (c == ',') {
            ++i;
            push(Tok::Comma, start, ",");
        } else if (c == '(') {
            ++i;
            push(Tok::LParen, start, "(");
        } else if (c == ')') {
            ++i;
            push(Tok::RParen, start, ")");
        } else {
            static constexpr std::array<std::string_view, 17> kOps{"==", "!=", "<=", ">=", "=~", "!~", "&&", "||",
                                                                   "<",  ">",  "=",  "+",  "-",  "*",  "/",  "%",
                                                                   "!"};
            bool matched = false;
            for (std::string_view op : kOps) {
                if (s.substr(i, op.size()) == op) {
                    i += op.size();
                    push(Tok::Op, start, std::string(op));
                    matched = true;
                    break;
                }
            }
            if (!matched) throw QueryError(std::string("unexpected character '") + c + "'", start);
        }
    }
    Token end;
    end.kind = Tok::End;
    end.offset = s.size();
    end.end = s.size();
    out.push_back(std::move(end));
    return out;
}

// ---- Parser -----------------------------------------------------------------------

constexpr std::size_t kMaxDepth = 128;

class Parser {
public:
    explicit Parser(std::string_view text) : tokens_(lex(text)) {}

    Query parse(std::string_view text) {
        Query q;
        q.text = std::string(text);
        const std::string_view t = trim(text);
        if (t.empty() || t == "*") return q;
        for (;;) {
            parse_segment(q);
            if (accept(Tok::Pipe)) continue;
            if (peek().kind != Tok::End) fail("expected '|' or end of query, found '" + peek().text + "'");
            break;
        }
        return q;
    }

private:
    const Token& peek(std::size_t ahead = 0) const {
        const std::size_t i = std::min(pos_ + ahead, tokens_.size() - 1);
        return tokens_[i];
    }
    const Token& advance() {
        const Token& t = tokens_[pos_];
        if (t.kind != Tok::End) ++pos_;
        return t;
    }
    bool accept(Tok k) {
        if (peek().kind != k) return false;
        advance();
        return true;
    }
    bool accept_op(std::string_view op) {
        if (peek().kind != Tok::Op || peek().text != op) return false;
        advance();
        return true;
    }
    bool accept_keyword(std::string_view word) {
        if (peek().kind != Tok::Ident || peek().quoted || !iequals(peek().text, word)) return false;
        advance();
        return true;
    }
    [[noreturn]] void fail(const std::string& message) const { throw QueryError(message, peek().offset); }
    [[noreturn]] void fail_at(const std::string& message, std::size_t offset) const {
        throw QueryError(message, offset);
    }

    std::string expect_name(const char* what) {
        if (peek().kind != Tok::Ident) fail(std::string("expected ") + what);
        return advance().text;
    }

    std::int64_t expect_count() {
        const Token& t = peek();
        if (t.kind != Tok::Number || !t.value.is_int() || t.value.as_int() < 0) fail("expected a non-negative integer");
        advance();
        return t.value.as_int();
    }

    // ---- Stages --------------------------------------------------------------

    static bool is_stage_keyword(const std::string& w) {
        static constexpr std::array<std::string_view, 12> kStages{"where", "eval",  "fields", "rename",
                                                                  "sort",  "head",  "tail",   "dedup",
                                                                  "stats", "bin",   "count",  "top"};
        return std::find(kStages.begin(), kStages.end(), w) != kStages.end();
    }

    // A keyword followed by an operator is a field reference in a filter
    // ("count > 5"), not a stage.
    bool starts_stage() const {
        const Token& t = peek();
        if (t.kind != Tok::Ident || t.quoted || !is_stage_keyword(to_lower(t.text))) return false;
        const Token& next = peek(1);
        const bool sign = next.text == "-" || next.text == "+";  // sort -x, fields - x
        const bool star = next.text == "*" && iequals(t.text, "fields");  // fields *
        if (next.kind == Tok::Op && !sign && !star) return false;
        if (next.kind == Tok::Ident && !next.quoted) {
            const std::string w = to_lower(next.text);
            if (w == "and" || w == "or" || w == "in") return false;
            if (w == "not" && peek(2).kind == Tok::Ident && iequals(peek(2).text, "in")) return false;
        }
        return true;
    }

    void parse_segment(Query& q) {
        if (!starts_stage()) {
            Stage s;
            s.kind = Stage::Kind::Where;
            s.offset = peek().offset;
            s.predicate = expression();
            q.stages.push_back(std::move(s));
            return;
        }
        const Token& kw = advance();
        const std::string word = to_lower(kw.text);
        Stage s;
        s.offset = kw.offset;
        if (word == "where") {
            s.kind = Stage::Kind::Where;
            s.predicate = expression();
        } else if (word == "eval") {
            s.kind = Stage::Kind::Eval;
            do {
                std::string name = expect_name("a field name");
                if (!accept_op("=")) fail("expected '=' after field name in eval");
                s.assignments.emplace_back(std::move(name), expression());
            } while (accept(Tok::Comma));
        } else if (word == "fields") {
            s.kind = Stage::Kind::Fields;
            s.exclude = accept_op("-");
            s.names = name_patterns();
        } else if (word == "rename") {
            s.kind = Stage::Kind::Rename;
            do {
                std::string from = expect_name("a field name");
                if (!accept_keyword("as")) fail("expected 'as' in rename");
                s.renames.emplace_back(std::move(from), expect_name("a new field name"));
            } while (accept(Tok::Comma));
        } else if (word == "sort") {
            s.kind = Stage::Kind::Sort;
            do {
                SortKey key;
                if (accept_op("-")) {
                    key.descending = true;
                } else {
                    accept_op("+");
                }
                key.field = expect_name("a field to sort by");
                if (accept_keyword("desc")) key.descending = true;
                else accept_keyword("asc");
                s.sort_keys.push_back(std::move(key));
            } while (accept(Tok::Comma));
        } else if (word == "head" || word == "tail") {
            s.kind = word == "head" ? Stage::Kind::Head : Stage::Kind::Tail;
            if (peek().kind == Tok::Number) s.count = expect_count();
        } else if (word == "dedup") {
            s.kind = Stage::Kind::Dedup;
            s.names = names();
        } else if (word == "stats") {
            s.kind = Stage::Kind::Stats;
            do {
                s.aggregates.push_back(aggregate());
            } while (accept(Tok::Comma));
            if (accept_keyword("by")) s.names = names();
        } else if (word == "count") {
            s.kind = Stage::Kind::Stats;
            Aggregate a;
            a.function = "count";
            a.alias = "count";
            s.aggregates.push_back(std::move(a));
            if (accept_keyword("by")) s.names = names();
        } else if (word == "top") {
            parse_top(q, kw.offset);
            return;
        } else if (word == "bin") {
            s.kind = Stage::Kind::Bin;
            s.field = expect_name("a field to bin");
            if (!accept_keyword("span") || !accept_op("=")) fail("expected span=<duration or number>");
            const Token& t = advance();
            if (t.kind == Tok::Duration) {
                s.duration_span = t.value.as_int();
                if (s.duration_span <= 0) fail_at("span must be positive", t.offset);
            } else if (t.kind == Tok::Number) {
                s.span_is_duration = false;
                s.numeric_span = *t.value.to_number();
                if (!(s.numeric_span > 0)) fail_at("span must be positive", t.offset);
            } else {
                fail_at("expected span=<duration or number>", t.offset);
            }
            s.alias = accept_keyword("as") ? expect_name("a field name") : s.field;
        }
        q.stages.push_back(std::move(s));
    }

    // top [N] field[, field...]  ==  stats count() as count by fields | sort -count | head N
    void parse_top(Query& q, std::size_t offset) {
        std::int64_t n = 10;
        if (peek().kind == Tok::Number) n = expect_count();
        Stage stats;
        stats.kind = Stage::Kind::Stats;
        stats.offset = offset;
        Aggregate a;
        a.function = "count";
        a.alias = "count";
        stats.aggregates.push_back(std::move(a));
        stats.names = names();
        Stage sort;
        sort.kind = Stage::Kind::Sort;
        sort.offset = offset;
        sort.sort_keys.push_back(SortKey{"count", true});
        Stage head;
        head.kind = Stage::Kind::Head;
        head.offset = offset;
        head.count = n;
        q.stages.push_back(std::move(stats));
        q.stages.push_back(std::move(sort));
        q.stages.push_back(std::move(head));
    }

    std::vector<std::string> names() {
        std::vector<std::string> out;
        do {
            out.push_back(expect_name("a field name"));
        } while (accept(Tok::Comma));
        return out;
    }

    // Field names, optionally ending in '*' (prefix pattern).
    std::vector<std::string> name_patterns() {
        std::vector<std::string> out;
        do {
            std::string name;
            if (peek().kind == Tok::Op && peek().text == "*") {
                advance();
                name = "*";
            } else {
                const Token& t = advance();
                if (t.kind != Tok::Ident) fail_at("expected a field name", t.offset);
                name = t.text;
                if (peek().kind == Tok::Op && peek().text == "*" && peek().offset == t.end) {
                    advance();
                    name += '*';
                }
            }
            out.push_back(std::move(name));
        } while (accept(Tok::Comma));
        return out;
    }

    Aggregate aggregate() {
        const Token& name_tok = peek();
        if (name_tok.kind != Tok::Ident) fail("expected an aggregate such as count() or avg(field)");
        advance();
        Aggregate a;
        a.function = to_lower(name_tok.text);
        if (!is_aggregate_function(a.function)) fail_at("unknown aggregate '" + name_tok.text + "'", name_tok.offset);
        if (!accept(Tok::LParen)) fail("expected '(' after " + a.function);
        if (peek().kind != Tok::RParen) a.argument = expression();
        if (a.function == "percentile") {
            if (!accept(Tok::Comma)) fail("percentile needs a rank: percentile(field, 95)");
            const Token& t = advance();
            const auto q = t.value.to_number();
            if (t.kind != Tok::Number || !q || *q < 0 || *q > 100) fail_at("percentile rank must be 0-100", t.offset);
            a.parameter = *q;
        }
        if (!accept(Tok::RParen)) fail("expected ')'");
        if (!a.argument && a.function != "count") fail_at(a.function + "() needs an argument", name_tok.offset);

        std::string text = a.function + "(";
        if (a.argument) text += to_string(*a.argument);
        if (a.function == "percentile") text += ", " + format_double(a.parameter);
        text += ")";
        // count() is simply "count"; other aggregates are named by their text.
        const std::string default_alias = (a.function == "count" && !a.argument) ? "count" : text;
        a.alias = accept_keyword("as") ? expect_name("a field name") : default_alias;
        return a;
    }

    // ---- Expressions ---------------------------------------------------------------

    ExprPtr expression() {
        if (++depth_ > kMaxDepth) fail("expression is nested too deeply");
        ExprPtr e = or_expr();
        --depth_;
        return e;
    }

    static ExprPtr binary(BinaryOp op, ExprPtr l, ExprPtr r, std::size_t offset) {
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::Binary;
        e->binary_op = op;
        e->offset = offset;
        e->children.push_back(std::move(l));
        e->children.push_back(std::move(r));
        return e;
    }

    ExprPtr or_expr() {
        ExprPtr lhs = and_expr();
        for (;;) {
            const std::size_t at = peek().offset;
            if (!accept_keyword("or") && !accept_op("||")) return lhs;
            lhs = binary(BinaryOp::Or, std::move(lhs), and_expr(), at);
        }
    }

    ExprPtr and_expr() {
        ExprPtr lhs = not_expr();
        for (;;) {
            const std::size_t at = peek().offset;
            if (!accept_keyword("and") && !accept_op("&&")) return lhs;
            lhs = binary(BinaryOp::And, std::move(lhs), not_expr(), at);
        }
    }

    ExprPtr not_expr() {
        const std::size_t at = peek().offset;
        if (accept_keyword("not") || accept_op("!")) {
            if (++depth_ > kMaxDepth) fail("expression is nested too deeply");
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Kind::Unary;
            e->unary_op = UnaryOp::Not;
            e->offset = at;
            e->children.push_back(not_expr());
            --depth_;
            return e;
        }
        return comparison();
    }

    ExprPtr comparison() {
        ExprPtr lhs = additive();
        const Token& t = peek();
        const std::size_t at = t.offset;
        if (t.kind == Tok::Op) {
            static constexpr std::array<std::pair<std::string_view, BinaryOp>, 9> kOps{{
                {"==", BinaryOp::Eq},
                {"=", BinaryOp::Eq},
                {"!=", BinaryOp::Ne},
                {"<", BinaryOp::Lt},
                {"<=", BinaryOp::Le},
                {">", BinaryOp::Gt},
                {">=", BinaryOp::Ge},
                {"=~", BinaryOp::Match},
                {"!~", BinaryOp::NotMatch},
            }};
            for (const auto& [text, op] : kOps) {
                if (t.text != text) continue;
                advance();
                ExprPtr e = binary(op, std::move(lhs), additive(), at);
                if ((op == BinaryOp::Match || op == BinaryOp::NotMatch) &&
                    e->children[1]->kind == Expr::Kind::Literal && e->children[1]->literal.is_string()) {
                    try {
                        e->regex = std::make_shared<const std::regex>(e->children[1]->literal.as_string(),
                                                                      std::regex::ECMAScript);
                    } catch (const std::regex_error& err) {
                        fail_at(std::string("invalid regular expression: ") + err.what(), e->children[1]->offset);
                    }
                }
                return e;
            }
        }
        bool negated = false;
        if (peek().kind == Tok::Ident && iequals(peek().text, "not") && peek(1).kind == Tok::Ident &&
            iequals(peek(1).text, "in")) {
            advance();
            negated = true;
        }
        if (accept_keyword("in")) {
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Kind::In;
            e->negated = negated;
            e->offset = at;
            e->children.push_back(std::move(lhs));
            if (!accept(Tok::LParen)) fail("expected '(' after in");
            if (peek().kind != Tok::RParen) {
                do {
                    e->children.push_back(additive());
                } while (accept(Tok::Comma));
            }
            if (!accept(Tok::RParen)) fail("expected ')' to close the in list");
            return e;
        }
        return lhs;
    }

    ExprPtr additive() {
        ExprPtr lhs = multiplicative();
        for (;;) {
            const std::size_t at = peek().offset;
            if (accept_op("+")) {
                lhs = binary(BinaryOp::Add, std::move(lhs), multiplicative(), at);
            } else if (accept_op("-")) {
                lhs = binary(BinaryOp::Sub, std::move(lhs), multiplicative(), at);
            } else {
                return lhs;
            }
        }
    }

    ExprPtr multiplicative() {
        ExprPtr lhs = unary();
        for (;;) {
            const std::size_t at = peek().offset;
            if (accept_op("*")) {
                lhs = binary(BinaryOp::Mul, std::move(lhs), unary(), at);
            } else if (accept_op("/")) {
                lhs = binary(BinaryOp::Div, std::move(lhs), unary(), at);
            } else if (accept_op("%")) {
                lhs = binary(BinaryOp::Mod, std::move(lhs), unary(), at);
            } else {
                return lhs;
            }
        }
    }

    ExprPtr unary() {
        const std::size_t at = peek().offset;
        if (accept_op("-")) {
            if (++depth_ > kMaxDepth) fail("expression is nested too deeply");
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Kind::Unary;
            e->unary_op = UnaryOp::Neg;
            e->offset = at;
            e->children.push_back(unary());
            --depth_;
            return e;
        }
        return primary();
    }

    ExprPtr primary() {
        const Token& t = peek();
        switch (t.kind) {
            case Tok::Number: advance(); return Expr::make_literal(t.value, t.offset);
            case Tok::Duration: {
                advance();
                ExprPtr e = Expr::make_literal(t.value, t.offset);
                e->is_duration = true;
                return e;
            }
            case Tok::String: advance(); return Expr::make_literal(Value::string(t.text), t.offset);
            case Tok::LParen: {
                advance();
                ExprPtr e = expression();
                if (!accept(Tok::RParen)) fail("expected ')'");
                return e;
            }
            case Tok::Ident: {
                advance();
                if (!t.quoted) {
                    const std::string w = to_lower(t.text);
                    if (w == "true" || w == "false") return Expr::make_literal(Value::boolean(w == "true"), t.offset);
                    if (w == "null") return Expr::make_literal(Value{}, t.offset);
                    if (peek().kind == Tok::LParen && peek().offset == t.end) return call(t, w);
                }
                return Expr::make_field(t.text, t.offset);
            }
            default: break;
        }
        if (t.kind == Tok::End) fail("unexpected end of query");
        fail("unexpected '" + t.text + "'");
    }

    ExprPtr call(const Token& name_tok, const std::string& name) {
        if (++depth_ > kMaxDepth) fail("expression is nested too deeply");
        const FunctionDef* fn = find_function(name);
        if (!fn) {
            if (is_aggregate_function(name)) {
                fail_at("'" + name + "' is an aggregate; use it in a stats stage", name_tok.offset);
            }
            fail_at("unknown function '" + name_tok.text + "'", name_tok.offset);
        }
        advance();  // '('
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::Call;
        e->name = name;
        e->offset = name_tok.offset;
        if (peek().kind != Tok::RParen) {
            do {
                e->children.push_back(expression());
            } while (accept(Tok::Comma));
        }
        if (!accept(Tok::RParen)) fail("expected ')' to close " + name + "(");
        const auto argc = static_cast<int>(e->children.size());
        if (argc < fn->min_args || (fn->max_args >= 0 && argc > fn->max_args)) {
            fail_at(name + "() takes " +
                        (fn->min_args == fn->max_args ? std::to_string(fn->min_args)
                                                      : std::to_string(fn->min_args) + "-" +
                                                            (fn->max_args < 0 ? std::string("any")
                                                                              : std::to_string(fn->max_args))) +
                        " argument(s), got " + std::to_string(argc),
                    name_tok.offset);
        }
        --depth_;
        return e;
    }

    std::vector<Token> tokens_;
    std::size_t pos_ = 0;
    std::size_t depth_ = 0;
};

}  // namespace

std::string to_string(const Expr& e) {
    std::string out;
    write_expr(out, e);
    return out;
}

bool is_constant(const Expr& e) {
    if (e.kind == Expr::Kind::Field) return false;
    if (e.kind == Expr::Kind::Call && e.name == "now") return true;
    for (const auto& c : e.children) {
        if (!is_constant(*c)) return false;
    }
    return true;
}

Query parse_query(std::string_view text) { return Parser(text).parse(text); }

std::string describe(const Query& q) {
    if (q.stages.empty()) return "(all records)\n";
    std::string out;
    std::size_t n = 0;
    for (const Stage& s : q.stages) {
        out += std::to_string(++n) + ". " + std::string(stage_name(s.kind));
        switch (s.kind) {
            case Stage::Kind::Where: out += " " + to_string(*s.predicate); break;
            case Stage::Kind::Eval:
                for (std::size_t i = 0; i < s.assignments.size(); ++i) {
                    out += i ? ", " : " ";
                    out += field_text(s.assignments[i].first) + " = " + to_string(*s.assignments[i].second);
                }
                break;
            case Stage::Kind::Fields:
                out += s.exclude ? " -" : "";
                for (std::size_t i = 0; i < s.names.size(); ++i) out += (i ? ", " : " ") + s.names[i];
                break;
            case Stage::Kind::Rename:
                for (std::size_t i = 0; i < s.renames.size(); ++i) {
                    out += (i ? ", " : " ") + s.renames[i].first + " as " + s.renames[i].second;
                }
                break;
            case Stage::Kind::Sort:
                for (std::size_t i = 0; i < s.sort_keys.size(); ++i) {
                    out += (i ? ", " : " ") + std::string(s.sort_keys[i].descending ? "-" : "") + s.sort_keys[i].field;
                }
                break;
            case Stage::Kind::Head:
            case Stage::Kind::Tail: out += " " + std::to_string(s.count); break;
            case Stage::Kind::Dedup:
                for (std::size_t i = 0; i < s.names.size(); ++i) out += (i ? ", " : " ") + s.names[i];
                break;
            case Stage::Kind::Stats:
                for (std::size_t i = 0; i < s.aggregates.size(); ++i) {
                    const Aggregate& a = s.aggregates[i];
                    out += i ? ", " : " ";
                    out += a.function + "(" + (a.argument ? to_string(*a.argument) : "") +
                           (a.function == "percentile" ? ", " + format_double(a.parameter) : "") + ") as " + a.alias;
                }
                if (!s.names.empty()) {
                    out += " by";
                    for (std::size_t i = 0; i < s.names.size(); ++i) out += (i ? ", " : " ") + s.names[i];
                }
                break;
            case Stage::Kind::Bin:
                out += " " + s.field + " span=" +
                       (s.span_is_duration ? format_duration(s.duration_span) : format_double(s.numeric_span));
                if (s.alias != s.field) out += " as " + s.alias;
                break;
        }
        out += '\n';
    }
    return out;
}

}  // namespace logsift
