// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Syslog in RFC 5424 and RFC 3164 (BSD) forms.
//
//   <34>1 2026-11-15T09:57:03.003Z host app 1234 ID47 [ex@1 k="v"] message
//   <34>Nov 15 09:57:03 host app[1234]: message
//   Nov 15 09:57:03 host app: message            (no PRI, as in /var/log files)

#include <array>

#include "logsift/formats.hpp"

namespace logsift {

namespace {

constexpr std::array<std::string_view, 24> kFacilities{
    "kern",     "user",   "mail",   "daemon", "auth",   "syslog", "lpr",    "news",
    "uucp",     "cron",   "authpriv", "ftp",  "ntp",    "security", "console", "solaris-cron",
    "local0",   "local1", "local2", "local3", "local4", "local5", "local6", "local7"};

constexpr std::array<std::string_view, 8> kSeverities{"emerg", "alert",  "crit", "err",
                                                      "warning", "notice", "info", "debug"};

// Reads "<PRI>" if present; returns false on a malformed one.
bool read_pri(std::string_view& s, std::optional<int>& pri) {
    if (s.empty() || s.front() != '<') return true;
    const std::size_t close = s.find('>');
    if (close == std::string_view::npos || close < 2 || close > 4) return false;
    int v = 0;
    for (std::size_t i = 1; i < close; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
    }
    if (v > 191) return false;
    pri = v;
    s.remove_prefix(close + 1);
    return true;
}

void set_priority(Record& r, int pri) {
    r.set("priority", Value::integer(pri));
    r.set("facility", Value::string(std::string(kFacilities[static_cast<std::size_t>(pri / 8)])));
    const std::string severity(kSeverities[static_cast<std::size_t>(pri % 8)]);
    r.set("severity", Value::string(severity));
    r.set("level", Value::string(severity));
}

// Next space-separated token; '-' (nil) yields nullopt in `value`.
bool token(std::string_view& s, std::string_view& out) {
    if (s.empty()) return false;
    const std::size_t sp = s.find(' ');
    out = s.substr(0, sp);
    s = sp == std::string_view::npos ? std::string_view{} : s.substr(sp + 1);
    return !out.empty();
}

void set_unless_nil(Record& r, std::string_view name, std::string_view v, bool numeric = false) {
    if (v == "-") return;
    r.set(name, numeric ? infer_value(v) : Value::string(std::string(v)));
}

// [id k="v" k2="v2"][id2 ...]
bool parse_structured_data(std::string_view& s, Record& r) {
    if (!s.empty() && s.front() == '-') {
        s.remove_prefix(1);
        return true;
    }
    if (s.empty() || s.front() != '[') return false;
    while (!s.empty() && s.front() == '[') {
        s.remove_prefix(1);
        std::size_t i = 0;
        while (i < s.size() && s[i] != ' ' && s[i] != ']') ++i;
        const std::string id(s.substr(0, i));
        if (id.empty()) return false;
        s.remove_prefix(i);
        for (;;) {
            if (s.empty()) return false;
            if (s.front() == ']') {
                s.remove_prefix(1);
                break;
            }
            if (s.front() != ' ') return false;
            s.remove_prefix(1);
            const std::size_t eq = s.find('=');
            if (eq == std::string_view::npos || eq == 0 || eq + 1 >= s.size() || s[eq + 1] != '"') return false;
            const std::string name(s.substr(0, eq));
            s.remove_prefix(eq + 2);
            std::string value;
            bool closed = false;
            while (!s.empty()) {
                const char c = s.front();
                s.remove_prefix(1);
                if (c == '\\' && !s.empty() && (s.front() == '"' || s.front() == '\\' || s.front() == ']')) {
                    value += s.front();
                    s.remove_prefix(1);
                    continue;
                }
                if (c == '"') {
                    closed = true;
                    break;
                }
                value += c;
            }
            if (!closed) return false;
            r.set("sd." + id + "." + name, Value::string(std::move(value)));
        }
    }
    return true;
}

std::optional<Record> parse_5424(std::string_view s, int pri) {
    Record r;
    set_priority(r, pri);
    std::string_view ts;
    std::string_view host;
    std::string_view app;
    std::string_view proc;
    std::string_view msgid;
    if (!token(s, ts) || !token(s, host) || !token(s, app) || !token(s, proc) || !token(s, msgid)) {
        return std::nullopt;
    }
    if (ts != "-") {
        auto t = parse_time(ts);
        if (!t) return std::nullopt;
        r.set(fields::kTime, Value::time(*t));
    }
    set_unless_nil(r, "host", host);
    set_unless_nil(r, "app", app);
    set_unless_nil(r, "pid", proc, true);
    set_unless_nil(r, "msgid", msgid);
    if (!parse_structured_data(s, r)) return std::nullopt;
    if (!s.empty()) {
        if (s.front() != ' ') return std::nullopt;
        s.remove_prefix(1);
    }
    if (s.substr(0, 3) == "\xEF\xBB\xBF") s.remove_prefix(3);  // UTF-8 BOM
    r.set("message", Value::string(std::string(s)));
    return r;
}

std::optional<Record> parse_3164(std::string_view s, std::optional<int> pri, int default_year) {
    if (s.size() < 16) return std::nullopt;
    const auto t = parse_time(s.substr(0, 15), default_year);
    if (!t || s[15] != ' ') return std::nullopt;
    s.remove_prefix(16);
    Record r;
    r.set(fields::kTime, Value::time(*t));
    if (pri) set_priority(r, *pri);

    std::string_view host;
    if (!token(s, host)) return std::nullopt;
    r.set("host", Value::string(std::string(host)));

    // TAG[pid]: message
    std::size_t i = 0;
    while (i < s.size() && s[i] != '[' && s[i] != ':' && s[i] != ' ') ++i;
    if (i > 0 && i < s.size() && (s[i] == '[' || s[i] == ':')) {
        r.set("app", Value::string(std::string(s.substr(0, i))));
        std::string_view rest = s.substr(i);
        if (rest.front() == '[') {
            const std::size_t close = rest.find(']');
            if (close == std::string_view::npos) return std::nullopt;
            r.set("pid", infer_value(rest.substr(1, close - 1)));
            rest.remove_prefix(close + 1);
        }
        if (!rest.empty() && rest.front() == ':') {
            rest.remove_prefix(1);
            if (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
            r.set("message", Value::string(std::string(rest)));
            return r;
        }
        if (rest.empty() || rest.front() != ' ') return std::nullopt;
        rest.remove_prefix(1);
        r.set("message", Value::string(std::string(rest)));
        return r;
    }
    r.set("message", Value::string(std::string(s)));
    return r;
}

}  // namespace

std::optional<Record> parse_syslog(std::string_view line, int default_year) {
    std::string_view s = line;
    std::optional<int> pri;
    if (!read_pri(s, pri)) return std::nullopt;
    if (pri && s.size() > 2 && s[0] >= '1' && s[0] <= '9' && s[1] == ' ') {
        s.remove_prefix(2);
        return parse_5424(s, *pri);
    }
    return parse_3164(s, pri, default_year);
}

}  // namespace logsift
