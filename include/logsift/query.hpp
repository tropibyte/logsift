// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// The query language: a pipeline of stages separated by "|".
//
//   level == "error" and status >= 500
//     | eval slow = duration_ms > 250
//     | stats count() as n, p95(duration_ms) by service
//     | sort -n
//     | head 5
//
// A segment that does not start with a stage keyword is a "where" filter.
// See docs/QUERY_LANGUAGE.md for the full reference.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "logsift/time.hpp"
#include "logsift/value.hpp"

namespace logsift {

/// Raised for malformed queries; offset() is a byte offset into the query.
class QueryError : public std::runtime_error {
public:
    QueryError(const std::string& message, std::size_t offset)
        : std::runtime_error(message), offset_(offset) {}
    std::size_t offset() const noexcept { return offset_; }

private:
    std::size_t offset_;
};

enum class BinaryOp { Or, And, Eq, Ne, Lt, Le, Gt, Ge, Match, NotMatch, Add, Sub, Mul, Div, Mod };
enum class UnaryOp { Not, Neg };

std::string_view binary_op_text(BinaryOp op) noexcept;

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
    enum class Kind { Literal, Field, Unary, Binary, Call, In };

    Kind kind = Kind::Literal;
    Value literal;                 // Literal
    std::string name;              // Field: field name; Call: lower-case function name
    UnaryOp unary_op = UnaryOp::Not;
    BinaryOp binary_op = BinaryOp::Eq;
    bool negated = false;          // In: "not in"
    bool is_duration = false;      // Literal written as a duration (5m), stored as Int nanoseconds
    std::vector<ExprPtr> children;  // operands, call arguments, or In: subject then items
    std::shared_ptr<const std::regex> regex;  // Match/NotMatch with a literal pattern
    std::size_t offset = 0;

    static ExprPtr make_literal(Value v, std::size_t offset = 0);
    static ExprPtr make_field(std::string name, std::size_t offset = 0);
};

/// Canonical text of an expression (used by explain output and as default
/// aggregate names).
std::string to_string(const Expr& e);

/// True if the expression reads no fields, so it can be evaluated once.
bool is_constant(const Expr& e);

struct SortKey {
    std::string field;
    bool descending = false;
};

struct Aggregate {
    std::string function;  // lower-case: count, sum, avg, p95, percentile, ...
    ExprPtr argument;      // null for count()
    double parameter = 0;  // percentile rank for percentile(x, q)
    std::string alias;     // output field name
};

struct Stage {
    enum class Kind { Where, Eval, Fields, Rename, Sort, Head, Tail, Dedup, Stats, Bin };

    Kind kind = Kind::Where;
    std::size_t offset = 0;

    ExprPtr predicate;                                     // Where
    std::vector<std::pair<std::string, ExprPtr>> assignments;  // Eval
    std::vector<std::string> names;  // Fields (patterns), Dedup fields, Stats group-by fields
    bool exclude = false;            // Fields: drop the listed fields instead
    std::vector<std::pair<std::string, std::string>> renames;  // Rename
    std::vector<SortKey> sort_keys;                            // Sort
    std::int64_t count = 10;                                   // Head, Tail
    std::vector<Aggregate> aggregates;                         // Stats

    // Bin: floor `field` to multiples of `span` (time or number), storing
    // the result in `alias` (defaults to the field itself).
    std::string field;
    bool span_is_duration = true;
    Duration duration_span = 0;
    double numeric_span = 0;
    std::string alias;
};

std::string_view stage_name(Stage::Kind kind) noexcept;

struct Query {
    std::vector<Stage> stages;
    std::string text;
};

/// Parses a query. An empty query or "*" matches every record. Throws
/// QueryError for syntax errors, unknown functions or stages, wrong
/// argument counts and invalid regular expressions.
Query parse_query(std::string_view text);

/// Multi-line description of the parsed pipeline.
std::string describe(const Query& q);

}  // namespace logsift
