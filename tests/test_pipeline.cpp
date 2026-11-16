// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include <fstream>

#include "helpers.hpp"

using namespace logsift;
using namespace logsift::test;

using Lines = std::vector<std::string>;

namespace {

std::vector<Record> requests() {
    return recs({
        "svc=api status=200 ms=120 user=ann",
        "svc=api status=500 ms=900 user=bob",
        "svc=auth status=200 ms=30 user=ann",
        "svc=api status=404 ms=15",
        "svc=billing status=503 ms=1500 user=cy",
        "svc=auth status=401 ms=25 user=bob",
        "svc=api status=200 ms=80 user=ann",
    });
}

}  // namespace

TEST_CASE("where filters") {
    CHECK(run("status >= 500", requests()) ==
          Lines{"svc=api status=500 ms=900 user=bob", "svc=billing status=503 ms=1500 user=cy"});
    CHECK(run("user == \"ann\" and svc != \"api\"", requests()) == Lines{"svc=auth status=200 ms=30 user=ann"});
    CHECK(run("not exists(user)", requests()) == Lines{"svc=api status=404 ms=15"});
    CHECK(run("*", requests()).size() == 7);
}

TEST_CASE("eval sees earlier assignments") {
    CHECK(run("head 1 | eval s = ms / 1000, slow = s > 0.1, label = concat(svc, \"-\", status)", requests()) ==
          Lines{"svc=api status=200 ms=120 user=ann s=0.12 slow=true label=api-200"});
    CHECK(run("head 1 | eval ms = ms * 2", requests()) == Lines{"svc=api status=200 ms=240 user=ann"});
}

TEST_CASE("fields keeps or drops, with prefix patterns") {
    const auto input = recs({"a=1 http.status=200 http.method=GET b=2"});
    CHECK(run("fields b, a", input) == Lines{"b=2 a=1"});
    CHECK(run("fields http.*", input) == Lines{"http.status=200 http.method=GET"});
    CHECK(run("fields - http.*", input) == Lines{"a=1 b=2"});
    CHECK(run("fields *", input) == Lines{"a=1 http.status=200 http.method=GET b=2"});
    CHECK(run("fields nosuch", input) == Lines{""});
}

TEST_CASE("rename") {
    const auto input = recs({"a=1 b=2 c=3"});
    CHECK(run("rename a as x", input) == Lines{"x=1 b=2 c=3"});
    CHECK(run("rename a as c", input) == Lines{"c=1 b=2"});  // replaces the existing c
    CHECK(run("rename nosuch as y", input) == Lines{"a=1 b=2 c=3"});
}

TEST_CASE("sort is stable, nulls last") {
    CHECK(run("sort user | fields user, ms", requests()) ==
          Lines{"user=ann ms=120", "user=ann ms=30", "user=ann ms=80", "user=bob ms=900", "user=bob ms=25",
                "user=cy ms=1500", "ms=15"});
    CHECK(run("sort -user, ms | fields user, ms", requests()) ==
          Lines{"user=cy ms=1500", "user=bob ms=25", "user=bob ms=900", "user=ann ms=30", "user=ann ms=80",
                "user=ann ms=120", "ms=15"});
    CHECK(run("sort -ms | head 2 | fields ms", requests()) == Lines{"ms=1500", "ms=900"});
}

TEST_CASE("head and tail") {
    CHECK(run("head 2 | fields ms", requests()) == Lines{"ms=120", "ms=900"});
    CHECK(run("head 0", requests()).empty());
    CHECK(run("tail 2 | fields ms", requests()) == Lines{"ms=25", "ms=80"});
    CHECK(run("tail 100", requests()).size() == 7);
    CHECK(run("tail 0", requests()).empty());
}

TEST_CASE("head stops the input early") {
    const Query q = parse_query("status == 200 | head 2");
    std::size_t delivered = 0;
    Pipeline p(q, [&](Record&&) {
        ++delivered;
        return true;
    });
    std::size_t pushed = 0;
    for (const Record& r : requests()) {
        ++pushed;
        if (!p.push(r)) break;
    }
    p.finish();
    CHECK(delivered == 2);
    CHECK(pushed == 3);  // the third record satisfied head; nothing after was read
    CHECK_FALSE(p.push(requests()[0]));
}

TEST_CASE("a sink can stop a pipeline") {
    const Query q = parse_query("sort ms");
    std::vector<std::string> got;
    Pipeline p(q, [&](Record&& r) {
        got.push_back(r.get("ms").to_string());
        return got.size() < 3;
    });
    for (const Record& r : requests()) p.push(r);
    p.finish();
    CHECK(got == std::vector<std::string>{"15", "25", "30"});
}

TEST_CASE("dedup keeps the first record per key") {
    CHECK(run("dedup user | fields user, ms", requests()) ==
          Lines{"user=ann ms=120", "user=bob ms=900", "ms=15", "user=cy ms=1500"});
    CHECK(run("dedup svc, status | count", requests()) == Lines{"count=6"});
}

TEST_CASE("stats aggregates") {
    CHECK(run("stats count(), count(user) as with_user, sum(ms), avg(ms), min(ms), max(ms), dc(user)", requests()) ==
          Lines{"count=7 with_user=6 sum(ms)=2670 avg(ms)=381.42857142857144 min(ms)=15 max(ms)=1500 dc(user)=3"});
    CHECK(run("stats count() as n by svc", requests()) == Lines{"svc=api n=4", "svc=auth n=2", "svc=billing n=1"});
    CHECK(run("stats values(user) as users, first(user) as f, last(user) as l by svc | fields svc, users, f, l",
              requests()) ==
          Lines{"svc=api users=\"ann, bob\" f=ann l=ann", "svc=auth users=\"ann, bob\" f=ann l=bob",
                "svc=billing users=cy f=cy l=cy"});
    CHECK(run("stats median(ms), p90(ms), percentile(ms, 0) as p0, range(ms) as r", requests()) ==
          Lines{"median(ms)=80 p90(ms)=1140.0000000000002 p0=15 r=1485"});
    CHECK(run("stats stdev(ms) as sd", recs({"ms=2", "ms=4", "ms=4", "ms=4", "ms=5", "ms=5", "ms=7", "ms=9"})) ==
          Lines{"sd=2.138089935299395"});
}

TEST_CASE("stats groups by several fields and treats missing ones as null") {
    CHECK(run("stats count() as n by svc, user", requests()) ==
          Lines{"svc=api n=1", "svc=api user=ann n=2", "svc=api user=bob n=1", "svc=auth user=ann n=1",
                "svc=auth user=bob n=1", "svc=billing user=cy n=1"});
}

TEST_CASE("stats over no input") {
    CHECK(run("stats count() as n, sum(ms) as s, avg(ms) as a", {}) == Lines{"n=0 s=0 a=\"\""});
    CHECK(run("stats count() as n by svc", {}).empty());
}

TEST_CASE("sum widens to float on overflow or float input") {
    CHECK(run("stats sum(x) as s", recs({"x=1", "x=2.5"})) == Lines{"s=3.5"});
    CHECK(run("stats sum(x) as s", recs({"x=9223372036854775807", "x=1"})) == Lines{"s=9223372036854775808"});
    CHECK(run("stats sum(x) as s", recs({"x=1", "x=abc", "x=\"2\""})) == Lines{"s=3"});
}

TEST_CASE("count and top") {
    CHECK(run("count", requests()) == Lines{"count=7"});
    CHECK(run("count by user", requests()) == Lines{"count=1", "user=ann count=3", "user=bob count=2", "user=cy count=1"});
    CHECK(run("top 2 svc", requests()) == Lines{"svc=api count=4", "svc=auth count=2"});
}

TEST_CASE("bin buckets times and numbers") {
    const auto input = recs({"time=2026-11-15T09:01:10Z ms=120", "time=2026-11-15T09:04:59Z ms=180",
                             "time=2026-11-15T09:05:00Z ms=260", "ms=7"});
    CHECK(run("bin _time span=5m | stats count() as n by _time", input) ==
          Lines{"n=1", "_time=2026-11-15T09:00:00Z n=2", "_time=2026-11-15T09:05:00Z n=1"});
    CHECK(run("bin ms span=100 as bucket | fields ms, bucket", input) ==
          Lines{"ms=120 bucket=100", "ms=180 bucket=100", "ms=260 bucket=200", "ms=7 bucket=0"});
    CHECK(run("bin ms span=0.5 | fields ms", recs({"ms=1.7"})) == Lines{"ms=1.5"});
}

TEST_CASE("multi-stage pipelines") {
    CHECK(run("status < 500 | eval sec = ms / 1000 | stats avg(sec) as avg_s by svc | sort -avg_s | head 1",
              requests()) == Lines{"svc=api avg_s=0.07166666666666667"});
}

TEST_CASE("pipelines over the example data") {
    std::ifstream in(std::string(LOGSIFT_EXAMPLES_DIR) + "/app.jsonl");
    REQUIRE(in);
    RecordReader reader(in, Format::Auto);
    std::vector<Record> all;
    while (auto r = reader.next()) all.push_back(std::move(*r));
    const auto errors = run("level == \"error\" | count", all);
    const auto by_status = run("http.status >= 500 | count", all);
    CHECK(errors == by_status);  // every error in the sample is a 5xx
    const auto total = run("count by level | stats sum(count) as total", all);
    CHECK(total == Lines{"total=" + std::to_string(all.size())});
}
