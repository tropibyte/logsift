// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Writing query results: aligned tables, JSON Lines, CSV, logfmt or raw lines.

#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "logsift/record.hpp"

namespace logsift {

enum class OutputFormat { Table, Json, Csv, Logfmt, Raw };

std::optional<OutputFormat> parse_output_format(std::string_view name) noexcept;

struct OutputOptions {
    /// Include fields whose names start with '_' (other than _time).
    bool show_internal = false;
    /// Table cells longer than this are truncated with an ellipsis. Tables
    /// show floats with 15 significant digits; other formats keep full precision.
    std::size_t max_column_width = 60;
};

class RecordWriter {
public:
    virtual ~RecordWriter() = default;
    virtual void write(const Record& r) = 0;
    /// Flushes buffered output (tables and CSV need every row first).
    virtual void finish() {}
};

std::unique_ptr<RecordWriter> make_writer(OutputFormat format, std::ostream& out, OutputOptions options = {});

/// Column order for tabular output: _time first when present, then fields in
/// order of first appearance.
std::vector<std::string> table_columns(const std::vector<Record>& rows, bool show_internal);

std::string format_table(const std::vector<Record>& rows, const OutputOptions& options = {});

/// One logfmt line (no trailing newline). Values containing spaces, quotes,
/// '=' or nothing at all are quoted.
std::string to_logfmt(const Record& r, bool include_internal = false);

/// One CSV field, quoted when needed.
std::string csv_field(std::string_view text);

/// Summarizes the fields seen across records: one output record per field
/// (in order of first appearance) with "field", "count" (records that have
/// it), "types" (e.g. "int 40, string 2") and "example" (first value).
std::vector<Record> field_summary(const std::vector<Record>& records, bool show_internal = false);

}  // namespace logsift
