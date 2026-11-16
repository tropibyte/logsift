// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Timestamps and durations. All times are UTC, stored as nanoseconds since
// the Unix epoch.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace logsift {

/// Nanoseconds since 1970-01-01T00:00:00Z.
struct TimePoint {
    std::int64_t ns = 0;
    friend constexpr auto operator<=>(const TimePoint&, const TimePoint&) = default;
};

/// A length of time in nanoseconds.
using Duration = std::int64_t;

inline constexpr Duration kNanosecond = 1;
inline constexpr Duration kMicrosecond = 1'000;
inline constexpr Duration kMillisecond = 1'000'000;
inline constexpr Duration kSecond = 1'000'000'000;
inline constexpr Duration kMinute = 60 * kSecond;
inline constexpr Duration kHour = 60 * kMinute;
inline constexpr Duration kDay = 24 * kHour;

/// Builds a time from calendar fields (UTC). Returns nullopt for invalid
/// dates such as February 30.
std::optional<TimePoint> make_time(int year, int month, int day, int hour = 0, int minute = 0,
                                   int second = 0, std::int64_t nanos = 0);

struct CivilTime {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    std::int64_t nanos;
    int weekday;  // 0 = Sunday
};

CivilTime to_civil(TimePoint t);

/// Parses a formatted timestamp:
///   RFC 3339 / ISO 8601   2026-11-15T09:57:03Z, 2026-11-15 09:57:03.25+05:30
///   Common Log Format     15/Nov/2026:09:57:03 -0500
///   BSD syslog            Nov 15 09:57:03   (year taken from default_year)
/// Times without an offset are taken as UTC. Bare numbers are not accepted;
/// see parse_epoch().
std::optional<TimePoint> parse_time(std::string_view text, int default_year = 1970);

/// Interprets a number as a Unix timestamp, choosing the unit by magnitude:
/// seconds below 1e11, milliseconds below 1e14, microseconds below 1e17,
/// nanoseconds above.
std::optional<TimePoint> parse_epoch(double value);

/// RFC 3339 in UTC: "2026-11-15T09:57:03Z", with fractional seconds
/// (3, 6 or 9 digits) only when needed.
std::string format_time(TimePoint t);

/// Parses durations such as "250ms", "5s", "1h30m", "2d", "1.5h".
/// Units: ns, us, ms, s, m, h, d, w.
std::optional<Duration> parse_duration(std::string_view text);

/// Short display form: "1h30m", "250ms", "0s".
std::string format_duration(Duration d);

/// Rounds t down to a multiple of span (span > 0), counting from the epoch.
TimePoint floor_time(TimePoint t, Duration span);

}  // namespace logsift
