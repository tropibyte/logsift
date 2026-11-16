// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Shared test helpers.

#pragma once

#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "logsift/logsift.hpp"

namespace logsift {

// Lets doctest print values and records in failure messages.
inline std::ostream& operator<<(std::ostream& os, const Value& v) {
    return os << type_name(v.type()) << '(' << v.to_string() << ')';
}

inline std::ostream& operator<<(std::ostream& os, const Record& r) { return os << record_to_json(r, true); }

}  // namespace logsift

namespace logsift::test {

/// A record from logfmt text, with _time assigned from ts/time fields.
inline Record rec(std::string_view logfmt) {
    auto r = parse_logfmt(logfmt);
    REQUIRE_MESSAGE(r.has_value(), "bad logfmt in test: " << logfmt);
    assign_time(*r, ParseOptions{});
    return *r;
}

inline std::vector<Record> recs(std::initializer_list<std::string_view> lines) {
    std::vector<Record> out;
    for (auto line : lines) out.push_back(rec(line));
    return out;
}

/// Runs a query and renders each output record as logfmt (internal fields
/// shown), which makes expectations easy to read.
inline std::vector<std::string> run(std::string_view query, const std::vector<Record>& input, TimePoint now = {}) {
    EvalContext ctx;
    ctx.now = now;
    std::vector<std::string> out;
    for (const Record& r : run_query(parse_query(query), input, ctx)) out.push_back(to_logfmt(r, true));
    return out;
}

inline Value eval_expr(std::string_view expr, const Record& r = {}, TimePoint now = {}) {
    const Query q = parse_query(std::string("where ") + std::string(expr));
    EvalContext ctx;
    ctx.now = now;
    return evaluate(*q.stages.at(0).predicate, r, ctx);
}

inline Value I(std::int64_t v) { return Value::integer(v); }
inline Value F(double v) { return Value::real(v); }
inline Value S(std::string v) { return Value::string(std::move(v)); }
inline Value B(bool v) { return Value::boolean(v); }

inline TimePoint at(int y, int mo, int d, int h = 0, int mi = 0, int s = 0) { return *make_time(y, mo, d, h, mi, s); }

}  // namespace logsift::test
