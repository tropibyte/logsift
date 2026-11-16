// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "helpers.hpp"

using namespace logsift;
using namespace logsift::test;

namespace {

std::string canonical(std::string_view expr) {
    const Query q = parse_query(std::string("where ") + std::string(expr));
    return to_string(*q.stages.at(0).predicate);
}

std::size_t error_offset(std::string_view query) {
    try {
        parse_query(query);
    } catch (const QueryError& e) {
        return e.offset();
    }
    FAIL("expected QueryError for: " << query);
    return 0;
}

}  // namespace

TEST_CASE("empty and match-all queries") {
    CHECK(parse_query("").stages.empty());
    CHECK(parse_query("  * ").stages.empty());
    CHECK(describe(parse_query("*")) == "(all records)\n");
}

TEST_CASE("a leading filter is an implicit where") {
    const Query q = parse_query("level == \"error\" | head 3");
    REQUIRE(q.stages.size() == 2);
    CHECK(q.stages[0].kind == Stage::Kind::Where);
    CHECK(q.stages[1].kind == Stage::Kind::Head);
    CHECK(q.stages[1].count == 3);
}

TEST_CASE("stage keywords can still be used as field names") {
    Query q = parse_query("count > 5");
    REQUIRE(q.stages.size() == 1);
    CHECK(q.stages[0].kind == Stage::Kind::Where);
    q = parse_query("head == 1 or tail in (1, 2)");
    CHECK(q.stages[0].kind == Stage::Kind::Where);
    q = parse_query("sort not in (\"x\")");
    CHECK(q.stages[0].kind == Stage::Kind::Where);
    q = parse_query("`stats` == 1");
    CHECK(q.stages[0].kind == Stage::Kind::Where);
}

TEST_CASE("operator precedence") {
    CHECK(canonical("a or b and c") == "a or b and c");
    CHECK(canonical("(a or b) and c") == "(a or b) and c");
    CHECK(canonical("not a == 1") == "not a == 1");
    CHECK(canonical("a + b * c") == "a + b * c");
    CHECK(canonical("(a + b) * c") == "(a + b) * c");
    CHECK(canonical("a - (b - c)") == "a - (b - c)");
    CHECK(canonical("-x * 2") == "-x * 2");
    CHECK(canonical("a && b || !c") == "a and b or not c");
    CHECK(canonical("x = 1") == "x == 1");
    CHECK(canonical("status in (500, 503)") == "status in (500, 503)");
    CHECK(canonical("status not in (500)") == "status not in (500)");
    CHECK(canonical("msg =~ 'time.*out'") == "msg =~ \"time.*out\"");
    CHECK(canonical("lower(svc) == \"api\"") == "lower(svc) == \"api\"");
    CHECK(canonical("`user-agent` != null") == "`user-agent` != null");
    CHECK(canonical("d > 1h30m") == "d > 1h30m");
    CHECK(canonical("x > 1.5e3") == "x > 1500");
}

TEST_CASE("every stage parses") {
    const Query q = parse_query(
        "where a > 1 | eval b = a * 2, c = upper(s) | fields a, http.*, b | fields - b | rename a as x, c as y"
        " | sort -x, y desc, +z | tail 4 | dedup x, y | stats count() as n, avg(x), p95(x), percentile(x, 99.9) by y"
        " | bin _time span=5m as bucket | head");
    REQUIRE(q.stages.size() == 11);
    CHECK(q.stages[1].assignments.size() == 2);
    CHECK(q.stages[2].names == std::vector<std::string>{"a", "http.*", "b"});
    CHECK(q.stages[3].exclude);
    CHECK(q.stages[4].renames[1] == std::pair<std::string, std::string>{"c", "y"});
    const auto& keys = q.stages[5].sort_keys;
    REQUIRE(keys.size() == 3);
    CHECK(keys[0].descending);
    CHECK(keys[1].descending);
    CHECK_FALSE(keys[2].descending);
    CHECK(q.stages[6].count == 4);
    const Stage& stats = q.stages[8];
    REQUIRE(stats.aggregates.size() == 4);
    CHECK(stats.aggregates[0].alias == "n");
    CHECK(stats.aggregates[1].alias == "avg(x)");
    CHECK(stats.aggregates[2].alias == "p95(x)");
    CHECK(stats.aggregates[3].parameter == 99.9);
    CHECK(stats.aggregates[3].alias == "percentile(x, 99.9)");
    CHECK(stats.names == std::vector<std::string>{"y"});
    CHECK(q.stages[9].duration_span == 5 * kMinute);
    CHECK(q.stages[9].alias == "bucket");
    CHECK(q.stages[10].count == 10);  // default

    const std::string text = describe(q);
    CHECK(text.find("1. where a > 1\n") != std::string::npos);
    CHECK(text.find("9. stats count() as n, avg(x) as avg(x)") != std::string::npos);
    CHECK(text.find("10. bin _time span=5m as bucket") != std::string::npos);
}

TEST_CASE("default aggregate names") {
    const Query q = parse_query("stats count(), count(x), sum(x), percentile(x, 50)");
    const auto& a = q.stages[0].aggregates;
    CHECK(a[0].alias == "count");
    CHECK(a[1].alias == "count(x)");
    CHECK(a[2].alias == "sum(x)");
    CHECK(a[3].alias == "percentile(x, 50)");
}

TEST_CASE("count and top are shorthands") {
    Query q = parse_query("count by level");
    REQUIRE(q.stages.size() == 1);
    CHECK(q.stages[0].kind == Stage::Kind::Stats);
    CHECK(q.stages[0].aggregates[0].alias == "count");
    CHECK(q.stages[0].names == std::vector<std::string>{"level"});

    q = parse_query("top 3 path, method");
    REQUIRE(q.stages.size() == 3);
    CHECK(q.stages[0].kind == Stage::Kind::Stats);
    CHECK(q.stages[1].kind == Stage::Kind::Sort);
    CHECK(q.stages[2].count == 3);
}

TEST_CASE("numeric bins") {
    const Query q = parse_query("bin latency span=100");
    CHECK_FALSE(q.stages[0].span_is_duration);
    CHECK(q.stages[0].numeric_span == 100);
    CHECK(q.stages[0].alias == "latency");
}

TEST_CASE("errors point at the problem") {
    CHECK(error_offset("status >=") == 9);
    CHECK(error_offset("a == 1 | head x") == 14);
    CHECK(error_offset("lower(a, b)") == 0);
    CHECK(error_offset("x =~ \"[\"") == 5);
}

TEST_CASE("syntax errors") {
    for (const char* bad : {"status >=", "(a == 1", "a == 1 |", "| head", "head -1", "eval x 1", "rename a b",
                            "stats", "stats avg()", "stats foo(x)", "stats count() by", "sort", "fields",
                            "bin _time", "bin _time span=0s", "bin x span=abc", "a in 1, 2", "nosuch(1)",
                            "lower(1, 2)", "msg =~ \"(\"", "\"unterminated", "x == 'a", "x > 5q", "a $ b",
                            "`unterminated", "``", "avg(x) > 1", "a == 1 b", "dedup", "x == 1 | where"}) {
        CAPTURE(bad);
        CHECK_THROWS_AS(parse_query(bad), QueryError);
    }
}

TEST_CASE("nesting depth is limited") {
    std::string deep;
    for (int i = 0; i < 500; ++i) deep += "(";
    deep += "1";
    for (int i = 0; i < 500; ++i) deep += ")";
    CHECK_THROWS_AS(parse_query(deep), QueryError);
    std::string nots;
    for (int i = 0; i < 500; ++i) nots += "not ";
    nots += "x";
    CHECK_THROWS_AS(parse_query(nots), QueryError);
}

TEST_CASE("is_constant") {
    auto constant = [](std::string_view expr) {
        return is_constant(*parse_query(std::string("where ") + std::string(expr)).stages[0].predicate);
    };
    CHECK(constant("1 + 2"));
    CHECK(constant("ago(1h)"));
    CHECK(constant("now()"));
    CHECK_FALSE(constant("x + 1"));
    CHECK_FALSE(constant("lower(name)"));
}
