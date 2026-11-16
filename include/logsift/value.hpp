// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Field values: null, boolean, integer, floating point, string or timestamp.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "logsift/time.hpp"

namespace logsift {

enum class Type : std::uint8_t { Null, Bool, Int, Float, String, Time };

std::string_view type_name(Type t) noexcept;

class Value {
public:
    Value() = default;  // null

    static Value boolean(bool v);
    static Value integer(std::int64_t v);
    static Value real(double v);
    static Value string(std::string v);
    static Value time(TimePoint t);

    Type type() const noexcept;
    bool is_null() const noexcept { return type() == Type::Null; }
    bool is_bool() const noexcept { return type() == Type::Bool; }
    bool is_int() const noexcept { return type() == Type::Int; }
    bool is_float() const noexcept { return type() == Type::Float; }
    bool is_number() const noexcept { return is_int() || is_float(); }
    bool is_string() const noexcept { return type() == Type::String; }
    bool is_time() const noexcept { return type() == Type::Time; }

    // Accessors; calling one for the wrong type throws std::bad_variant_access.
    bool as_bool() const;
    std::int64_t as_int() const;
    double as_float() const;
    const std::string& as_string() const;
    TimePoint as_time() const;

    /// Numeric view: ints and floats; strings that parse as numbers; booleans
    /// as 0/1. nullopt otherwise.
    std::optional<double> to_number() const;

    /// Time view: times; strings in a recognised timestamp format.
    std::optional<TimePoint> to_time() const;

    /// Display form: "null" is empty, floats use the shortest round-trip
    /// text, times are RFC 3339.
    std::string to_string() const;

    /// false for null, false, 0, 0.0 and ""; true otherwise.
    bool truthy() const;

    /// Exact structural equality (Int 1 != Float 1.0).
    friend bool operator==(const Value& a, const Value& b) = default;

private:
    std::variant<std::monostate, bool, std::int64_t, double, std::string, TimePoint> data_;
};

/// Total order used by sort, dedup and group-by: null < bool < numbers <
/// times < strings. Ints and floats compare numerically; strings compare
/// byte-wise.
int compare_total(const Value& a, const Value& b);

/// Comparison used by query operators. Numbers compare numerically (a
/// string that parses as a number counts as one when the other side is
/// numeric); times compare with times and with timestamp strings; strings
/// compare byte-wise; booleans compare with booleans and the strings
/// "true"/"false". Returns nullopt when the values are not comparable,
/// including whenever either side is null.
std::optional<int> compare_query(const Value& a, const Value& b);

/// Equality for the == operator: null == null is true; otherwise
/// compare_query(a, b) == 0.
bool equals_query(const Value& a, const Value& b);

/// Key under which a value is stored in an equality index. Values that are
/// equal under equals_query() always share a key.
std::string index_key(const Value& v);

/// Infers a typed value from text: integers, floats, true/false, else string.
Value infer_value(std::string_view text);

/// Shortest decimal text that round-trips to the same double, exactly as
/// std::to_chars writes it, on every platform.
std::string format_double(double v);

namespace detail {

/// The implementation format_double uses where the standard library has no
/// std::to_chars for double (libc++ on macOS, for one). Always compiled so
/// that tests can check it against std::to_chars where that exists.
std::string format_double_printf(double v);

}  // namespace detail

/// Strict, locale-independent decimal parsing (no surrounding spaces).
std::optional<double> parse_double(std::string_view text) noexcept;
std::optional<std::int64_t> parse_int(std::string_view text) noexcept;

}  // namespace logsift
