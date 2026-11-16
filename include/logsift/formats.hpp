// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Input formats: turning log lines into records.

#pragma once

#include <cstddef>
#include <deque>
#include <istream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "logsift/record.hpp"

namespace logsift {

enum class Format {
    Auto,    // detect from the first lines
    Json,    // one JSON object per line
    Logfmt,  // key=value key2="quoted value"
    Syslog,  // RFC 5424 or RFC 3164 (BSD)
    Clf,     // Apache/nginx Common or Combined Log Format
    Plain,   // the whole line as "message"
};

std::string_view format_name(Format f) noexcept;
std::optional<Format> parse_format_name(std::string_view name) noexcept;

struct ParseOptions {
    /// Year for timestamps that do not carry one (BSD syslog).
    int default_year = 1970;
    /// Keep the original line in _raw.
    bool keep_raw = true;
};

/// Parses one line in a specific format (not Auto). Returns nullopt when the
/// line does not match. Only the format's own fields and _time are set.
std::optional<Record> parse_line(Format format, std::string_view line, const ParseOptions& options = {});

/// Format-specific parsers (exposed for tests and embedding).
std::optional<Record> parse_logfmt(std::string_view line);
std::optional<Record> parse_syslog(std::string_view line, int default_year);
std::optional<Record> parse_clf(std::string_view line);

/// Field names checked, in order, when looking for a record's timestamp.
const std::vector<std::string_view>& timestamp_field_names();

/// Sets _time from the first timestamp-like field (see
/// timestamp_field_names) unless _time is already present. Strings are
/// parsed with parse_time(); numbers are taken as Unix epochs.
void assign_time(Record& r, const ParseOptions& options);

/// Picks the format matching most of the sample lines (at least 60% of
/// them), or Plain.
Format detect_format(const std::vector<std::string>& sample, const ParseOptions& options = {});

/// Reads records from a stream, one per non-empty line. Lines that do not
/// match the format become {message, _parse_error: true}. Every record gets
/// _line, and _raw / _file when enabled.
class RecordReader {
public:
    RecordReader(std::istream& in, Format format, ParseOptions options = {}, std::string source = {});

    std::optional<Record> next();

    /// The format in use (resolved after the first call to next() when Auto).
    Format format() const noexcept { return format_; }
    std::size_t lines_read() const noexcept { return line_no_; }
    std::size_t parse_errors() const noexcept { return parse_errors_; }

    /// Lines examined when detecting the format.
    static constexpr std::size_t kSampleLines = 20;

private:
    bool read_line(std::string& line);
    Record make_record(const std::string& line, std::size_t line_no);

    std::istream& in_;
    Format format_;
    ParseOptions options_;
    std::string source_;
    std::size_t line_no_ = 0;
    std::size_t parse_errors_ = 0;
    bool detected_ = false;
    std::deque<std::pair<std::size_t, std::string>> pending_;
};

}  // namespace logsift
