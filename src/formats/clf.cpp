// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Common and Combined Log Format (Apache, nginx):
//   127.0.0.1 - frank [15/Nov/2026:09:57:03 -0500] "GET /a.gif HTTP/1.1" 200 2326 "ref" "agent"

#include "logsift/formats.hpp"
#include "logsift/text.hpp"

namespace logsift {

namespace {

bool next_token(std::string_view& s, std::string_view& out) {
    const std::size_t sp = s.find(' ');
    if (sp == std::string_view::npos || sp == 0) return false;
    out = s.substr(0, sp);
    s.remove_prefix(sp + 1);
    return true;
}

// "..." with \" escapes. Leaves s after the closing quote.
bool quoted(std::string_view& s, std::string& out) {
    if (s.empty() || s.front() != '"') return false;
    std::size_t i = 1;
    while (i < s.size()) {
        const char c = s[i++];
        if (c == '\\' && i < s.size()) {
            out += s[i++];
            continue;
        }
        if (c == '"') {
            s.remove_prefix(i);
            return true;
        }
        out += c;
    }
    return false;
}

void set_dash_null(Record& r, std::string_view name, std::string_view v) {
    if (v != "-") r.set(name, Value::string(std::string(v)));
}

}  // namespace

std::optional<Record> parse_clf(std::string_view s) {
    Record r;
    std::string_view client;
    std::string_view ident;
    std::string_view user;
    if (!next_token(s, client) || !next_token(s, ident) || !next_token(s, user)) return std::nullopt;
    if (s.empty() || s.front() != '[') return std::nullopt;
    const std::size_t close = s.find(']');
    if (close == std::string_view::npos) return std::nullopt;
    const auto t = parse_time(s.substr(1, close - 1));
    if (!t) return std::nullopt;
    s.remove_prefix(close + 1);
    if (s.empty() || s.front() != ' ') return std::nullopt;
    s.remove_prefix(1);

    std::string request;
    if (!quoted(s, request) || s.empty() || s.front() != ' ') return std::nullopt;
    s.remove_prefix(1);
    std::string_view status;
    std::string_view bytes;
    if (!next_token(s, status)) return std::nullopt;
    // The byte count ends the line in Common Log Format; Combined continues.
    const std::size_t sp = s.find(' ');
    bytes = s.substr(0, sp);
    s = sp == std::string_view::npos ? std::string_view{} : s.substr(sp + 1);

    const auto status_code = parse_int(status);
    if (!status_code || *status_code < 100 || *status_code > 999) return std::nullopt;
    if (bytes != "-" && !parse_int(bytes)) return std::nullopt;

    r.set(fields::kTime, Value::time(*t));
    r.set("client", Value::string(std::string(client)));
    set_dash_null(r, "ident", ident);
    set_dash_null(r, "user", user);
    const auto parts = split(request, ' ');
    if (parts.size() == 3) {
        r.set("method", Value::string(std::string(parts[0])));
        r.set("path", Value::string(std::string(parts[1])));
        r.set("protocol", Value::string(std::string(parts[2])));
    } else {
        r.set("request", Value::string(request));
    }
    r.set("status", Value::integer(*status_code));
    if (bytes != "-") r.set("bytes", Value::integer(*parse_int(bytes)));

    if (!s.empty()) {
        std::string referer;
        std::string agent;
        if (!quoted(s, referer)) return std::nullopt;
        if (!s.empty()) {
            if (s.front() != ' ') return std::nullopt;
            s.remove_prefix(1);
            if (!quoted(s, agent)) return std::nullopt;
        }
        if (!trim(s).empty()) return std::nullopt;
        if (referer != "-") r.set("referer", Value::string(std::move(referer)));
        if (!agent.empty() && agent != "-") r.set("user_agent", Value::string(std::move(agent)));
    }
    return r;
}

}  // namespace logsift
