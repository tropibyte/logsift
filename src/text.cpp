// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "logsift/text.hpp"

#include <cstdio>

namespace logsift {

namespace {

char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
char upper(char c) noexcept { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }
bool continuation(char c) noexcept { return (static_cast<unsigned char>(c) & 0xC0u) == 0x80u; }

}  // namespace

std::string_view trim(std::string_view s) noexcept {
    std::size_t b = 0;
    std::size_t e = s.size();
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (b < e && space(s[b])) ++b;
    while (e > b && space(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = lower(c);
    return out;
}

std::string to_upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = upper(c);
    return out;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

bool starts_with(std::string_view s, std::string_view prefix) noexcept {
    return s.substr(0, prefix.size()) == prefix;
}

bool ends_with(std::string_view s, std::string_view suffix) noexcept {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

std::vector<std::string_view> split(std::string_view s, char delimiter) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t pos = s.find(delimiter, start);
        if (pos == std::string_view::npos) {
            out.push_back(s.substr(start));
            return out;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
}

std::size_t display_width(std::string_view s) noexcept {
    std::size_t n = 0;
    for (char c : s) {
        if (!continuation(c)) ++n;
    }
    return n;
}

std::string truncate_display(std::string_view s, std::size_t width) {
    if (display_width(s) <= width) return std::string(s);
    if (width == 0) return {};
    std::string out;
    std::size_t count = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (!continuation(s[i])) {
            if (count == width - 1) break;
            ++count;
        }
        out += s[i];
    }
    out += "\xE2\x80\xA6";  // …
    return out;
}

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

void append_utf8(std::string& out, char32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

}  // namespace logsift
