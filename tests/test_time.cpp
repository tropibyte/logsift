// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "helpers.hpp"

using namespace logsift;
using namespace logsift::test;

TEST_CASE("make_time and to_civil") {
    CHECK(make_time(1970, 1, 1)->ns == 0);
    CHECK(make_time(2026, 11, 15, 9, 57, 3)->ns == 1'794'736'623LL * kSecond);
    CHECK_FALSE(make_time(2026, 2, 29).has_value());
    CHECK(make_time(2024, 2, 29).has_value());
    CHECK_FALSE(make_time(2026, 13, 1).has_value());
    CHECK_FALSE(make_time(2026, 1, 1, 24).has_value());
    CHECK_FALSE(make_time(1500, 1, 1).has_value());

    const CivilTime c = to_civil(TimePoint{at(2026, 11, 15, 9, 57, 3).ns + 250 * kMillisecond});
    CHECK(c.year == 2026);
    CHECK(c.month == 11);
    CHECK(c.day == 15);
    CHECK(c.hour == 9);
    CHECK(c.minute == 57);
    CHECK(c.second == 3);
    CHECK(c.nanos == 250 * kMillisecond);
    CHECK(c.weekday == 0);  // a Sunday
    CHECK(to_civil(TimePoint{-1}).year == 1969);
    CHECK(to_civil(TimePoint{-1}).second == 59);
}

TEST_CASE("parse_time formats") {
    const TimePoint base = at(2026, 11, 15, 9, 57, 3);
    CHECK(parse_time("2026-11-15T09:57:03Z") == base);
    CHECK(parse_time("2026-11-15t09:57:03z") == base);
    CHECK(parse_time("2026-11-15 09:57:03") == base);
    CHECK(parse_time("2026-11-15T04:57:03-05:00") == base);
    CHECK(parse_time("2026-11-15T15:27:03+0530") == base);
    CHECK(parse_time("2026-11-15T10:57:03+01") == base);
    CHECK(parse_time("2026-11-15T09:57:03.5Z")->ns == base.ns + 500 * kMillisecond);
    CHECK(parse_time("2026-11-15T09:57:03,123456789Z")->ns == base.ns + 123456789);
    CHECK(parse_time("2026-11-15T09:57:03.1234567891Z")->ns == base.ns + 123456789);  // extra digits ignored
    CHECK(parse_time("2026-11-15T09:57Z") == at(2026, 11, 15, 9, 57));
    CHECK(parse_time("2026-11-15") == at(2026, 11, 15));
    CHECK(parse_time("15/Nov/2026:04:57:03 -0500") == base);
    CHECK(parse_time("Nov 15 09:57:03", 2026) == base);
    CHECK(parse_time("Nov  5 09:57:03", 2026) == at(2026, 11, 5, 9, 57, 3));
    CHECK(parse_time("  2026-11-15T09:57:03Z  ") == base);

    for (const char* bad : {"", "1700000000", "2026-11-15T", "2026-11-31", "2026-11-15T25:00:00Z", "15/Foo/2026:00:00:00",
                            "Nov 15", "2026-11-15T09:57:03 extra", "2026-11-15T09:57:03+99:00", "hello world"}) {
        CAPTURE(bad);
        CHECK_FALSE(parse_time(bad).has_value());
    }
}

TEST_CASE("parse_epoch picks the unit by magnitude") {
    const std::int64_t s = 1'794'736'623;
    CHECK(parse_epoch(static_cast<double>(s))->ns == s * kSecond);
    CHECK(parse_epoch(1794736623.5)->ns == s * kSecond + 500 * kMillisecond);
    CHECK(parse_epoch(static_cast<double>(s) * 1e3)->ns == s * kSecond);
    CHECK(parse_epoch(static_cast<double>(s) * 1e6)->ns == s * kSecond);
    CHECK(parse_epoch(static_cast<double>(s) * 1e9)->ns == s * kSecond);
    CHECK_FALSE(parse_epoch(-1).has_value());
    CHECK_FALSE(parse_epoch(1e30).has_value());
}

TEST_CASE("format_time uses the shortest fraction") {
    const TimePoint base = at(2026, 11, 15, 9, 57, 3);
    CHECK(format_time(base) == "2026-11-15T09:57:03Z");
    CHECK(format_time(TimePoint{base.ns + 120 * kMillisecond}) == "2026-11-15T09:57:03.120Z");
    CHECK(format_time(TimePoint{base.ns + 5 * kMicrosecond}) == "2026-11-15T09:57:03.000005Z");
    CHECK(format_time(TimePoint{base.ns + 7}) == "2026-11-15T09:57:03.000000007Z");
    CHECK(format_time(TimePoint{-kSecond}) == "1969-12-31T23:59:59Z");
    CHECK(parse_time(format_time(TimePoint{base.ns + 123456789})) == TimePoint{base.ns + 123456789});
}

TEST_CASE("durations") {
    CHECK(parse_duration("250ms") == 250 * kMillisecond);
    CHECK(parse_duration("1h30m") == 90 * kMinute);
    CHECK(parse_duration("1.5h") == 90 * kMinute);
    CHECK(parse_duration("2d") == 2 * kDay);
    CHECK(parse_duration("1w") == 7 * kDay);
    CHECK(parse_duration("10us") == 10 * kMicrosecond);
    CHECK(parse_duration("10\xC2\xB5s") == 10 * kMicrosecond);
    CHECK(parse_duration("-5s") == -5 * kSecond);
    CHECK(parse_duration("3ns") == 3);
    for (const char* bad : {"", "5", "m", "5x", "1.2.3s", "5s5", "1e3s"}) {
        CAPTURE(bad);
        CHECK_FALSE(parse_duration(bad).has_value());
    }
    CHECK(format_duration(0) == "0s");
    CHECK(format_duration(250 * kMillisecond) == "250ms");
    CHECK(format_duration(90 * kMinute) == "1h30m");
    CHECK(format_duration(kDay + 5 * kSecond) == "1d5s");
    CHECK(format_duration(1500 * kMillisecond) == "1.5s");
    CHECK(format_duration(-2 * kMinute) == "-2m");
    CHECK(format_duration(7) == "7ns");
}

TEST_CASE("floor_time") {
    const TimePoint t = at(2026, 11, 15, 9, 57, 3);
    CHECK(floor_time(t, 5 * kMinute) == at(2026, 11, 15, 9, 55));
    CHECK(floor_time(t, kHour) == at(2026, 11, 15, 9));
    CHECK(floor_time(t, kDay) == at(2026, 11, 15));
    CHECK(floor_time(TimePoint{-1}, kSecond) == TimePoint{-kSecond});
    CHECK(floor_time(t, 0) == t);
}
