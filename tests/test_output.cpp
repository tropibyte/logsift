// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include <algorithm>
#include <sstream>

#include "helpers.hpp"

using namespace logsift;
using namespace logsift::test;

namespace {

std::string write_all(OutputFormat format, const std::vector<Record>& rows, OutputOptions options = {}) {
    std::ostringstream out;
    auto writer = make_writer(format, out, options);
    for (const Record& r : rows) writer->write(r);
    writer->finish();
    return out.str();
}

std::vector<Record> rows() {
    std::vector<Record> out = recs({"time=2026-11-15T09:00:00Z svc=api ms=120 msg=\"all good\"",
                                    "time=2026-11-15T09:01:00.5Z svc=billing ms=7 extra=yes"});
    out[0].set("_raw", S("raw one"));
    return out;
}

}  // namespace

TEST_CASE("output format names") {
    CHECK(parse_output_format("TABLE") == OutputFormat::Table);
    CHECK(parse_output_format("jsonl") == OutputFormat::Json);
    CHECK_FALSE(parse_output_format("yaml").has_value());
}

TEST_CASE("table columns put _time first and hide internal fields") {
    CHECK(table_columns(rows(), false) == std::vector<std::string>{"_time", "time", "svc", "ms", "msg", "extra"});
    CHECK(table_columns(rows(), true) ==
          std::vector<std::string>{"_time", "time", "svc", "ms", "msg", "_raw", "extra"});
}

TEST_CASE("tables align columns and right-align numbers") {
    const std::string t = format_table(recs({"name=a n=5", "name=longer n=1234"}));
    CHECK(t ==
          "name    n\n"
          "------  ----\n"
          "a          5\n"
          "longer  1234\n");
    CHECK(format_table({}) == "(no results)\n");
    CHECK(format_table({Record{{"_raw", S("x")}}}) == "(no fields)\n");
}

TEST_CASE("tables show floats at 15 significant digits") {
    const std::string t = format_table({Record{{"x", F(572.8199999999999)}, {"y", F(0.1 + 0.2)}}});
    CHECK(t == "x       y\n------  ---\n572.82  0.3\n");
    // Other formats keep full precision.
    CHECK(to_logfmt(Record{{"x", F(0.1 + 0.2)}}) == "x=0.30000000000000004");
}

TEST_CASE("tables truncate wide cells and flatten newlines") {
    OutputOptions opts;
    opts.max_column_width = 8;
    const std::string t = format_table({Record{{"msg", S("line one\nline two is long")}}}, opts);
    CHECK(t == "msg\n--------\nline on\xE2\x80\xA6\n");
}

TEST_CASE("csv output") {
    const std::string csv = write_all(OutputFormat::Csv, recs({"a=1 b=\"x,y\"", "b=\"say \\\"hi\\\"\" c=3"}));
    CHECK(csv ==
          "a,b,c\n"
          "1,\"x,y\",\n"
          ",\"say \"\"hi\"\"\",3\n");
    CHECK(csv_field("plain") == "plain");
    CHECK(write_all(OutputFormat::Csv, {}).empty());
}

TEST_CASE("json and logfmt output stream one record per line") {
    const auto r = rows();
    const std::string json = write_all(OutputFormat::Json, r);
    CHECK(json.substr(0, json.find('\n')) ==
          R"({"time":"2026-11-15T09:00:00Z","svc":"api","ms":120,"msg":"all good","_time":"2026-11-15T09:00:00Z"})");
    CHECK(std::count(json.begin(), json.end(), '\n') == 2);

    const std::string lf = write_all(OutputFormat::Logfmt, r);
    CHECK(lf.substr(0, lf.find('\n')) ==
          "time=2026-11-15T09:00:00Z svc=api ms=120 msg=\"all good\" _time=2026-11-15T09:00:00Z");
    CHECK(to_logfmt(Record{{"k", S("a=b")}, {"e", S("")}, {"q", S("x\"y")}}) == "k=\"a=b\" e=\"\" q=\"x\\\"y\"");
}

TEST_CASE("raw output prefers the original line") {
    const std::string raw = write_all(OutputFormat::Raw, rows());
    CHECK(raw ==
          "raw one\n"
          "time=2026-11-15T09:01:00.5Z svc=billing ms=7 extra=yes _time=2026-11-15T09:01:00.500Z\n");
}

TEST_CASE("logfmt output parses back") {
    const Record original{{"a", I(1)}, {"msg", S("two words \"quoted\" \\ end")}, {"f", F(2.5)}, {"flag", B(true)}};
    auto back = parse_logfmt(to_logfmt(original));
    REQUIRE(back);
    CHECK(back->get("a") == I(1));
    CHECK(back->get("msg") == S("two words \"quoted\" \\ end"));
    CHECK(back->get("f") == F(2.5));
    CHECK(back->get("flag") == B(true));
}

TEST_CASE("field summary") {
    const auto summary = field_summary(recs({"a=1 b=x", "a=2.5 c=true", "a=\"3\" b=y"}));
    REQUIRE(summary.size() == 3);
    CHECK(summary[0].get("field") == S("a"));
    CHECK(summary[0].get("count") == I(3));
    CHECK(summary[0].get("types") == S("int 1, float 1, string 1"));
    CHECK(summary[0].get("example") == S("1"));
    CHECK(summary[1].get("field") == S("b"));
    CHECK(summary[1].get("count") == I(2));
    CHECK(summary[2].get("types") == S("bool 1"));
}
