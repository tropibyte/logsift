// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "helpers.hpp"

using namespace logsift;
using namespace logsift::test;

TEST_CASE("comparisons across types") {
    const Record r = rec("n=5 f=2.5 s=abc num_text=\"10\" b=true");
    CHECK(eval_expr("n == 5", r) == B(true));
    CHECK(eval_expr("n == 5.0", r) == B(true));
    CHECK(eval_expr("n < f * 3", r) == B(true));
    CHECK(eval_expr("num_text > 9", r) == B(true));        // numeric because one side is a number
    CHECK(eval_expr("num_text > \"9\"", r) == B(false));   // byte-wise between strings
    CHECK(eval_expr("s > 1", r) == B(false));              // incomparable: false
    CHECK(eval_expr("s != 1", r) == B(true));
    CHECK(eval_expr("b == true", r) == B(true));
    CHECK(eval_expr("b == \"true\"", r) == B(true));
}

TEST_CASE("missing fields are null") {
    const Record r = rec("a=1");
    CHECK(eval_expr("missing == null", r) == B(true));
    CHECK(eval_expr("missing != null", r) == B(false));
    CHECK(eval_expr("missing == 0", r) == B(false));
    CHECK(eval_expr("missing < 10", r) == B(false));
    CHECK(eval_expr("missing >= 10", r) == B(false));
    CHECK(eval_expr("not missing", r) == B(true));
    CHECK(eval_expr("missing + 1", r).is_null());
    CHECK(eval_expr("a == null", r) == B(false));
}

TEST_CASE("logic short-circuits and returns booleans") {
    const Record r = rec("a=1 z=0");
    CHECK(eval_expr("a and \"x\"", r) == B(true));
    CHECK(eval_expr("z or \"\"", r) == B(false));
    CHECK(eval_expr("z and nosuch == 1", r) == B(false));
    CHECK(eval_expr("not z", r) == B(true));
}

TEST_CASE("in lists") {
    const Record r = rec("status=503 level=warn");
    CHECK(eval_expr("status in (500, 503)", r) == B(true));
    CHECK(eval_expr("status in (\"503\")", r) == B(true));
    CHECK(eval_expr("level in (\"error\", \"fatal\")", r) == B(false));
    CHECK(eval_expr("level not in (\"error\", \"fatal\")", r) == B(true));
    CHECK(eval_expr("missing in (null)", r) == B(true));
    CHECK(eval_expr("status in ()", r) == B(false));
}

TEST_CASE("regular expressions") {
    const Record r = rec("msg=\"connection timed out after 30s\" code=E1234");
    CHECK(eval_expr("msg =~ \"timed? ?out\"", r) == B(true));
    CHECK(eval_expr("msg =~ \"^timed\"", r) == B(false));
    CHECK(eval_expr("msg !~ \"refused\"", r) == B(true));
    CHECK(eval_expr("code =~ \"^E\\\\d{4}$\"", r) == B(true));
    CHECK(eval_expr("missing =~ \".*\"", r) == B(false));
    CHECK(eval_expr("missing !~ \"x\"", r) == B(false));
    CHECK(eval_expr("msg =~ concat(\"time\", \"d\")", r) == B(true));  // pattern built at run time
    CHECK(eval_expr("msg =~ concat(\"(\")", r) == B(false));           // invalid runtime pattern: no match
}

TEST_CASE("arithmetic") {
    const Record r = rec("a=7 b=2 f=0.5 s=\"3\" t=hello");
    CHECK(eval_expr("a + b", r) == I(9));
    CHECK(eval_expr("a - b * 3", r) == I(1));
    CHECK(eval_expr("a / b", r) == F(3.5));
    CHECK(eval_expr("a % b", r) == I(1));
    CHECK(eval_expr("a % 0", r).is_null());
    CHECK(eval_expr("a / 0", r).is_null());
    CHECK(eval_expr("a * f", r) == F(3.5));
    CHECK(eval_expr("s * 2", r) == F(6.0));
    CHECK(eval_expr("t * 2", r).is_null());
    CHECK(eval_expr("-a", r) == I(-7));
    CHECK(eval_expr("9223372036854775807 + 1", r) == F(9223372036854775808.0));  // overflow widens
    CHECK(eval_expr("true + 1", r).is_null());
}

TEST_CASE("time arithmetic and durations") {
    const TimePoint now = at(2026, 11, 15, 12);
    const Record r = rec("time=2026-11-15T10:30:00Z start=2026-11-15T10:00:00Z");
    CHECK(eval_expr("_time - totime(start)", r) == I(30 * kMinute));
    CHECK(eval_expr("_time + 30m", r) == Value::time(at(2026, 11, 15, 11)));
    CHECK(eval_expr("_time > ago(2h)", r, now) == B(true));
    CHECK(eval_expr("_time > ago(1h)", r, now) == B(false));
    CHECK(eval_expr("_time > \"2026-11-15T10:00:00Z\"", r) == B(true));
    CHECK(eval_expr("now() - _time > 1h", r, now) == B(true));
    CHECK(eval_expr("_time * 2", r).is_null());
}

TEST_CASE("string functions") {
    const Record r = rec("s=\"Hello World\" path=/api/v1/users/42 empty=\"\"");
    CHECK(eval_expr("lower(s)", r) == S("hello world"));
    CHECK(eval_expr("upper(s)", r) == S("HELLO WORLD"));
    CHECK(eval_expr("len(s)", r) == I(11));
    CHECK(eval_expr("len(\"héllo\")", r) == I(5));
    CHECK(eval_expr("trim(\"  x \")", r) == S("x"));
    CHECK(eval_expr("contains(s, \"lo W\")", r) == B(true));
    CHECK(eval_expr("startswith(path, \"/api\")", r) == B(true));
    CHECK(eval_expr("endswith(path, \"42\")", r) == B(true));
    CHECK(eval_expr("contains(missing, \"x\")", r) == B(false));
    CHECK(eval_expr("substr(s, 6)", r) == S("World"));
    CHECK(eval_expr("substr(s, 0, 5)", r) == S("Hello"));
    CHECK(eval_expr("substr(s, 50)", r) == S(""));
    CHECK(eval_expr("substr(s, -1)", r).is_null());
    CHECK(eval_expr("replace(s, \"o\", \"0\")", r) == S("Hell0 W0rld"));
    CHECK(eval_expr("concat(\"a\", 1, true)", r) == S("a1true"));
    CHECK(eval_expr("split_part(path, \"/\", 4)", r) == S("users"));
    CHECK(eval_expr("split_part(path, \"/\", 9)", r).is_null());
    CHECK(eval_expr("extract(path, \"users/(\\\\d+)\")", r) == S("42"));
    CHECK(eval_expr("extract(path, \"(v)(\\\\d)\", 2)", r) == S("1"));
    CHECK(eval_expr("extract(path, \"nope(\\\\d)\")", r).is_null());
    CHECK(eval_expr("match(path, \"^/api/v[0-9]\")", r) == B(true));
}

TEST_CASE("conversion and null-handling functions") {
    const Record r = rec("n=\" 12 \" f=\"2.5\" bad=x ts=1794736623 empty=\"\"");
    CHECK(eval_expr("tonumber(n)", r) == I(12));
    CHECK(eval_expr("tonumber(f)", r) == F(2.5));
    CHECK(eval_expr("tonumber(bad)", r).is_null());
    CHECK(eval_expr("toint(f)", r) == I(2));
    CHECK(eval_expr("tostring(42)", r) == S("42"));
    CHECK(eval_expr("tostring(missing)", r).is_null());
    CHECK(eval_expr("totime(ts)", r) == Value::time(at(2026, 11, 15, 9, 57, 3)));
    CHECK(eval_expr("totime(\"2026-11-15\")", r) == Value::time(at(2026, 11, 15)));
    CHECK(eval_expr("typeof(f)", r) == S("string"));
    CHECK(eval_expr("typeof(ts)", r) == S("int"));
    CHECK(eval_expr("typeof(missing)", r) == S("null"));
    CHECK(eval_expr("isnull(missing)", r) == B(true));
    CHECK(eval_expr("exists(empty)", r) == B(true));
    CHECK(eval_expr("coalesce(missing, empty, bad)", r) == S(""));
    CHECK(eval_expr("if(bad == \"x\", 1, 2)", r) == I(1));
    CHECK(eval_expr("if(false, 1)", r).is_null());
}

TEST_CASE("math functions") {
    CHECK(eval_expr("abs(-3)") == I(3));
    CHECK(eval_expr("abs(-2.5)") == F(2.5));
    CHECK(eval_expr("floor(2.7)") == I(2));
    CHECK(eval_expr("ceil(2.1)") == I(3));
    CHECK(eval_expr("floor(-2.5)") == I(-3));
    CHECK(eval_expr("round(2.5)") == I(3));
    CHECK(eval_expr("round(3.14159, 2)") == F(3.14));
    CHECK(eval_expr("sqrt(16)") == F(4.0));
    CHECK(eval_expr("sqrt(-1)").is_null());
    CHECK(eval_expr("least(3, 1, 2)") == I(1));
    CHECK(eval_expr("greatest(3, null, 7.5)") == F(7.5));
    CHECK(eval_expr("abs(\"x\")").is_null());
}

TEST_CASE("time functions") {
    const Record r = rec("time=2026-11-15T09:57:03.250Z");
    CHECK(eval_expr("year(_time)", r) == I(2026));
    CHECK(eval_expr("month(_time)", r) == I(11));
    CHECK(eval_expr("day(_time)", r) == I(15));
    CHECK(eval_expr("hour(_time)", r) == I(9));
    CHECK(eval_expr("minute(_time)", r) == I(57));
    CHECK(eval_expr("second(_time)", r) == I(3));
    CHECK(eval_expr("weekday(_time)", r) == I(0));
    CHECK(eval_expr("date(_time)", r) == S("2026-11-15"));
    CHECK(eval_expr("hour(\"2026-01-01T05:00:00Z\")", r) == I(5));
    CHECK(eval_expr("hour(42)", r).is_null());
    CHECK(eval_expr("bucket(_time, 15m)", r) == Value::time(at(2026, 11, 15, 9, 45)));
    CHECK(eval_expr("duration(\"1h\")", r) == I(kHour));
    CHECK(eval_expr("format_duration(90m)", r) == S("1h30m"));
    CHECK(eval_expr("now()", r, at(2026, 1, 1)) == Value::time(at(2026, 1, 1)));
    CHECK(eval_expr("ago(\"1d\")", r, at(2026, 1, 2)) == Value::time(at(2026, 1, 1)));
}

TEST_CASE("function table is consistent") {
    for (const FunctionDef& f : function_table()) {
        CAPTURE(f.name);
        CHECK(find_function(f.name) == &f);
        CHECK_FALSE(is_aggregate_function(f.name));
        CHECK(f.min_args >= 0);
        CHECK((f.max_args < 0 || f.max_args >= f.min_args));
        CHECK_FALSE(f.help.empty());
    }
    for (std::string_view name : aggregate_function_names()) {
        CAPTURE(name);
        CHECK(make_aggregator(std::string(name), 50) != nullptr);
    }
    CHECK_THROWS_AS(make_aggregator("nope"), std::invalid_argument);
}
