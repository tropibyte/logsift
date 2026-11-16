// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

#include "helpers.hpp"

using namespace logsift;
using namespace logsift::test;

TEST_CASE("value types and accessors") {
    CHECK(Value{}.is_null());
    CHECK(I(5).is_int());
    CHECK(I(5).is_number());
    CHECK(F(1.5).is_float());
    CHECK(S("x").is_string());
    CHECK(Value::time(TimePoint{1}).is_time());
    CHECK(type_name(Type::Float) == "float");
    CHECK_THROWS(I(1).as_string());
    CHECK(I(1) != F(1.0));  // structural equality distinguishes Int and Float
}

TEST_CASE("to_string and truthiness") {
    CHECK(Value{}.to_string().empty());
    CHECK(B(true).to_string() == "true");
    CHECK(I(-42).to_string() == "-42");
    CHECK(F(0.1).to_string() == "0.1");
    CHECK(F(2.0).to_string() == "2");
    CHECK(Value::time(at(2026, 11, 15, 9, 57, 3)).to_string() == "2026-11-15T09:57:03Z");
    CHECK_FALSE(Value{}.truthy());
    CHECK_FALSE(I(0).truthy());
    CHECK_FALSE(S("").truthy());
    CHECK(S("0").truthy());
    CHECK(F(0.5).truthy());
}

TEST_CASE("numeric parsing") {
    CHECK(parse_int("42") == 42);
    CHECK(parse_int("-9223372036854775808") == std::numeric_limits<std::int64_t>::min());
    CHECK_FALSE(parse_int("9223372036854775808").has_value());
    CHECK_FALSE(parse_int("12a").has_value());
    CHECK_FALSE(parse_int("").has_value());
    CHECK_FALSE(parse_int("-").has_value());
    CHECK(parse_double("1e3") == 1000.0);
    CHECK(parse_double("-.5") == -0.5);
    CHECK_FALSE(parse_double(" 1").has_value());
    CHECK_FALSE(parse_double("inf").has_value());
    CHECK(format_double(1.0 / 3.0) == "0.3333333333333333");
}

TEST_CASE("format_double without std::to_chars") {
    // Regression: where std::to_chars is missing (libc++ on macOS), the
    // fallback took the first %.*g text that read back, so 80 printed as
    // "8e+01" and 1500 as "1.5e+03".
    using detail::format_double_printf;
    CHECK(format_double_printf(80) == "80");
    CHECK(format_double_printf(100) == "100");
    CHECK(format_double_printf(1500) == "1500");
    CHECK(format_double_printf(100000) == "1e+05");     // shorter than 100000
    CHECK(format_double_printf(120000) == "120000");    // a tie with 1.2e+05 goes to fixed
    CHECK(format_double_printf(0.1) == "0.1");
    CHECK(format_double_printf(0.001) == "0.001");      // a tie with 1e-03 goes to fixed
    CHECK(format_double_printf(0.0001) == "1e-04");     // shorter than 0.0001
    CHECK(format_double_printf(32944984306792075264.0) == "32944984306792075264");  // exact, as to_chars
    // At a power of two the correctly rounded 16 digits do not read back,
    // but the 16-digit neighbour above does.
    CHECK(format_double_printf(std::ldexp(1.0, -44)) == "5.684341886080802e-14");
    CHECK(format_double_printf(-2.5) == "-2.5");
    CHECK(format_double_printf(-0.0) == "0");
    CHECK(format_double_printf(123.456) == "123.456");
    CHECK(format_double_printf(1.0 / 3.0) == "0.3333333333333333");
    CHECK(format_double_printf(1e300) == "1e+300");
    CHECK(format_double_printf(1.5e-300) == "1.5e-300");
    CHECK(format_double_printf(9007199254740993.0) == "9007199254740992");

#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    // Where std::to_chars exists, the fallback must agree with it exactly,
    // so output and index keys are the same on every platform.
    std::uint64_t state = 0x9E3779B97F4A7C15u;
    auto next = [&] {  // xorshift64
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    auto to_chars_text = [](double v) {
        char buf[64];
        const auto result = std::to_chars(buf, buf + sizeof buf, v);
        return std::string(buf, result.ptr);
    };
    int checked = 0;
    for (int i = 0; i < 10'000; ++i) {
        double v = 0;
        switch (i % 5) {
            case 0: {  // any bit pattern: subnormals, huge and tiny exponents
                const std::uint64_t bits = next();
                std::memcpy(&v, &bits, sizeof v);
                break;
            }
            case 1: v = static_cast<double>(static_cast<std::int64_t>(next() % 2'000'000'000) - 1'000'000'000); break;
            case 2: v = static_cast<double>(next() % 100'000'000) / std::pow(10.0, static_cast<double>(next() % 12)); break;
            case 3: v = std::ldexp(1.0, static_cast<int>(next() % 2098) - 1074); break;  // powers of two
            default: v = static_cast<double>(next() >> (next() % 64)); break;          // integers of every size
        }
        if (!std::isfinite(v) || v == 0) continue;
        CAPTURE(v);
        REQUIRE(format_double_printf(v) == to_chars_text(v));
        ++checked;
    }
    CHECK(checked > 9'000);
#endif
}

TEST_CASE("infer_value") {
    CHECK(infer_value("12") == I(12));
    CHECK(infer_value("-1.5") == F(-1.5));
    CHECK(infer_value("true") == B(true));
    CHECK(infer_value("TRUE") == S("TRUE"));
    CHECK(infer_value("12ms") == S("12ms"));
    CHECK(infer_value("99999999999999999999") == F(1e20));
}

TEST_CASE("compare_total orders kinds") {
    const std::vector<Value> ordered{Value{}, B(false), B(true), I(-3), F(-2.5), I(0), F(0.5), I(10),
                                     Value::time(TimePoint{0}), Value::time(TimePoint{5}), S(""), S("A"), S("a")};
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        for (std::size_t j = 0; j < ordered.size(); ++j) {
            CAPTURE(i);
            CAPTURE(j);
            const int c = compare_total(ordered[i], ordered[j]);
            if (i < j) CHECK(c < 0);
            if (i > j) CHECK(c > 0);
            if (i == j) CHECK(c == 0);
        }
    }
    CHECK(compare_total(I(2), F(2.0)) == 0);
}

TEST_CASE("compare_query semantics") {
    CHECK(compare_query(I(5), F(5.0)) == 0);
    CHECK(compare_query(I(5), S("10")) == -1);  // numeric when one side is a number
    CHECK(compare_query(S("10"), S("9")) == -1);  // byte-wise for two strings
    CHECK_FALSE(compare_query(I(5), S("abc")).has_value());
    CHECK_FALSE(compare_query(Value{}, Value{}).has_value());
    CHECK_FALSE(compare_query(I(1), B(true)).has_value());
    CHECK(compare_query(B(true), S("true")) == 0);
    CHECK_FALSE(compare_query(B(true), S("TRUE")).has_value());
    const Value t = Value::time(at(2026, 11, 15, 10));
    CHECK(compare_query(t, S("2026-11-15T09:00:00Z")) == 1);
    CHECK(compare_query(S("2026-11-15T11:00:00+02:00"), t) == -1);
    CHECK_FALSE(compare_query(t, I(0)).has_value());
    CHECK(equals_query(Value{}, Value{}));
    CHECK_FALSE(equals_query(Value{}, I(0)));
    CHECK(equals_query(S("7"), I(7)));
}

TEST_CASE("values equal under equals_query share an index key") {
    const std::vector<Value> values{
        I(0),     F(0.0),    F(-0.0),   I(500),    F(500.0),  S("500"),  S("500.0"), S("5e2"),
        B(true),  S("true"), S("True"), B(false),  S("x"),    S("X"),    F(0.1),     S("0.1"),
        Value::time(at(2026, 1, 2)),     S("2026-01-02T00:00:00Z"),      S("2026-01-02 01:00:00+01:00"),
        S("Jan  2 00:00:00"),            I(1767312000),
    };
    for (const Value& a : values) {
        for (const Value& b : values) {
            if (equals_query(a, b)) {
                CAPTURE(a);
                CAPTURE(b);
                CHECK(index_key(a) == index_key(b));
            }
        }
    }
    CHECK(index_key(S("x")) != index_key(S("X")));
}
