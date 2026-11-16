// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Expression evaluation and the scalar function library.

#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "logsift/query.hpp"
#include "logsift/record.hpp"

namespace logsift {

struct EvalContext {
    /// The time now() and ago() are relative to. Fixed per query so results
    /// are consistent within a run.
    TimePoint now{};
};

/// Evaluates an expression against a record. Missing fields are null;
/// type mismatches produce null rather than errors.
Value evaluate(const Expr& e, const Record& r, const EvalContext& ctx);

/// evaluate(...).truthy()
bool matches(const Expr& e, const Record& r, const EvalContext& ctx);

using ScalarFn = Value (*)(std::span<const Value> args, const EvalContext& ctx);

struct FunctionDef {
    std::string_view name;
    int min_args;
    int max_args;  // -1: unlimited
    ScalarFn fn;
    std::string_view help;
};

const FunctionDef* find_function(std::string_view lower_name);
const std::vector<FunctionDef>& function_table();

/// Aggregate functions recognised by the stats stage.
bool is_aggregate_function(std::string_view lower_name);
std::vector<std::string_view> aggregate_function_names();

}  // namespace logsift
