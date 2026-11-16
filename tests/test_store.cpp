// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include <fstream>

#include "helpers.hpp"

using namespace logsift;
using namespace logsift::test;

namespace {

LogStore sample_store(bool time_index, std::vector<std::string> indexed) {
    LogStore store;
    const char* lines[] = {
        "time=2026-11-15T09:00:00Z svc=api status=200",
        "time=2026-11-15T09:05:00Z svc=auth status=500",
        "time=2026-11-15T09:10:00Z svc=api status=\"500\"",
        "time=2026-11-15T09:15:00Z svc=billing status=503",
        "svc=api status=200",  // no timestamp
        "time=2026-11-15T09:20:00Z svc=API status=500.0",
        "time=2026-11-15T09:02:00Z svc=api",  // out of order, no status
    };
    for (const char* line : lines) store.add(rec(line));
    if (time_index) store.build_time_index();
    for (const auto& f : indexed) store.build_field_index(f);
    return store;
}

std::vector<std::string> exec(const LogStore& store, std::string_view query, ExecStats* stats_out = nullptr,
                              std::optional<ScanPlan::Bound> from = std::nullopt,
                              std::optional<ScanPlan::Bound> to = std::nullopt) {
    std::vector<std::string> out;
    const Query q = parse_query(query);
    const ExecStats stats = execute(q, store, [&](Record&& r) {
        out.push_back(to_logfmt(r, true));
        return true;
    }, EvalContext{at(2026, 11, 15, 9, 30)}, from, to);
    if (stats_out) *stats_out = stats;
    return out;
}

}  // namespace

TEST_CASE("time index range queries") {
    const LogStore store = sample_store(true, {});
    CHECK(store.has_time_index());
    CHECK(store.time_range(at(2026, 11, 15, 9, 5), true, at(2026, 11, 15, 9, 15), false) ==
          std::vector<std::size_t>{1, 2});
    CHECK(store.time_range(at(2026, 11, 15, 9, 5), false, std::nullopt, false) == std::vector<std::size_t>{2, 3, 5});
    CHECK(store.time_range(std::nullopt, false, at(2026, 11, 15, 9, 2), true) == std::vector<std::size_t>{0, 6});
    const auto span = store.time_span();
    REQUIRE(span);
    CHECK(span->first == at(2026, 11, 15, 9));
    CHECK(span->second == at(2026, 11, 15, 9, 20));
}

TEST_CASE("field index lookups agree with query equality") {
    const LogStore store = sample_store(false, {"status", "svc"});
    CHECK(store.has_field_index("status"));
    CHECK(store.indexed_fields() == std::vector<std::string>{"status", "svc"});
    CHECK(store.lookup("status", I(500)) == std::vector<std::size_t>{1, 2, 5});  // 500, "500", 500.0
    CHECK(store.lookup("status", S("500")) == std::vector<std::size_t>{1, 2, 5});
    CHECK(store.lookup("svc", S("api")) == std::vector<std::size_t>{0, 2, 4, 6});  // case-sensitive
    CHECK(store.lookup("svc", Value{}).empty());
    CHECK(store.lookup("nosuch", I(1)).empty());
}

TEST_CASE("indexes stay current as records are added") {
    LogStore store = sample_store(true, {"svc"});
    store.add(rec("time=2026-11-15T08:00:00Z svc=api"));
    CHECK(store.lookup("svc", S("api")).back() == 7);
    CHECK(store.time_range(std::nullopt, false, at(2026, 11, 15, 8, 30), false) == std::vector<std::size_t>{7});
}

TEST_CASE("the planner uses indexes for conjunctive filters") {
    const LogStore store = sample_store(true, {"status", "svc"});
    auto plan_of = [&](std::string_view q) { return plan_scan(parse_query(q), store, EvalContext{at(2026, 11, 15, 9, 30)}); };

    ScanPlan p = plan_of("svc == \"api\" and _time >= \"2026-11-15T09:05:00Z\" and _time < ago(15m)");
    REQUIRE(p.from);
    CHECK(p.from->time == at(2026, 11, 15, 9, 5));
    CHECK(p.from->inclusive);
    REQUIRE(p.to);
    CHECK(p.to->time == at(2026, 11, 15, 9, 15));
    CHECK_FALSE(p.to->inclusive);
    REQUIRE(p.equalities.size() == 1);
    CHECK(p.equalities[0].first == "svc");
    CHECK(p.describe().find("index scan") == 0);

    p = plan_of("\"2026-11-15T09:10:00Z\" > _time and _time > \"2026-11-15T09:00:00Z\" and _time >= \"2026-11-15T08:00:00Z\"");
    REQUIRE(p.from);
    CHECK(p.from->time == at(2026, 11, 15, 9));
    CHECK_FALSE(p.from->inclusive);
    CHECK(p.to->time == at(2026, 11, 15, 9, 10));

    p = plan_of("status in (500, 503) | head 1");
    REQUIRE(p.equalities.size() == 1);
    CHECK(p.equalities[0].second.size() == 2);

    CHECK(plan_of("svc == \"api\" or status == 500").full_scan());   // disjunction: no index
    CHECK(plan_of("lower(svc) == \"api\"").full_scan());             // not a bare field
    CHECK(plan_of("region == \"x\"").full_scan());                   // not indexed
    CHECK(plan_of("status in (500, null)").full_scan());             // null item
    CHECK(plan_of("status not in (500)").full_scan());
    CHECK(plan_of("svc == other_field").full_scan());
    CHECK(plan_of("head 5 | svc == \"api\"").full_scan());           // only the first stage is planned
    CHECK(plan_of("*").full_scan());
    CHECK(plan_of("*").describe() == "full scan");
}

TEST_CASE("indexed execution matches a full scan") {
    const LogStore indexed = sample_store(true, {"status", "svc"});
    const LogStore plain = sample_store(false, {});
    const char* queries[] = {
        "svc == \"api\"",
        "status == 500",
        "status == \"500\" and svc == \"api\"",
        "status in (200, 503)",
        "_time >= \"2026-11-15T09:05:00Z\"",
        "_time > \"2026-11-15T09:05:00Z\" and _time <= \"2026-11-15T09:15:00Z\"",
        "_time == \"2026-11-15T09:10:00Z\"",
        "_time < ago(20m) and svc == \"api\"",
        "svc == \"api\" and status != 200",
        "status == 500 | stats count() as n by svc",
        "svc == \"nobody\"",
        "_time > \"not a time\"",
        "_time > 5",
        "*",
    };
    for (const char* q : queries) {
        CAPTURE(q);
        ExecStats stats;
        const auto fast = exec(indexed, q, &stats);
        const auto slow = exec(plain, q);
        CHECK(fast == slow);
        CHECK(stats.candidates <= indexed.size());
    }
}

TEST_CASE("execution statistics reflect index use") {
    const LogStore store = sample_store(true, {"svc"});
    ExecStats stats;
    const auto out = exec(store, "svc == \"billing\"", &stats);
    CHECK(out.size() == 1);
    CHECK(stats.candidates == 1);
    CHECK(stats.examined == 1);
    CHECK(stats.emitted == 1);
}

TEST_CASE("time windows from outside the query") {
    const LogStore indexed = sample_store(true, {});
    const LogStore plain = sample_store(false, {});
    const ScanPlan::Bound from{at(2026, 11, 15, 9, 2), true};
    const ScanPlan::Bound to{at(2026, 11, 15, 9, 15), false};
    const auto a = exec(indexed, "*", nullptr, from, to);
    const auto b = exec(plain, "*", nullptr, from, to);
    CHECK(a == b);
    CHECK(a.size() == 3);  // 09:02, 09:05, 09:10; the record without a time is excluded
    CHECK(exec(plain, "count", nullptr, std::nullopt, to) == std::vector<std::string>{"count=4"});
}

TEST_CASE("indexed and streaming results agree on the example data") {
    std::ifstream in(std::string(LOGSIFT_EXAMPLES_DIR) + "/app.jsonl");
    REQUIRE(in);
    RecordReader reader(in, Format::Auto);
    LogStore store;
    std::vector<Record> all;
    while (auto r = reader.next()) {
        all.push_back(*r);
        store.add(std::move(*r));
    }
    store.build_time_index();
    store.build_field_index("service");
    store.build_field_index("http.status");
    const char* queries[] = {
        "service == \"api\" and http.status >= 400 | stats count() as n, avg(duration_ms) as avg by level",
        "http.status in (500, 503) and _time > \"2026-11-15T09:20:00Z\" | fields _time, service, msg",
        "service == \"billing\" | sort -duration_ms | head 3",
    };
    for (const char* q : queries) {
        CAPTURE(q);
        std::vector<std::string> streamed;
        for (const Record& r : run_query(parse_query(q), all)) streamed.push_back(to_logfmt(r, true));
        CHECK(exec(store, q) == streamed);
    }
}
