// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "logsift/json.hpp"

#include <cmath>

#include "logsift/text.hpp"

namespace logsift {

namespace {

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : s_(text) {}

    bool parse_root(Record& out) {
        skip_ws();
        if (peek() != '{') return fail("expected '{'");
        if (!parse_object(out, "", 1)) return false;
        skip_ws();
        if (pos_ != s_.size()) return fail("unexpected text after the object");
        return true;
    }

    const std::string& error() const { return error_; }

private:
    char peek() const { return pos_ < s_.size() ? s_[pos_] : '\0'; }

    void skip_ws() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool fail(const std::string& message) {
        if (error_.empty()) error_ = message + " at offset " + std::to_string(pos_);
        return false;
    }

    bool expect(char c) {
        skip_ws();
        if (peek() != c) return fail(std::string("expected '") + c + "'");
        ++pos_;
        return true;
    }

    bool literal(std::string_view word) {
        if (s_.substr(pos_, word.size()) != word) return fail("invalid literal");
        pos_ += word.size();
        return true;
    }

    bool hex4(char32_t& out) {
        if (s_.size() - pos_ < 4) return fail("truncated \\u escape");
        char32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') {
                v |= static_cast<char32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                v |= static_cast<char32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                v |= static_cast<char32_t>(c - 'A' + 10);
            } else {
                return fail("invalid \\u escape");
            }
        }
        out = v;
        return true;
    }

    bool parse_string(std::string& out) {
        if (peek() != '"') return fail("expected string");
        ++pos_;
        for (;;) {
            if (pos_ >= s_.size()) return fail("unterminated string");
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) return fail("unterminated escape");
            const char e = s_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    char32_t cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && s_.substr(pos_, 2) == "\\u") {
                        pos_ += 2;
                        char32_t low = 0;
                        if (!hex4(low)) return false;
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            append_utf8(out, 0xFFFD);
                            cp = low;
                        }
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: return fail("invalid escape");
            }
        }
    }

    bool parse_number(Value& out) {
        const std::size_t start = pos_;
        if (peek() == '-') ++pos_;
        if (peek() == '0') {
            ++pos_;
        } else if (peek() >= '1' && peek() <= '9') {
            while (peek() >= '0' && peek() <= '9') ++pos_;
        } else {
            return fail("invalid number");
        }
        bool integral = true;
        if (peek() == '.') {
            integral = false;
            ++pos_;
            if (!(peek() >= '0' && peek() <= '9')) return fail("invalid number");
            while (peek() >= '0' && peek() <= '9') ++pos_;
        }
        if (peek() == 'e' || peek() == 'E') {
            integral = false;
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            if (!(peek() >= '0' && peek() <= '9')) return fail("invalid number");
            while (peek() >= '0' && peek() <= '9') ++pos_;
        }
        const std::string_view text = s_.substr(start, pos_ - start);
        if (integral) {
            if (auto i = parse_int(text)) {
                out = Value::integer(*i);
                return true;
            }
        }
        auto d = parse_double(text);
        if (!d) return fail("number out of range");
        out = Value::real(*d);
        return true;
    }

    // Skips any value, used to capture arrays as raw text.
    bool skip_value(int depth) {
        if (depth > kMaxJsonDepth) return fail("nesting too deep");
        skip_ws();
        const char c = peek();
        if (c == '"') {
            std::string ignored;
            return parse_string(ignored);
        }
        if (c == '{' || c == '[') {
            const char close = c == '{' ? '}' : ']';
            ++pos_;
            skip_ws();
            if (peek() == close) {
                ++pos_;
                return true;
            }
            for (;;) {
                if (c == '{') {
                    skip_ws();
                    std::string key;
                    if (!parse_string(key) || !expect(':')) return false;
                }
                if (!skip_value(depth + 1)) return false;
                skip_ws();
                if (peek() == ',') {
                    ++pos_;
                    continue;
                }
                if (peek() == close) {
                    ++pos_;
                    return true;
                }
                return fail("expected ',' or closing bracket");
            }
        }
        Value ignored;
        return parse_scalar(ignored);
    }

    bool parse_scalar(Value& out) {
        switch (peek()) {
            case '"': {
                std::string s;
                if (!parse_string(s)) return false;
                out = Value::string(std::move(s));
                return true;
            }
            case 't':
                out = Value::boolean(true);
                return literal("true");
            case 'f':
                out = Value::boolean(false);
                return literal("false");
            case 'n':
                out = Value{};
                return literal("null");
            default: return parse_number(out);
        }
    }

    bool parse_object(Record& out, const std::string& prefix, int depth) {
        if (depth > kMaxJsonDepth) return fail("nesting too deep");
        ++pos_;  // '{'
        skip_ws();
        if (peek() == '}') {
            ++pos_;
            if (!prefix.empty()) out.set(prefix.substr(0, prefix.size() - 1), Value::string("{}"));
            return true;
        }
        for (;;) {
            skip_ws();
            std::string key;
            if (!parse_string(key) || !expect(':')) return false;
            skip_ws();
            const std::string name = prefix + key;
            const char c = peek();
            if (c == '{') {
                if (!parse_object(out, name + ".", depth + 1)) return false;
            } else if (c == '[') {
                const std::size_t start = pos_;
                if (!skip_value(depth + 1)) return false;
                out.set(name, Value::string(compact(s_.substr(start, pos_ - start))));
            } else {
                Value v;
                if (!parse_scalar(v)) return false;
                out.set(name, std::move(v));
            }
            skip_ws();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == '}') {
                ++pos_;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    // Removes insignificant whitespace from already-validated JSON.
    static std::string compact(std::string_view json) {
        std::string out;
        bool in_string = false;
        for (std::size_t i = 0; i < json.size(); ++i) {
            const char c = json[i];
            if (in_string) {
                out += c;
                if (c == '\\' && i + 1 < json.size()) {
                    out += json[++i];
                } else if (c == '"') {
                    in_string = false;
                }
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
            if (c == '"') in_string = true;
            out += c;
        }
        return out;
    }

    std::string_view s_;
    std::size_t pos_ = 0;
    std::string error_;
};

}  // namespace

std::optional<Record> parse_json_object(std::string_view text, std::string* error) {
    JsonParser parser(text);
    Record r;
    if (!parser.parse_root(r)) {
        if (error) *error = parser.error();
        return std::nullopt;
    }
    return r;
}

std::string value_to_json(const Value& v) {
    switch (v.type()) {
        case Type::Null: return "null";
        case Type::Bool: return v.as_bool() ? "true" : "false";
        case Type::Int: return std::to_string(v.as_int());
        case Type::Float: return std::isfinite(v.as_float()) ? format_double(v.as_float()) : "null";
        case Type::String: return "\"" + json_escape(v.as_string()) + "\"";
        case Type::Time: return "\"" + format_time(v.as_time()) + "\"";
    }
    return "null";
}

std::string record_to_json(const Record& r, bool include_internal) {
    std::string out = "{";
    bool first = true;
    for (const auto& f : r.fields()) {
        if (!include_internal && is_internal_field(f.name) && f.name != fields::kTime) continue;
        if (!first) out += ',';
        first = false;
        out += '"';
        out += json_escape(f.name);
        out += "\":";
        out += value_to_json(f.value);
    }
    out += '}';
    return out;
}

}  // namespace logsift
