// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "helpers.hpp"

using namespace logsift;
using namespace logsift::test;

TEST_CASE("flat objects with every scalar type") {
    auto r = parse_json_object(R"({"a":1,"b":-2.5,"c":"x","d":true,"e":false,"f":null})");
    REQUIRE(r);
    CHECK(r->get("a") == I(1));
    CHECK(r->get("b") == F(-2.5));
    CHECK(r->get("c") == S("x"));
    CHECK(r->get("d") == B(true));
    CHECK(r->get("e") == B(false));
    CHECK(r->has("f"));
    CHECK(r->get("f").is_null());
    CHECK(r->size() == 6);
}

TEST_CASE("nested objects are flattened, arrays kept as JSON text") {
    auto r = parse_json_object(R"({ "http" : { "status" : 200, "req": {"id": "abc"} }, "tags": [ "a", {"b" : 1} ], "empty": {} })");
    REQUIRE(r);
    CHECK(r->get("http.status") == I(200));
    CHECK(r->get("http.req.id") == S("abc"));
    CHECK(r->get("tags") == S(R"(["a",{"b":1}])"));
    CHECK(r->get("empty") == S("{}"));
}

TEST_CASE("numbers") {
    auto r = parse_json_object(R"({"big":12345678901234567890,"exp":1e3,"neg":-0,"int":9007199254740993})");
    REQUIRE(r);
    CHECK(r->get("big") == F(12345678901234567890.0));
    CHECK(r->get("exp") == F(1000.0));
    CHECK(r->get("neg") == I(0));
    CHECK(r->get("int") == I(9007199254740993));  // exact, not rounded through double
}

TEST_CASE("string escapes and unicode") {
    auto r = parse_json_object(R"({"s":"a\"b\\c\/d\n\t","u":"caf\u00e9 \ud83d\ude00","bad":"\ud800x"})");
    REQUIRE(r);
    CHECK(r->get("s") == S("a\"b\\c/d\n\t"));
    CHECK(r->get("u") == S("café 😀"));
    CHECK(r->get("bad") == S("\xEF\xBF\xBDx"));  // lone surrogate becomes U+FFFD
}

TEST_CASE("duplicate keys keep the last value") {
    auto r = parse_json_object(R"({"a":1,"a":2})");
    REQUIRE(r);
    CHECK(r->get("a") == I(2));
    CHECK(r->size() == 1);
}

TEST_CASE("invalid JSON is rejected with a message") {
    for (const char* bad : {"", "[]", "\"x\"", "{", "{\"a\"}", "{\"a\":}", "{\"a\":1,}", "{\"a\":01}", "{\"a\":1} x",
                            "{\"a\":tru}", "{\"a\":\"x}", "{\"a\":\"\\q\"}", "{a:1}", "{\"a\":[1,2}",
                            "{\"a\":\"line\nbreak\"}", "{\"a\":-}", "{\"a\":1.}"}) {
        CAPTURE(bad);
        std::string error;
        CHECK_FALSE(parse_json_object(bad, &error).has_value());
        CHECK_FALSE(error.empty());
    }
}

TEST_CASE("nesting depth is limited") {
    std::string deep = "{\"a\":";
    for (int i = 0; i < 100; ++i) deep += "[";
    for (int i = 0; i < 100; ++i) deep += "]";
    deep += "}";
    CHECK_FALSE(parse_json_object(deep).has_value());
    std::string ok = "{\"a\":";
    for (int i = 0; i < 20; ++i) ok += "{\"b\":";
    ok += "1";
    for (int i = 0; i < 21; ++i) ok += "}";
    auto r = parse_json_object(ok);
    REQUIRE(r);
    CHECK(r->size() == 1);
}

TEST_CASE("writing JSON") {
    Record r{{"msg", S("say \"hi\"\n")}, {"n", I(3)}, {"x", F(0.5)}, {"ok", B(false)}, {"none", Value{}},
             {"_time", Value::time(at(2026, 11, 15))}, {"_raw", S("raw line")}};
    CHECK(record_to_json(r) ==
          R"({"msg":"say \"hi\"\n","n":3,"x":0.5,"ok":false,"none":null,"_time":"2026-11-15T00:00:00Z"})");
    CHECK(record_to_json(r, true).find("\"_raw\":\"raw line\"") != std::string::npos);
    CHECK(value_to_json(S(std::string("\x01", 1))) == "\"\\u0001\"");

    // Round trip through the parser.
    auto back = parse_json_object(record_to_json(r));
    REQUIRE(back);
    CHECK(back->get("msg") == S("say \"hi\"\n"));
    CHECK(back->get("x") == F(0.5));
}
