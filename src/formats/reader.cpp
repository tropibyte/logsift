// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Format dispatch, detection, timestamp promotion and the line reader.

#include <array>
#include <cmath>

#include "logsift/formats.hpp"
#include "logsift/json.hpp"
#include "logsift/text.hpp"

namespace logsift {

namespace {

constexpr std::array<std::pair<Format, std::string_view>, 6> kFormatNames{{
    {Format::Auto, "auto"},
    {Format::Json, "json"},
    {Format::Logfmt, "logfmt"},
    {Format::Syslog, "syslog"},
    {Format::Clf, "clf"},
    {Format::Plain, "plain"},
}};

std::size_t logfmt_pairs(const Record& r) {
    std::size_t n = 0;
    for (const auto& f : r.fields()) {
        if (!f.value.is_bool() || !f.value.as_bool()) ++n;
    }
    return n;
}

}  // namespace

std::string_view format_name(Format f) noexcept {
    for (const auto& [fmt, name] : kFormatNames) {
        if (fmt == f) return name;
    }
    return "unknown";
}

std::optional<Format> parse_format_name(std::string_view name) noexcept {
    for (const auto& [fmt, n] : kFormatNames) {
        if (iequals(n, name)) return fmt;
    }
    if (iequals(name, "jsonl") || iequals(name, "ndjson")) return Format::Json;
    if (iequals(name, "access") || iequals(name, "combined")) return Format::Clf;
    return std::nullopt;
}

const std::vector<std::string_view>& timestamp_field_names() {
    static const std::vector<std::string_view> kNames{"@timestamp", "timestamp", "time", "ts",
                                                      "datetime",   "date",      "t"};
    return kNames;
}

void assign_time(Record& r, const ParseOptions& options) {
    // An existing _time must hold a Time; convert it or drop it.
    if (const Value* existing = r.find(fields::kTime)) {
        if (existing->is_time()) return;
        std::optional<TimePoint> t;
        if (existing->is_string()) t = parse_time(existing->as_string(), options.default_year);
        if (!t && existing->is_number()) t = parse_epoch(*existing->to_number());
        if (t) {
            r.set(fields::kTime, Value::time(*t));
            return;
        }
        r.erase(fields::kTime);
    }
    for (std::string_view name : timestamp_field_names()) {
        const Value* v = r.find(name);
        if (!v) continue;
        std::optional<TimePoint> t;
        if (v->is_string()) {
            t = parse_time(v->as_string(), options.default_year);
        } else if (v->is_number()) {
            t = parse_epoch(*v->to_number());
        } else if (v->is_time()) {
            t = v->as_time();
        }
        if (t) {
            r.set(fields::kTime, Value::time(*t));
            return;
        }
    }
}

std::optional<Record> parse_line(Format format, std::string_view line, const ParseOptions& options) {
    std::optional<Record> r;
    switch (format) {
        case Format::Json: r = parse_json_object(line); break;
        case Format::Logfmt: r = parse_logfmt(line); break;
        case Format::Syslog: r = parse_syslog(line, options.default_year); break;
        case Format::Clf: r = parse_clf(line); break;
        case Format::Plain: r = Record{{"message", Value::string(std::string(line))}}; break;
        case Format::Auto: return std::nullopt;
    }
    if (r) assign_time(*r, options);
    return r;
}

Format detect_format(const std::vector<std::string>& sample, const ParseOptions& options) {
    std::vector<std::string_view> lines;
    for (const auto& s : sample) {
        if (!trim(s).empty()) lines.push_back(s);
    }
    if (lines.empty()) return Format::Plain;

    constexpr std::array<Format, 4> kCandidates{Format::Json, Format::Syslog, Format::Clf, Format::Logfmt};
    Format best = Format::Plain;
    std::size_t best_score = 0;
    for (Format f : kCandidates) {
        std::size_t score = 0;
        for (std::string_view line : lines) {
            auto r = parse_line(f, line, options);
            // A single key=value pair is too weak a signal for logfmt.
            if (r && (f != Format::Logfmt || logfmt_pairs(*r) >= 2)) ++score;
        }
        if (score > best_score) {
            best = f;
            best_score = score;
        }
    }
    // Require a clear majority.
    if (best_score * 10 < lines.size() * 6) return Format::Plain;
    return best;
}

RecordReader::RecordReader(std::istream& in, Format format, ParseOptions options, std::string source)
    : in_(in), format_(format), options_(options), source_(std::move(source)) {}

bool RecordReader::read_line(std::string& line) {
    if (!std::getline(in_, line)) return false;
    ++line_no_;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
}

Record RecordReader::make_record(const std::string& line, std::size_t line_no) {
    std::optional<Record> parsed = parse_line(format_, line, options_);
    Record r;
    if (parsed) {
        r = std::move(*parsed);
    } else {
        ++parse_errors_;
        r.set("message", Value::string(line));
        r.set(fields::kParseError, Value::boolean(true));
    }
    r.set(fields::kLine, Value::integer(static_cast<std::int64_t>(line_no)));
    if (!source_.empty()) r.set(fields::kFile, Value::string(source_));
    if (options_.keep_raw) r.set(fields::kRaw, Value::string(line));
    return r;
}

std::optional<Record> RecordReader::next() {
    if (!detected_) {
        detected_ = true;
        if (format_ == Format::Auto) {
            std::vector<std::string> sample;
            std::string line;
            while (sample.size() < kSampleLines && read_line(line)) {
                pending_.emplace_back(line_no_, line);
                if (!trim(line).empty()) sample.push_back(line);
            }
            format_ = detect_format(sample, options_);
        }
    }
    for (;;) {
        std::string line;
        std::size_t number = 0;
        if (!pending_.empty()) {
            number = pending_.front().first;
            line = std::move(pending_.front().second);
            pending_.pop_front();
        } else {
            if (!read_line(line)) return std::nullopt;
            number = line_no_;
        }
        if (trim(line).empty()) continue;
        return make_record(line, number);
    }
}

}  // namespace logsift
