// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// logfmt: space-separated key=value pairs. Quoted values may contain
// spaces and \" \\ \n \t escapes. A bare key is a boolean flag.

#include "logsift/formats.hpp"

namespace logsift {

std::optional<Record> parse_logfmt(std::string_view line) {
    Record r;
    std::size_t i = 0;
    std::size_t pairs = 0;
    const std::size_t n = line.size();
    while (i < n) {
        while (i < n && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= n) break;
        const std::size_t key_start = i;
        while (i < n && line[i] != '=' && line[i] != ' ' && line[i] != '\t' && line[i] != '"') ++i;
        const std::string_view key = line.substr(key_start, i - key_start);
        if (key.empty()) return std::nullopt;  // stray '=' or '"'
        if (i >= n || line[i] == ' ' || line[i] == '\t') {
            r.set(key, Value::boolean(true));
            continue;
        }
        if (line[i] == '"') return std::nullopt;  // quote inside a key
        ++i;                                      // '='
        if (i < n && line[i] == '"') {
            ++i;
            std::string value;
            bool closed = false;
            while (i < n) {
                const char c = line[i++];
                if (c == '"') {
                    closed = true;
                    break;
                }
                if (c == '\\' && i < n) {
                    const char e = line[i++];
                    switch (e) {
                        case 'n': value += '\n'; break;
                        case 't': value += '\t'; break;
                        case 'r': value += '\r'; break;
                        default: value += e; break;
                    }
                    continue;
                }
                value += c;
            }
            if (!closed) return std::nullopt;
            if (i < n && line[i] != ' ' && line[i] != '\t') return std::nullopt;
            r.set(key, Value::string(std::move(value)));
        } else {
            const std::size_t value_start = i;
            while (i < n && line[i] != ' ' && line[i] != '\t') ++i;
            const std::string_view text = line.substr(value_start, i - value_start);
            if (text.find('"') != std::string_view::npos) return std::nullopt;
            r.set(key, text.empty() ? Value::string("") : infer_value(text));
        }
        ++pairs;
    }
    if (pairs == 0) return std::nullopt;
    return r;
}

}  // namespace logsift
