// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include <fstream>
#include <sstream>

#include "helpers.hpp"

using namespace logsift;
using namespace logsift::test;

TEST_CASE("logfmt") {
    auto r = parse_logfmt(R"(level=info msg="hello world" n=3 ratio=0.5 ok=true quoted="42" empty= flag path=/a/b)");
    REQUIRE(r);
    CHECK(r->get("level") == S("info"));
    CHECK(r->get("msg") == S("hello world"));
    CHECK(r->get("n") == I(3));
    CHECK(r->get("ratio") == F(0.5));
    CHECK(r->get("ok") == B(true));
    CHECK(r->get("quoted") == S("42"));  // quoted values stay text
    CHECK(r->get("empty") == S(""));
    CHECK(r->get("flag") == B(true));
    CHECK(r->get("path") == S("/a/b"));

    r = parse_logfmt(R"(msg="tab\there \"q\" back\\slash")");
    REQUIRE(r);
    CHECK(r->get("msg") == S("tab\there \"q\" back\\slash"));

    for (const char* bad : {"", "just words", "=value", "k=\"open", "k=\"a\"b", "k=a\"b", "\"k\"=v"}) {
        CAPTURE(bad);
        CHECK_FALSE(parse_logfmt(bad).has_value());
    }
}

TEST_CASE("syslog RFC 5424") {
    auto r = parse_syslog(
        R"(<34>1 2026-11-15T09:57:03.003Z mymachine.example.com su - ID47 [exampleSDID@32473 iut="3" eventSource="Application"][meta x="a\"b"] 'su root' failed)",
        2026);
    REQUIRE(r);
    CHECK(r->get("priority") == I(34));
    CHECK(r->get("facility") == S("auth"));
    CHECK(r->get("severity") == S("crit"));
    CHECK(r->get("level") == S("crit"));
    CHECK(r->get("host") == S("mymachine.example.com"));
    CHECK(r->get("app") == S("su"));
    CHECK_FALSE(r->has("pid"));
    CHECK(r->get("msgid") == S("ID47"));
    CHECK(r->get("sd.exampleSDID@32473.iut") == S("3"));
    CHECK(r->get("sd.meta.x") == S("a\"b"));
    CHECK(r->get("message") == S("'su root' failed"));
    CHECK(r->time()->ns == at(2026, 11, 15, 9, 57, 3).ns + 3 * kMillisecond);

    r = parse_syslog("<165>1 - - - - - -", 2026);
    REQUIRE(r);
    CHECK(r->get("facility") == S("local4"));
    CHECK(r->get("severity") == S("notice"));
    CHECK(r->get("message") == S(""));
    CHECK_FALSE(r->time().has_value());

    CHECK_FALSE(parse_syslog("<34>1 notatime host app - - - msg", 2026).has_value());
    CHECK_FALSE(parse_syslog("<34>1 2026-11-15T09:57:03Z host app - - [broken msg", 2026).has_value());
    CHECK_FALSE(parse_syslog("<999>1 - - - - - -", 2026).has_value());
}

TEST_CASE("syslog RFC 3164 / BSD") {
    auto r = parse_syslog("<13>Nov 15 09:57:03 web01 sshd[2211]: Failed password for root", 2026);
    REQUIRE(r);
    CHECK(r->get("severity") == S("notice"));
    CHECK(r->get("host") == S("web01"));
    CHECK(r->get("app") == S("sshd"));
    CHECK(r->get("pid") == I(2211));
    CHECK(r->get("message") == S("Failed password for root"));
    CHECK(r->time() == at(2026, 11, 15, 9, 57, 3));

    r = parse_syslog("Nov  5 01:02:03 db01 kernel: Out of memory", 2026);  // no PRI, padded day
    REQUIRE(r);
    CHECK_FALSE(r->has("priority"));
    CHECK(r->get("app") == S("kernel"));
    CHECK_FALSE(r->has("pid"));
    CHECK(r->get("message") == S("Out of memory"));

    r = parse_syslog("Nov 15 09:57:03 host something without a tag", 2026);
    REQUIRE(r);
    CHECK_FALSE(r->has("app"));
    CHECK(r->get("message") == S("something without a tag"));

    CHECK_FALSE(parse_syslog("Hello there, this is not syslog", 2026).has_value());
}

TEST_CASE("Common and Combined Log Format") {
    auto r = parse_clf(
        R"x(127.0.0.1 - frank [15/Nov/2026:04:57:03 -0500] "GET /apache_pb.gif?x=1 HTTP/1.0" 200 2326 "http://www.example.com/start.html" "Mozilla/4.08 [en] (Win98; I ;Nav)")x");
    REQUIRE(r);
    CHECK(r->get("client") == S("127.0.0.1"));
    CHECK_FALSE(r->has("ident"));
    CHECK(r->get("user") == S("frank"));
    CHECK(r->time() == at(2026, 11, 15, 9, 57, 3));
    CHECK(r->get("method") == S("GET"));
    CHECK(r->get("path") == S("/apache_pb.gif?x=1"));
    CHECK(r->get("protocol") == S("HTTP/1.0"));
    CHECK(r->get("status") == I(200));
    CHECK(r->get("bytes") == I(2326));
    CHECK(r->get("referer") == S("http://www.example.com/start.html"));
    CHECK(r->get("user_agent") == S("Mozilla/4.08 [en] (Win98; I ;Nav)"));

    r = parse_clf(R"(10.0.0.1 - - [15/Nov/2026:04:57:03 +0000] "-" 408 -)");
    REQUIRE(r);
    CHECK(r->get("request") == S("-"));
    CHECK_FALSE(r->has("bytes"));
    CHECK(r->get("status") == I(408));

    for (const char* bad : {"", "127.0.0.1 - - [bad] \"GET / HTTP/1.1\" 200 1",
                            "127.0.0.1 - - [15/Nov/2026:04:57:03 -0500] GET / 200 1",
                            "127.0.0.1 - - [15/Nov/2026:04:57:03 -0500] \"GET / HTTP/1.1\" abc 1",
                            "127.0.0.1 - - [15/Nov/2026:04:57:03 -0500] \"GET / HTTP/1.1\" 200 1 \"ref\" junk"}) {
        CAPTURE(bad);
        CHECK_FALSE(parse_clf(bad).has_value());
    }
}

TEST_CASE("timestamp promotion") {
    Record r = rec("time=2026-11-15T09:57:03Z msg=x");
    CHECK(r.time() == at(2026, 11, 15, 9, 57, 3));
    r = rec("ts=1794736623 msg=x");
    CHECK(r.time() == at(2026, 11, 15, 9, 57, 3));
    r = rec("ts=1794736623000 msg=x");  // milliseconds
    CHECK(r.time() == at(2026, 11, 15, 9, 57, 3));
    r = rec("timestamp=garbage date=2026-11-15 msg=x");  // falls through to the next candidate
    CHECK(r.time() == at(2026, 11, 15));
    r = rec("_time=2026-11-15T00:00:00Z msg=x");  // an existing _time is converted
    CHECK(r.get("_time").is_time());
    r = rec("_time=nonsense msg=x");
    CHECK_FALSE(r.has("_time"));
    auto j = parse_line(Format::Json, R"({"@timestamp":"2026-11-15T09:57:03Z","time":"later"})");
    REQUIRE(j);
    CHECK(j->time() == at(2026, 11, 15, 9, 57, 3));
}

TEST_CASE("format names") {
    CHECK(parse_format_name("JSON") == Format::Json);
    CHECK(parse_format_name("ndjson") == Format::Json);
    CHECK(parse_format_name("combined") == Format::Clf);
    CHECK_FALSE(parse_format_name("xml").has_value());
    CHECK(format_name(Format::Syslog) == "syslog");
}

TEST_CASE("format detection") {
    CHECK(detect_format({R"({"a":1})", R"({"b":2})", "garbage"}) == Format::Json);
    CHECK(detect_format({"a=1 b=2", "c=3 d=\"x y\""}) == Format::Logfmt);
    CHECK(detect_format({"Nov 15 09:57:03 web01 sshd[1]: hi"}) == Format::Syslog);
    CHECK(detect_format({R"(1.2.3.4 - - [15/Nov/2026:04:57:03 -0500] "GET / HTTP/1.1" 200 5)"}) == Format::Clf);
    CHECK(detect_format({"just some text", "and more text"}) == Format::Plain);
    CHECK(detect_format({"key=value only", "plain text here", "more plain"}) == Format::Plain);
    CHECK(detect_format({}) == Format::Plain);
}

TEST_CASE("RecordReader") {
    std::istringstream in("{\"a\":1}\r\n\n{\"a\":2}\nnot json\n");
    RecordReader reader(in, Format::Auto, ParseOptions{}, "mem.log");
    auto r1 = reader.next();
    REQUIRE(r1);
    CHECK(reader.format() == Format::Json);
    CHECK(r1->get("a") == I(1));
    CHECK(r1->get("_line") == I(1));
    CHECK(r1->get("_file") == S("mem.log"));
    CHECK(r1->get("_raw") == S("{\"a\":1}"));
    auto r2 = reader.next();
    REQUIRE(r2);
    CHECK(r2->get("_line") == I(3));  // blank line skipped but counted
    auto r3 = reader.next();
    REQUIRE(r3);
    CHECK(r3->get("_parse_error") == B(true));
    CHECK(r3->get("message") == S("not json"));
    CHECK_FALSE(reader.next().has_value());
    CHECK(reader.lines_read() == 4);
    CHECK(reader.parse_errors() == 1);
}

TEST_CASE("RecordReader without raw text or a source name") {
    std::istringstream in("a=1 b=2\n");
    ParseOptions opts;
    opts.keep_raw = false;
    RecordReader reader(in, Format::Logfmt, opts);
    auto r = reader.next();
    REQUIRE(r);
    CHECK_FALSE(r->has("_raw"));
    CHECK_FALSE(r->has("_file"));
}

TEST_CASE("example files parse cleanly") {
    const std::pair<const char*, Format> files[] = {
        {"app.jsonl", Format::Json},
        {"access.log", Format::Clf},
        {"syslog.log", Format::Syslog},
        {"worker.logfmt", Format::Logfmt},
    };
    for (const auto& [name, expected] : files) {
        CAPTURE(name);
        std::ifstream in(std::string(LOGSIFT_EXAMPLES_DIR) + "/" + name);
        REQUIRE(in);
        ParseOptions opts;
        opts.default_year = 2026;
        RecordReader reader(in, Format::Auto, opts);
        std::size_t n = 0;
        while (auto r = reader.next()) {
            ++n;
            CHECK(r->time().has_value());
        }
        CHECK(reader.format() == expected);
        CHECK(reader.parse_errors() == 0);
        CHECK(n > 20);
    }
}
