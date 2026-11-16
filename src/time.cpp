// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "logsift/time.hpp"

#include <array>
#include <cmath>
#include <cstdio>

#include "logsift/text.hpp"

namespace logsift {

namespace {

constexpr std::int64_t days_from_civil(std::int64_t y, int m, int d) noexcept {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t mp = m > 2 ? m - 3 : m + 9;
    const std::int64_t doy = (153 * mp + 2) / 5 + d - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

struct Ymd {
    int y;
    int m;
    int d;
};

constexpr Ymd civil_from_days(std::int64_t z) noexcept {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = yoe + era * 400;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const std::int64_t m = mp < 10 ? mp + 3 : mp - 9;
    return Ymd{static_cast<int>(y + (m <= 2 ? 1 : 0)), static_cast<int>(m), static_cast<int>(d)};
}

std::int64_t floor_div(std::int64_t a, std::int64_t b) noexcept {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

bool leap(int y) noexcept { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int days_in_month(int y, int m) noexcept {
    static constexpr std::array<int, 12> kDays{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && leap(y)) return 29;
    return kDays[static_cast<std::size_t>(m - 1)];
}

constexpr std::array<std::string_view, 12> kMonths{"jan", "feb", "mar", "apr", "may", "jun",
                                                   "jul", "aug", "sep", "oct", "nov", "dec"};

int month_from_name(std::string_view s) {
    if (s.size() != 3) return 0;
    for (std::size_t i = 0; i < kMonths.size(); ++i) {
        if (iequals(s, kMonths[i])) return static_cast<int>(i) + 1;
    }
    return 0;
}

// Minimal cursor over the text being parsed.
class Scanner {
public:
    explicit Scanner(std::string_view s) : s_(s) {}

    bool done() const { return pos_ >= s_.size(); }
    char peek() const { return done() ? '\0' : s_[pos_]; }
    bool accept(char c) {
        if (peek() != c) return false;
        ++pos_;
        return true;
    }
    bool digits(std::size_t count, int& out) {
        if (s_.size() - pos_ < count) return false;
        int v = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const char c = s_[pos_ + i];
            if (c < '0' || c > '9') return false;
            v = v * 10 + (c - '0');
        }
        pos_ += count;
        out = v;
        return true;
    }
    // Up to `max` digits as a fraction scaled to nanoseconds.
    bool fraction(std::int64_t& nanos) {
        std::int64_t v = 0;
        std::size_t n = 0;
        while (!done() && peek() >= '0' && peek() <= '9') {
            if (n < 9) {
                v = v * 10 + (peek() - '0');
                ++n;
            }
            ++pos_;
        }
        if (n == 0) return false;
        while (n++ < 9) v *= 10;
        nanos = v;
        return true;
    }
    std::string_view take(std::size_t n) {
        const auto out = s_.substr(pos_, n);
        pos_ += out.size();
        return out;
    }
    void skip_spaces() {
        while (peek() == ' ') ++pos_;
    }

private:
    std::string_view s_;
    std::size_t pos_ = 0;
};

// Parses Z, +HH:MM, +HHMM or +HH. Returns the offset in seconds.
bool parse_zone(Scanner& sc, std::int64_t& offset) {
    offset = 0;
    if (sc.accept('Z') || sc.accept('z')) return true;
    int sign = 0;
    if (sc.accept('+')) {
        sign = 1;
    } else if (sc.accept('-')) {
        sign = -1;
    } else {
        return true;  // no zone: UTC
    }
    int hh = 0;
    int mm = 0;
    if (!sc.digits(2, hh)) return false;
    sc.accept(':');
    if (!sc.done() && !sc.digits(2, mm)) return false;
    if (hh > 23 || mm > 59) return false;
    offset = sign * (hh * 3600 + mm * 60);
    return true;
}

std::optional<TimePoint> apply_offset(std::optional<TimePoint> t, std::int64_t offset_seconds) {
    if (!t) return std::nullopt;
    return TimePoint{t->ns - offset_seconds * kSecond};
}

std::optional<TimePoint> parse_iso(std::string_view text) {
    Scanner sc(text);
    int y = 0;
    int mo = 0;
    int d = 0;
    if (!sc.digits(4, y) || !sc.accept('-') || !sc.digits(2, mo) || !sc.accept('-') || !sc.digits(2, d)) {
        return std::nullopt;
    }
    if (sc.done()) return make_time(y, mo, d);
    if (!sc.accept('T') && !sc.accept('t') && !sc.accept(' ')) return std::nullopt;
    int h = 0;
    int mi = 0;
    int s = 0;
    std::int64_t nanos = 0;
    if (!sc.digits(2, h) || !sc.accept(':') || !sc.digits(2, mi)) return std::nullopt;
    if (sc.accept(':')) {
        if (!sc.digits(2, s)) return std::nullopt;
        if ((sc.accept('.') || sc.accept(',')) && !sc.fraction(nanos)) return std::nullopt;
    }
    sc.skip_spaces();
    std::int64_t offset = 0;
    if (!parse_zone(sc, offset) || !sc.done()) return std::nullopt;
    return apply_offset(make_time(y, mo, d, h, mi, s, nanos), offset);
}

// 15/Nov/2026:09:57:03 -0500
std::optional<TimePoint> parse_clf(std::string_view text) {
    Scanner sc(text);
    int d = 0;
    int y = 0;
    int h = 0;
    int mi = 0;
    int s = 0;
    if (!sc.digits(2, d) || !sc.accept('/')) return std::nullopt;
    const int mo = month_from_name(sc.take(3));
    if (mo == 0 || !sc.accept('/') || !sc.digits(4, y) || !sc.accept(':') || !sc.digits(2, h) || !sc.accept(':') ||
        !sc.digits(2, mi) || !sc.accept(':') || !sc.digits(2, s)) {
        return std::nullopt;
    }
    std::int64_t nanos = 0;
    if (sc.accept('.') && !sc.fraction(nanos)) return std::nullopt;
    sc.skip_spaces();
    std::int64_t offset = 0;
    if (!parse_zone(sc, offset) || !sc.done()) return std::nullopt;
    return apply_offset(make_time(y, mo, d, h, mi, s, nanos), offset);
}

// Nov 15 09:57:03  (day may be space padded: "Nov  5 09:57:03")
std::optional<TimePoint> parse_bsd(std::string_view text, int year) {
    Scanner sc(text);
    const int mo = month_from_name(sc.take(3));
    if (mo == 0 || !sc.accept(' ')) return std::nullopt;
    sc.skip_spaces();
    int d = 0;
    if (!sc.digits(2, d)) {
        if (!sc.digits(1, d)) return std::nullopt;
    }
    int h = 0;
    int mi = 0;
    int s = 0;
    if (!sc.accept(' ') || !sc.digits(2, h) || !sc.accept(':') || !sc.digits(2, mi) || !sc.accept(':') ||
        !sc.digits(2, s)) {
        return std::nullopt;
    }
    std::int64_t nanos = 0;
    if (sc.accept('.') && !sc.fraction(nanos)) return std::nullopt;
    if (!sc.done()) return std::nullopt;
    return make_time(year, mo, d, h, mi, s, nanos);
}

}  // namespace

std::optional<TimePoint> make_time(int year, int month, int day, int hour, int minute, int second,
                                   std::int64_t nanos) {
    // int64 nanoseconds cover roughly 1678-2262.
    if (year < 1678 || year > 2261) return std::nullopt;
    if (month < 1 || month > 12 || day < 1 || day > days_in_month(year, month)) return std::nullopt;
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) return std::nullopt;
    if (nanos < 0 || nanos >= kSecond) return std::nullopt;
    const std::int64_t days = days_from_civil(year, month, day);
    return TimePoint{days * kDay + hour * kHour + minute * kMinute + second * kSecond + nanos};
}

CivilTime to_civil(TimePoint t) {
    const std::int64_t days = floor_div(t.ns, kDay);
    std::int64_t rem = t.ns - days * kDay;
    const Ymd ymd = civil_from_days(days);
    CivilTime c{};
    c.year = ymd.y;
    c.month = ymd.m;
    c.day = ymd.d;
    c.hour = static_cast<int>(rem / kHour);
    rem %= kHour;
    c.minute = static_cast<int>(rem / kMinute);
    rem %= kMinute;
    c.second = static_cast<int>(rem / kSecond);
    c.nanos = rem % kSecond;
    c.weekday = static_cast<int>(((days % 7) + 7 + 4) % 7);  // 1970-01-01 was a Thursday
    return c;
}

std::optional<TimePoint> parse_time(std::string_view text, int default_year) {
    text = trim(text);
    if (text.size() < 8) return std::nullopt;
    if (text[0] >= '0' && text[0] <= '9') {
        if (text.size() > 4 && text[4] == '-') return parse_iso(text);
        if (text.size() > 2 && text[2] == '/') return parse_clf(text);
        return std::nullopt;
    }
    return parse_bsd(text, default_year);
}

std::optional<TimePoint> parse_epoch(double v) {
    if (!std::isfinite(v) || v < 0) return std::nullopt;
    double ns = 0;
    if (v < 1e11) {
        ns = v * 1e9;
    } else if (v < 1e14) {
        ns = v * 1e6;
    } else if (v < 1e17) {
        ns = v * 1e3;
    } else {
        ns = v;
    }
    if (ns >= 9.2e18) return std::nullopt;
    return TimePoint{std::llround(ns)};
}

std::string format_time(TimePoint t) {
    const CivilTime c = to_civil(t);
    char buf[96];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d", c.year, c.month, c.day, c.hour, c.minute,
                  c.second);
    std::string out(buf);
    if (c.nanos != 0) {
        char frac[32];
        if (c.nanos % kMillisecond == 0) {
            std::snprintf(frac, sizeof frac, ".%03lld", static_cast<long long>(c.nanos / kMillisecond));
        } else if (c.nanos % kMicrosecond == 0) {
            std::snprintf(frac, sizeof frac, ".%06lld", static_cast<long long>(c.nanos / kMicrosecond));
        } else {
            std::snprintf(frac, sizeof frac, ".%09lld", static_cast<long long>(c.nanos));
        }
        out += frac;
    }
    out += 'Z';
    return out;
}

std::optional<Duration> parse_duration(std::string_view text) {
    text = trim(text);
    if (text.empty()) return std::nullopt;
    bool negative = false;
    if (text.front() == '-') {
        negative = true;
        text.remove_prefix(1);
    }
    double total = 0;
    std::size_t i = 0;
    bool any = false;
    while (i < text.size()) {
        const std::size_t start = i;
        while (i < text.size() && ((text[i] >= '0' && text[i] <= '9') || text[i] == '.')) ++i;
        if (i == start) return std::nullopt;
        double amount = 0;
        {
            // Manual decimal parse: digits[.digits]
            const std::string_view num = text.substr(start, i - start);
            if (num.find('.') != num.rfind('.') || num == ".") return std::nullopt;
            double whole = 0;
            double scale = 0;
            for (char c : num) {
                if (c == '.') {
                    scale = 0.1;
                    continue;
                }
                if (scale == 0) {
                    whole = whole * 10 + (c - '0');
                } else {
                    whole += (c - '0') * scale;
                    scale /= 10;
                }
            }
            amount = whole;
        }
        const std::size_t unit_start = i;
        while (i < text.size() && ((text[i] >= 'a' && text[i] <= 'z') || static_cast<unsigned char>(text[i]) >= 0x80)) {
            ++i;
        }
        const std::string_view unit = text.substr(unit_start, i - unit_start);
        double scale = 0;
        if (unit == "ns") {
            scale = 1;
        } else if (unit == "us" || unit == "\xC2\xB5s") {
            scale = static_cast<double>(kMicrosecond);
        } else if (unit == "ms") {
            scale = static_cast<double>(kMillisecond);
        } else if (unit == "s") {
            scale = static_cast<double>(kSecond);
        } else if (unit == "m") {
            scale = static_cast<double>(kMinute);
        } else if (unit == "h") {
            scale = static_cast<double>(kHour);
        } else if (unit == "d") {
            scale = static_cast<double>(kDay);
        } else if (unit == "w") {
            scale = static_cast<double>(7 * kDay);
        } else {
            return std::nullopt;
        }
        total += amount * scale;
        any = true;
    }
    if (!any || total >= 9.2e18) return std::nullopt;
    const auto d = static_cast<Duration>(std::llround(total));
    return negative ? -d : d;
}

std::string format_duration(Duration d) {
    if (d == 0) return "0s";
    std::string out;
    if (d < 0) {
        out += '-';
        d = -d;
    }
    if (d < kSecond) {
        if (d % kMillisecond == 0) return out + std::to_string(d / kMillisecond) + "ms";
        if (d % kMicrosecond == 0) return out + std::to_string(d / kMicrosecond) + "us";
        return out + std::to_string(d) + "ns";
    }
    if (d >= kDay) {
        out += std::to_string(d / kDay) + "d";
        d %= kDay;
    }
    if (d >= kHour) {
        out += std::to_string(d / kHour) + "h";
        d %= kHour;
    }
    if (d >= kMinute) {
        out += std::to_string(d / kMinute) + "m";
        d %= kMinute;
    }
    if (d > 0) {
        out += std::to_string(d / kSecond);
        std::int64_t frac = d % kSecond;
        if (frac != 0) {
            std::string digits = std::to_string(frac + kSecond).substr(1);  // zero-padded to 9
            while (!digits.empty() && digits.back() == '0') digits.pop_back();
            out += "." + digits;
        }
        out += "s";
    }
    return out;
}

TimePoint floor_time(TimePoint t, Duration span) {
    if (span <= 0) return t;
    return TimePoint{floor_div(t.ns, span) * span};
}

}  // namespace logsift
