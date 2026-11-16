// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Expression evaluation and scalar functions.

#include "logsift/eval.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "logsift/text.hpp"

namespace logsift {

namespace {

// ---- Arithmetic -------------------------------------------------------------------

bool add_overflows(std::int64_t a, std::int64_t b, std::int64_t& out) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_add_overflow(a, b, &out);
#else
    if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
        (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b)) {
        return true;
    }
    out = a + b;
    return false;
#endif
}

bool mul_overflows(std::int64_t a, std::int64_t b, std::int64_t& out) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_mul_overflow(a, b, &out);
#else
    if (a == 0 || b == 0) {
        out = 0;
        return false;
    }
    const long double p = static_cast<long double>(a) * static_cast<long double>(b);
    if (p > static_cast<long double>(std::numeric_limits<std::int64_t>::max()) ||
        p < static_cast<long double>(std::numeric_limits<std::int64_t>::min())) {
        return true;
    }
    out = a * b;
    return false;
#endif
}

Value real_or_null(double v) { return std::isfinite(v) ? Value::real(v) : Value{}; }

Value arithmetic(BinaryOp op, const Value& a, const Value& b) {
    if (a.is_null() || b.is_null()) return {};

    // Time arithmetic: time - time = duration (ns); time +/- duration = time.
    if (a.is_time() && b.is_time()) {
        if (op != BinaryOp::Sub) return {};
        return Value::integer(a.as_time().ns - b.as_time().ns);
    }
    if (a.is_time() && b.is_int() && (op == BinaryOp::Add || op == BinaryOp::Sub)) {
        std::int64_t out = 0;
        if (add_overflows(a.as_time().ns, op == BinaryOp::Add ? b.as_int() : -b.as_int(), out)) return {};
        return Value::time(TimePoint{out});
    }
    if (a.is_time() || b.is_time()) return {};

    if (a.is_int() && b.is_int()) {
        const std::int64_t x = a.as_int();
        const std::int64_t y = b.as_int();
        std::int64_t out = 0;
        switch (op) {
            case BinaryOp::Add:
                if (!add_overflows(x, y, out)) return Value::integer(out);
                break;
            case BinaryOp::Sub:
                if (y != std::numeric_limits<std::int64_t>::min() && !add_overflows(x, -y, out)) {
                    return Value::integer(out);
                }
                break;
            case BinaryOp::Mul:
                if (!mul_overflows(x, y, out)) return Value::integer(out);
                break;
            case BinaryOp::Mod:
                if (y == 0) return {};
                if (y == -1) return Value::integer(0);
                return Value::integer(x % y);
            default: break;  // Div falls through to floating point
        }
    }
    const auto x = a.to_number();
    const auto y = b.to_number();
    if (!x || !y || a.is_bool() || b.is_bool()) return {};
    switch (op) {
        case BinaryOp::Add: return real_or_null(*x + *y);
        case BinaryOp::Sub: return real_or_null(*x - *y);
        case BinaryOp::Mul: return real_or_null(*x * *y);
        case BinaryOp::Div: return *y == 0 ? Value{} : real_or_null(*x / *y);
        case BinaryOp::Mod: return *y == 0 ? Value{} : real_or_null(std::fmod(*x, *y));
        default: return {};
    }
}

bool regex_matches(const Expr& e, const Value& subject, const Value& pattern) {
    if (subject.is_null()) return false;
    const std::string text = subject.to_string();
    if (e.regex) return std::regex_search(text, *e.regex);
    if (!pattern.is_string()) return false;
    try {
        return std::regex_search(text, std::regex(pattern.as_string(), std::regex::ECMAScript));
    } catch (const std::regex_error&) {
        return false;
    }
}

}  // namespace

Value evaluate(const Expr& e, const Record& r, const EvalContext& ctx) {
    switch (e.kind) {
        case Expr::Kind::Literal: return e.literal;
        case Expr::Kind::Field: return r.get(e.name);
        case Expr::Kind::Unary: {
            const Value v = evaluate(*e.children[0], r, ctx);
            if (e.unary_op == UnaryOp::Not) return Value::boolean(!v.truthy());
            if (v.is_int() && v.as_int() != std::numeric_limits<std::int64_t>::min()) return Value::integer(-v.as_int());
            if (v.is_float()) return Value::real(-v.as_float());
            if (v.is_string()) {
                if (auto n = v.to_number()) return Value::real(-*n);
            }
            return {};
        }
        case Expr::Kind::Binary: {
            switch (e.binary_op) {
                case BinaryOp::And:
                    return Value::boolean(evaluate(*e.children[0], r, ctx).truthy() &&
                                          evaluate(*e.children[1], r, ctx).truthy());
                case BinaryOp::Or:
                    return Value::boolean(evaluate(*e.children[0], r, ctx).truthy() ||
                                          evaluate(*e.children[1], r, ctx).truthy());
                default: break;
            }
            const Value a = evaluate(*e.children[0], r, ctx);
            const Value b = evaluate(*e.children[1], r, ctx);
            switch (e.binary_op) {
                case BinaryOp::Eq: return Value::boolean(equals_query(a, b));
                case BinaryOp::Ne: return Value::boolean(!equals_query(a, b));
                case BinaryOp::Lt:
                case BinaryOp::Le:
                case BinaryOp::Gt:
                case BinaryOp::Ge: {
                    const auto c = compare_query(a, b);
                    if (!c) return Value::boolean(false);
                    switch (e.binary_op) {
                        case BinaryOp::Lt: return Value::boolean(*c < 0);
                        case BinaryOp::Le: return Value::boolean(*c <= 0);
                        case BinaryOp::Gt: return Value::boolean(*c > 0);
                        default: return Value::boolean(*c >= 0);
                    }
                }
                case BinaryOp::Match: return Value::boolean(regex_matches(e, a, b));
                case BinaryOp::NotMatch: return Value::boolean(!a.is_null() && !regex_matches(e, a, b));
                default: return arithmetic(e.binary_op, a, b);
            }
        }
        case Expr::Kind::In: {
            const Value subject = evaluate(*e.children[0], r, ctx);
            bool found = false;
            for (std::size_t i = 1; i < e.children.size() && !found; ++i) {
                found = equals_query(subject, evaluate(*e.children[i], r, ctx));
            }
            return Value::boolean(found != e.negated);
        }
        case Expr::Kind::Call: {
            const FunctionDef* fn = find_function(e.name);
            if (!fn) return {};
            std::vector<Value> args;
            args.reserve(e.children.size());
            for (const auto& c : e.children) args.push_back(evaluate(*c, r, ctx));
            return fn->fn(args, ctx);
        }
    }
    return {};
}

bool matches(const Expr& e, const Record& r, const EvalContext& ctx) { return evaluate(e, r, ctx).truthy(); }

// ---- Scalar functions ----------------------------------------------------------------

namespace {

using Args = std::span<const Value>;

std::optional<std::string> str(const Value& v) {
    if (v.is_null()) return std::nullopt;
    return v.to_string();
}

Value fn_lower(Args a, const EvalContext&) {
    auto s = str(a[0]);
    return s ? Value::string(to_lower(*s)) : Value{};
}

Value fn_upper(Args a, const EvalContext&) {
    auto s = str(a[0]);
    return s ? Value::string(to_upper(*s)) : Value{};
}

Value fn_len(Args a, const EvalContext&) {
    auto s = str(a[0]);
    return s ? Value::integer(static_cast<std::int64_t>(display_width(*s))) : Value{};
}

Value fn_trim(Args a, const EvalContext&) {
    auto s = str(a[0]);
    return s ? Value::string(std::string(trim(*s))) : Value{};
}

template <bool (*Test)(std::string_view, std::string_view)>
Value text_test(Args a, const EvalContext&) {
    auto s = str(a[0]);
    auto t = str(a[1]);
    if (!s || !t) return Value::boolean(false);
    return Value::boolean(Test(*s, *t));
}

bool contains_text(std::string_view s, std::string_view t) { return s.find(t) != std::string_view::npos; }
bool starts_text(std::string_view s, std::string_view t) { return starts_with(s, t); }
bool ends_text(std::string_view s, std::string_view t) { return ends_with(s, t); }

Value fn_substr(Args a, const EvalContext&) {
    auto s = str(a[0]);
    auto start = a[1].to_number();
    if (!s || !start || *start < 0) return {};
    const auto b = static_cast<std::size_t>(std::min(*start, 1e9));
    if (b >= s->size()) return Value::string("");
    std::size_t n = std::string::npos;
    if (a.size() > 2) {
        auto len = a[2].to_number();
        if (!len || *len < 0) return {};
        n = static_cast<std::size_t>(std::min(*len, 1e9));
    }
    return Value::string(s->substr(b, n));
}

Value fn_replace(Args a, const EvalContext&) {
    auto s = str(a[0]);
    auto from = str(a[1]);
    auto to = str(a[2]);
    if (!s || !from || !to) return {};
    if (from->empty()) return Value::string(*s);
    std::string out;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t hit = s->find(*from, pos);
        if (hit == std::string::npos) break;
        out.append(*s, pos, hit - pos);
        out += *to;
        pos = hit + from->size();
    }
    out.append(*s, pos, std::string::npos);
    return Value::string(std::move(out));
}

Value fn_concat(Args a, const EvalContext&) {
    std::string out;
    for (const Value& v : a) out += v.to_string();
    return Value::string(std::move(out));
}

Value fn_split_part(Args a, const EvalContext&) {
    auto s = str(a[0]);
    auto sep = str(a[1]);
    auto n = a[2].to_number();
    if (!s || !sep || sep->size() != 1 || !n || *n < 1) return {};
    const auto parts = split(*s, (*sep)[0]);
    const auto i = static_cast<std::size_t>(*n) - 1;
    if (i >= parts.size()) return {};
    return Value::string(std::string(parts[i]));
}

Value fn_extract(Args a, const EvalContext&) {
    auto s = str(a[0]);
    auto pattern = str(a[1]);
    if (!s || !pattern) return {};
    std::size_t group = 1;
    if (a.size() > 2) {
        auto g = a[2].to_number();
        if (!g || *g < 0) return {};
        group = static_cast<std::size_t>(*g);
    }
    try {
        std::smatch m;
        const std::regex re(*pattern, std::regex::ECMAScript);
        if (!std::regex_search(*s, m, re) || group >= m.size() || !m[group].matched) return {};
        return Value::string(m[group].str());
    } catch (const std::regex_error&) {
        return {};
    }
}

Value fn_match(Args a, const EvalContext&) {
    auto s = str(a[0]);
    auto pattern = str(a[1]);
    if (!s || !pattern) return Value::boolean(false);
    try {
        return Value::boolean(std::regex_search(*s, std::regex(*pattern, std::regex::ECMAScript)));
    } catch (const std::regex_error&) {
        return Value::boolean(false);
    }
}

Value fn_tonumber(Args a, const EvalContext&) {
    const Value& v = a[0];
    if (v.is_number()) return v;
    if (v.is_string()) {
        const std::string_view t = trim(v.as_string());
        if (auto i = parse_int(t)) return Value::integer(*i);
        if (auto d = parse_double(t)) return Value::real(*d);
    }
    if (v.is_bool()) return Value::integer(v.as_bool() ? 1 : 0);
    return {};
}

Value fn_toint(Args a, const EvalContext& ctx) {
    const Value n = fn_tonumber(a, ctx);
    if (n.is_int()) return n;
    if (n.is_float() && std::fabs(n.as_float()) < 9.2e18) return Value::integer(static_cast<std::int64_t>(n.as_float()));
    return {};
}

Value fn_tostring(Args a, const EvalContext&) {
    if (a[0].is_null()) return {};
    return Value::string(a[0].to_string());
}

Value fn_totime(Args a, const EvalContext&) {
    const Value& v = a[0];
    if (v.is_time()) return v;
    if (v.is_string()) {
        if (auto t = parse_time(v.as_string())) return Value::time(*t);
        if (auto n = parse_double(v.as_string())) {
            if (auto t = parse_epoch(*n)) return Value::time(*t);
        }
        return {};
    }
    if (v.is_number()) {
        if (auto t = parse_epoch(*v.to_number())) return Value::time(*t);
    }
    return {};
}

Value fn_typeof(Args a, const EvalContext&) { return Value::string(std::string(type_name(a[0].type()))); }

Value fn_isnull(Args a, const EvalContext&) { return Value::boolean(a[0].is_null()); }
Value fn_isnotnull(Args a, const EvalContext&) { return Value::boolean(!a[0].is_null()); }

Value fn_coalesce(Args a, const EvalContext&) {
    for (const Value& v : a) {
        if (!v.is_null()) return v;
    }
    return {};
}

Value fn_if(Args a, const EvalContext&) { return a[0].truthy() ? a[1] : (a.size() > 2 ? a[2] : Value{}); }

std::optional<double> plain_number(const Value& v) {
    if (v.is_bool()) return std::nullopt;
    return v.to_number();
}

// An integral double as Int when it fits, else Float.
Value whole(double v) {
    if (std::fabs(v) < 9.2e18) return Value::integer(static_cast<std::int64_t>(v));
    return real_or_null(v);
}

Value fn_abs(Args a, const EvalContext&) {
    if (a[0].is_int()) {
        const auto v = a[0].as_int();
        if (v == std::numeric_limits<std::int64_t>::min()) return {};
        return Value::integer(v < 0 ? -v : v);
    }
    auto x = plain_number(a[0]);
    return x ? real_or_null(std::fabs(*x)) : Value{};
}

Value fn_floor(Args a, const EvalContext&) {
    if (a[0].is_int()) return a[0];
    auto x = plain_number(a[0]);
    return x ? whole(std::floor(*x)) : Value{};
}

Value fn_ceil(Args a, const EvalContext&) {
    if (a[0].is_int()) return a[0];
    auto x = plain_number(a[0]);
    return x ? whole(std::ceil(*x)) : Value{};
}

Value fn_sqrt(Args a, const EvalContext&) {
    auto x = plain_number(a[0]);
    if (!x || *x < 0) return {};
    return real_or_null(std::sqrt(*x));
}

Value fn_round(Args a, const EvalContext&) {
    auto x = plain_number(a[0]);
    if (!x) return {};
    int digits = 0;
    if (a.size() > 1) {
        auto d = a[1].to_number();
        if (!d) return {};
        digits = static_cast<int>(std::clamp(*d, -15.0, 15.0));
    }
    if (digits == 0) {
        return whole(std::round(*x));
    }
    const double scale = std::pow(10.0, digits);
    return real_or_null(std::round(*x * scale) / scale);
}

Value extreme(Args a, bool want_max) {
    const Value* best = nullptr;
    for (const Value& v : a) {
        if (v.is_null()) continue;
        if (!best) {
            best = &v;
            continue;
        }
        const auto c = compare_query(v, *best);
        if (c && (want_max ? *c > 0 : *c < 0)) best = &v;
    }
    return best ? *best : Value{};
}

Value fn_least(Args a, const EvalContext&) { return extreme(a, false); }
Value fn_greatest(Args a, const EvalContext&) { return extreme(a, true); }

Value fn_now(Args, const EvalContext& ctx) { return Value::time(ctx.now); }

Value fn_ago(Args a, const EvalContext& ctx) {
    Duration d = 0;
    if (a[0].is_int()) {
        d = a[0].as_int();
    } else if (a[0].is_string()) {
        auto parsed = parse_duration(a[0].as_string());
        if (!parsed) return {};
        d = *parsed;
    } else {
        return {};
    }
    return Value::time(TimePoint{ctx.now.ns - d});
}

Value fn_duration(Args a, const EvalContext&) {
    if (a[0].is_int()) return a[0];
    if (!a[0].is_string()) return {};
    auto d = parse_duration(a[0].as_string());
    return d ? Value::integer(*d) : Value{};
}

Value fn_format_duration(Args a, const EvalContext&) {
    if (!a[0].is_int()) return {};
    return Value::string(format_duration(a[0].as_int()));
}

template <int CivilTime::*Member>
Value time_part(Args a, const EvalContext&) {
    auto t = a[0].to_time();
    if (!t) return {};
    return Value::integer(to_civil(*t).*Member);
}

Value fn_date(Args a, const EvalContext&) {
    auto t = a[0].to_time();
    if (!t) return {};
    return Value::string(format_time(*t).substr(0, 10));
}

Value fn_bucket(Args a, const EvalContext&) {
    auto t = a[0].to_time();
    if (!t || !a[1].is_int() || a[1].as_int() <= 0) return {};
    return Value::time(floor_time(*t, a[1].as_int()));
}

const std::vector<FunctionDef> kFunctions{
    {"lower", 1, 1, fn_lower, "lower(s): lower-case ASCII letters"},
    {"upper", 1, 1, fn_upper, "upper(s): upper-case ASCII letters"},
    {"len", 1, 1, fn_len, "len(s): length in code points"},
    {"trim", 1, 1, fn_trim, "trim(s): strip surrounding whitespace"},
    {"contains", 2, 2, text_test<contains_text>, "contains(s, sub): substring test"},
    {"startswith", 2, 2, text_test<starts_text>, "startswith(s, prefix)"},
    {"endswith", 2, 2, text_test<ends_text>, "endswith(s, suffix)"},
    {"substr", 2, 3, fn_substr, "substr(s, start[, length]): bytes, 0-based"},
    {"replace", 3, 3, fn_replace, "replace(s, from, to): replace every occurrence"},
    {"concat", 1, -1, fn_concat, "concat(a, b, ...): join values as text"},
    {"split_part", 3, 3, fn_split_part, "split_part(s, sep, n): n-th piece (1-based) split on a 1-char separator"},
    {"extract", 2, 3, fn_extract, "extract(s, regex[, group]): capture group (default 1)"},
    {"match", 2, 2, fn_match, "match(s, regex): regex search"},
    {"tonumber", 1, 1, fn_tonumber, "tonumber(x): int or float, null if not numeric"},
    {"toint", 1, 1, fn_toint, "toint(x): integer, truncating"},
    {"tostring", 1, 1, fn_tostring, "tostring(x)"},
    {"totime", 1, 1, fn_totime, "totime(x): timestamp text or Unix epoch to time"},
    {"typeof", 1, 1, fn_typeof, "typeof(x): null, bool, int, float, string or time"},
    {"isnull", 1, 1, fn_isnull, "isnull(x)"},
    {"isnotnull", 1, 1, fn_isnotnull, "isnotnull(x)"},
    {"exists", 1, 1, fn_isnotnull, "exists(field): the field is present and not null"},
    {"coalesce", 1, -1, fn_coalesce, "coalesce(a, b, ...): first non-null value"},
    {"if", 2, 3, fn_if, "if(condition, then[, else])"},
    {"abs", 1, 1, fn_abs, "abs(x)"},
    {"floor", 1, 1, fn_floor, "floor(x): integer"},
    {"ceil", 1, 1, fn_ceil, "ceil(x): integer"},
    {"sqrt", 1, 1, fn_sqrt, "sqrt(x)"},
    {"round", 1, 2, fn_round, "round(x[, digits])"},
    {"least", 1, -1, fn_least, "least(a, b, ...): smallest non-null value"},
    {"greatest", 1, -1, fn_greatest, "greatest(a, b, ...): largest non-null value"},
    {"now", 0, 0, fn_now, "now(): query start time"},
    {"ago", 1, 1, fn_ago, "ago(duration): now() minus a duration such as 1h"},
    {"duration", 1, 1, fn_duration, "duration(text): \"1h30m\" to nanoseconds"},
    {"format_duration", 1, 1, fn_format_duration, "format_duration(ns): nanoseconds to \"1h30m\""},
    {"year", 1, 1, time_part<&CivilTime::year>, "year(t)"},
    {"month", 1, 1, time_part<&CivilTime::month>, "month(t): 1-12"},
    {"day", 1, 1, time_part<&CivilTime::day>, "day(t): 1-31"},
    {"hour", 1, 1, time_part<&CivilTime::hour>, "hour(t): 0-23 (UTC)"},
    {"minute", 1, 1, time_part<&CivilTime::minute>, "minute(t)"},
    {"second", 1, 1, time_part<&CivilTime::second>, "second(t)"},
    {"weekday", 1, 1, time_part<&CivilTime::weekday>, "weekday(t): 0 = Sunday"},
    {"date", 1, 1, fn_date, "date(t): \"YYYY-MM-DD\""},
    {"bucket", 2, 2, fn_bucket, "bucket(t, span): floor a time to a multiple of span"},
};

constexpr std::array<std::string_view, 17> kAggregates{"count", "sum",   "avg",    "min",    "max", "dc",
                                                       "values", "first", "last",   "median", "p50", "p90",
                                                       "p95",   "p99",   "percentile", "stdev", "range"};

}  // namespace

const FunctionDef* find_function(std::string_view lower_name) {
    for (const auto& f : kFunctions) {
        if (f.name == lower_name) return &f;
    }
    return nullptr;
}

const std::vector<FunctionDef>& function_table() { return kFunctions; }

bool is_aggregate_function(std::string_view lower_name) {
    return std::find(kAggregates.begin(), kAggregates.end(), lower_name) != kAggregates.end();
}

std::vector<std::string_view> aggregate_function_names() { return {kAggregates.begin(), kAggregates.end()}; }

}  // namespace logsift
