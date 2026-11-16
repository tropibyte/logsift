// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "logsift/value.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <locale>
#include <sstream>
#include <system_error>

#include "logsift/text.hpp"

namespace logsift {

std::string_view type_name(Type t) noexcept {
    switch (t) {
        case Type::Null: return "null";
        case Type::Bool: return "bool";
        case Type::Int: return "int";
        case Type::Float: return "float";
        case Type::String: return "string";
        case Type::Time: return "time";
    }
    return "unknown";
}

Value Value::boolean(bool v) {
    Value r;
    r.data_ = v;
    return r;
}

Value Value::integer(std::int64_t v) {
    Value r;
    r.data_ = v;
    return r;
}

Value Value::real(double v) {
    Value r;
    r.data_ = v;
    return r;
}

Value Value::string(std::string v) {
    Value r;
    r.data_ = std::move(v);
    return r;
}

Value Value::time(TimePoint t) {
    Value r;
    r.data_ = t;
    return r;
}

Type Value::type() const noexcept {
    switch (data_.index()) {
        case 1: return Type::Bool;
        case 2: return Type::Int;
        case 3: return Type::Float;
        case 4: return Type::String;
        case 5: return Type::Time;
        default: return Type::Null;
    }
}

bool Value::as_bool() const { return std::get<bool>(data_); }
std::int64_t Value::as_int() const { return std::get<std::int64_t>(data_); }
double Value::as_float() const { return std::get<double>(data_); }
const std::string& Value::as_string() const { return std::get<std::string>(data_); }
TimePoint Value::as_time() const { return std::get<TimePoint>(data_); }

std::optional<double> Value::to_number() const {
    switch (type()) {
        case Type::Int: return static_cast<double>(as_int());
        case Type::Float: return as_float();
        case Type::Bool: return as_bool() ? 1.0 : 0.0;
        case Type::String: return parse_double(as_string());
        default: return std::nullopt;
    }
}

std::optional<TimePoint> Value::to_time() const {
    if (is_time()) return as_time();
    if (is_string()) return parse_time(as_string());
    return std::nullopt;
}

std::string Value::to_string() const {
    switch (type()) {
        case Type::Null: return "";
        case Type::Bool: return as_bool() ? "true" : "false";
        case Type::Int: return std::to_string(as_int());
        case Type::Float: return format_double(as_float());
        case Type::String: return as_string();
        case Type::Time: return format_time(as_time());
    }
    return "";
}

bool Value::truthy() const {
    switch (type()) {
        case Type::Null: return false;
        case Type::Bool: return as_bool();
        case Type::Int: return as_int() != 0;
        case Type::Float: return as_float() != 0.0;
        case Type::String: return !as_string().empty();
        case Type::Time: return true;
    }
    return false;
}

namespace {

int rank(Type t) {
    switch (t) {
        case Type::Null: return 0;
        case Type::Bool: return 1;
        case Type::Int:
        case Type::Float: return 2;
        case Type::Time: return 3;
        case Type::String: return 4;
    }
    return 5;
}

template <class T>
int three_way(const T& a, const T& b) {
    return a < b ? -1 : (b < a ? 1 : 0);
}

int compare_numbers(const Value& a, const Value& b) {
    if (a.is_int() && b.is_int()) return three_way(a.as_int(), b.as_int());
    return three_way(*a.to_number(), *b.to_number());
}

std::optional<bool> bool_text(const std::string& s) {
    if (s == "true") return true;
    if (s == "false") return false;
    return std::nullopt;
}

std::string number_key(double v) {
    if (v == 0) return "n:0";
    if (std::trunc(v) == v && std::fabs(v) < 9.0e18) return "n:" + std::to_string(static_cast<std::int64_t>(v));
    return "n:" + format_double(v);
}

}  // namespace

int compare_total(const Value& a, const Value& b) {
    const int ra = rank(a.type());
    const int rb = rank(b.type());
    if (ra != rb) return ra < rb ? -1 : 1;
    switch (a.type()) {
        case Type::Null: return 0;
        case Type::Bool: return three_way(a.as_bool(), b.as_bool());
        case Type::Int:
        case Type::Float: return compare_numbers(a, b);
        case Type::Time: return three_way(a.as_time(), b.as_time());
        case Type::String: {
            const int c = a.as_string().compare(b.as_string());
            return c < 0 ? -1 : (c > 0 ? 1 : 0);
        }
    }
    return 0;
}

std::optional<int> compare_query(const Value& a, const Value& b) {
    if (a.is_null() || b.is_null()) return std::nullopt;
    if (a.is_number() && b.is_number()) return compare_numbers(a, b);
    if (a.is_number() && b.is_string()) {
        const auto y = b.to_number();
        if (!y) return std::nullopt;
        return three_way(*a.to_number(), *y);
    }
    if (a.is_string() && b.is_number()) {
        const auto x = a.to_number();
        if (!x) return std::nullopt;
        return three_way(*x, *b.to_number());
    }
    if (a.is_time() || b.is_time()) {
        if (a.is_number() || b.is_number() || a.is_bool() || b.is_bool()) return std::nullopt;
        const auto x = a.to_time();
        const auto y = b.to_time();
        if (!x || !y) return std::nullopt;
        return three_way(*x, *y);
    }
    if (a.is_string() && b.is_string()) {
        const int c = a.as_string().compare(b.as_string());
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    if (a.is_bool() && b.is_bool()) return three_way(a.as_bool(), b.as_bool());
    if (a.is_bool() && b.is_string()) {
        const auto y = bool_text(b.as_string());
        if (!y) return std::nullopt;
        return three_way(a.as_bool(), *y);
    }
    if (a.is_string() && b.is_bool()) {
        const auto x = bool_text(a.as_string());
        if (!x) return std::nullopt;
        return three_way(*x, b.as_bool());
    }
    return std::nullopt;
}

bool equals_query(const Value& a, const Value& b) {
    if (a.is_null() || b.is_null()) return a.is_null() && b.is_null();
    const auto c = compare_query(a, b);
    return c && *c == 0;
}

std::string index_key(const Value& v) {
    switch (v.type()) {
        case Type::Null: return "z:";
        case Type::Bool: return v.as_bool() ? "b:true" : "b:false";
        case Type::Int:
        case Type::Float: return number_key(*v.to_number());
        case Type::Time: return "t:" + std::to_string(v.as_time().ns);
        case Type::String: {
            const std::string& s = v.as_string();
            if (auto n = parse_double(s)) return number_key(*n);
            if (auto t = parse_time(s)) return "t:" + std::to_string(t->ns);
            if (auto b = bool_text(s)) return *b ? "b:true" : "b:false";
            return "s:" + s;
        }
    }
    return "";
}

Value infer_value(std::string_view text) {
    if (auto i = parse_int(text)) return Value::integer(*i);
    if (auto d = parse_double(text)) return Value::real(*d);
    if (text == "true") return Value::boolean(true);
    if (text == "false") return Value::boolean(false);
    return Value::string(std::string(text));
}

std::optional<double> parse_double(std::string_view s) noexcept {
    const std::size_t n = s.size();
    std::size_t i = 0;
    if (i < n && (s[i] == '+' || s[i] == '-')) ++i;
    std::size_t digits = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') ++i, ++digits;
    if (i < n && s[i] == '.') {
        ++i;
        while (i < n && s[i] >= '0' && s[i] <= '9') ++i, ++digits;
    }
    if (digits == 0) return std::nullopt;
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < n && (s[i] == '+' || s[i] == '-')) ++i;
        std::size_t exp_digits = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') ++i, ++exp_digits;
        if (exp_digits == 0) return std::nullopt;
    }
    if (i != n) return std::nullopt;
    const char* begin = s.data();
    if (*begin == '+') ++begin;
    double value = 0;
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    const auto [ptr, ec] = std::from_chars(begin, s.data() + n, value);
    if (ec != std::errc{} || ptr != s.data() + n) return std::nullopt;
#else
    try {
        std::istringstream in{std::string(begin, s.data() + n)};
        in.imbue(std::locale::classic());
        in >> value;
        if (in.fail()) return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
#endif
    if (!std::isfinite(value)) return std::nullopt;
    return value;
}

std::optional<std::int64_t> parse_int(std::string_view s) noexcept {
    if (s.empty()) return std::nullopt;
    std::size_t i = 0;
    bool negative = false;
    if (s[0] == '+' || s[0] == '-') {
        negative = s[0] == '-';
        i = 1;
    }
    if (i == s.size()) return std::nullopt;
    std::uint64_t v = 0;
    constexpr std::uint64_t kLimit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
        if (v > kLimit + (negative ? 1 : 0)) return std::nullopt;
    }
    if (negative) {
        if (v == kLimit + 1) return std::numeric_limits<std::int64_t>::min();
        return -static_cast<std::int64_t>(v);
    }
    return static_cast<std::int64_t>(v);
}

std::string format_double(double v) {
    if (v == 0) return "0";
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v > 0 ? "inf" : "-inf";
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    char buf[64];
    const auto [ptr, ec] = std::to_chars(buf, buf + sizeof buf, v);
    if (ec == std::errc{}) return std::string(buf, ptr);
#endif
    return detail::format_double_printf(v);
}

namespace detail {

namespace {

// A decimal d.ddd x 10^exponent, kept as its significant digits.
struct Decimal {
    std::string digits;  // no sign, no point, digits[0] != '0'
    int exponent = 0;

    std::string scientific() const {  // "d.ddde+XX", as printf %e writes it
        std::string out(1, digits[0]);
        if (digits.size() > 1) out += "." + digits.substr(1);
        out += exponent < 0 ? "e-" : "e+";
        const int magnitude = exponent < 0 ? -exponent : exponent;
        if (magnitude < 10) out += '0';
        out += std::to_string(magnitude);
        return out;
    }

    // The neighbouring decimal with the same number of digits, one unit in
    // the last place up or down.
    Decimal step(bool up) const {
        Decimal d = *this;
        std::size_t i = d.digits.size();
        while (i-- > 0) {
            char& c = d.digits[i];
            if (up ? c < '9' : c > '0') {
                c = static_cast<char>(up ? c + 1 : c - 1);
                break;
            }
            c = up ? '0' : '9';
            if (i == 0) break;
        }
        if (up && d.digits[0] == '0') {  // 9.99 -> 10.0: carry out of the top
            d.digits.insert(d.digits.begin(), '1');
            d.digits.pop_back();
            ++d.exponent;
        } else if (!up && d.digits[0] == '0') {  // 1.00 -> 0.99: borrow from the top
            d.digits.erase(d.digits.begin());
            d.digits += '9';
            --d.exponent;
        }
        return d;
    }
};

bool reads_back_as(const Decimal& d, bool negative, double value) {
    const auto back = parse_double((negative ? "-" : "") + d.scientific());
    return back && *back == value;
}

}  // namespace

std::string format_double_printf(double value) {
    if (value == 0.0) return "0";
    if (std::isnan(value)) return "nan";
    if (std::isinf(value)) return value < 0 ? "-inf" : "inf";
    const bool negative = value < 0;

    // Fewest significant digits that read back as the same double, as
    // std::to_chars chooses them. %.*e gives the correctly rounded decimal
    // for each digit count. At a power of two the doubles below are twice
    // as dense as those above, so that decimal can miss while its
    // neighbour in the last digit still reads back; nothing further away
    // can work if the neighbours don't. 17 digits always suffice.
    Decimal best;
    for (int count = 1; count <= 17; ++count) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.*e", count - 1, negative ? -value : value);
        const std::string text(buf);
        const std::size_t epos = text.find('e');
        Decimal d;
        for (std::size_t i = 0; i < epos; ++i) {
            if (text[i] >= '0' && text[i] <= '9') d.digits += text[i];  // skips the point, whatever the locale
        }
        d.exponent = std::atoi(text.c_str() + epos + 1);
        best = d;
        if (reads_back_as(d, negative, value)) break;
        if (const Decimal up = d.step(true); reads_back_as(up, negative, value)) {
            best = up;
            break;
        }
        if (const Decimal down = d.step(false); reads_back_as(down, negative, value)) {
            best = down;
            break;
        }
    }
    std::string& digits = best.digits;
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    const int exponent = best.exponent;

    // The same digits in both notations; std::to_chars picks the shorter,
    // preferring fixed on a tie.
    const std::string scientific = best.scientific();
    std::string fixed;
    if (exponent < 0) {
        fixed = "0." + std::string(static_cast<std::size_t>(-exponent - 1), '0') + digits;
    } else if (digits.size() <= static_cast<std::size_t>(exponent) + 1) {
        // A whole number. Padding the digits with zeros gives the right
        // length, but above 2^53 std::to_chars writes the exact integer
        // (the same length, closer to the value): 32944984306792075264, not
        // 32944984306792075000. %.0f prints exactly that.
        char buf[400];
        std::snprintf(buf, sizeof buf, "%.0f", negative ? -value : value);
        fixed = buf;
    } else {
        const auto point = static_cast<std::size_t>(exponent) + 1;
        fixed = digits.substr(0, point) + "." + digits.substr(point);
    }

    const std::string& shortest = fixed.size() <= scientific.size() ? fixed : scientific;
    return negative ? "-" + shortest : shortest;
}

}  // namespace detail

}  // namespace logsift
