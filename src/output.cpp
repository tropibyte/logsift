// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "logsift/output.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

#include "logsift/json.hpp"
#include "logsift/text.hpp"

namespace logsift {

std::optional<OutputFormat> parse_output_format(std::string_view name) noexcept {
    if (iequals(name, "table")) return OutputFormat::Table;
    if (iequals(name, "json") || iequals(name, "jsonl") || iequals(name, "ndjson")) return OutputFormat::Json;
    if (iequals(name, "csv")) return OutputFormat::Csv;
    if (iequals(name, "logfmt")) return OutputFormat::Logfmt;
    if (iequals(name, "raw")) return OutputFormat::Raw;
    return std::nullopt;
}

namespace {

bool visible(std::string_view name, bool show_internal) {
    return show_internal || !is_internal_field(name) || name == fields::kTime;
}

// Floats are shown with 15 significant digits so that binary rounding
// noise (572.8199999999999) does not reach the screen.
std::string display_text(const Value& v) {
    if (!v.is_float() || !std::isfinite(v.as_float())) return v.to_string();
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.15g", v.as_float());
    std::string out(buf);
    for (char& c : out) {
        if (c == ',') c = '.';
    }
    return out;
}

}  // namespace

std::vector<std::string> table_columns(const std::vector<Record>& rows, bool show_internal) {
    std::vector<std::string> cols;
    std::unordered_set<std::string> seen;
    bool has_time = false;
    for (const Record& r : rows) {
        for (const auto& f : r.fields()) {
            if (f.name == fields::kTime) {
                has_time = true;
                continue;
            }
            if (!visible(f.name, show_internal)) continue;
            if (seen.insert(f.name).second) cols.push_back(f.name);
        }
    }
    if (has_time) cols.insert(cols.begin(), std::string(fields::kTime));
    return cols;
}

std::string format_table(const std::vector<Record>& rows, const OutputOptions& options) {
    if (rows.empty()) return "(no results)\n";
    const std::vector<std::string> cols = table_columns(rows, options.show_internal);
    if (cols.empty()) return "(no fields)\n";

    std::vector<std::vector<std::string>> cells(rows.size(), std::vector<std::string>(cols.size()));
    std::vector<std::vector<bool>> numeric(rows.size(), std::vector<bool>(cols.size(), false));
    std::vector<std::size_t> width(cols.size());
    for (std::size_t c = 0; c < cols.size(); ++c) width[c] = display_width(cols[c]);
    for (std::size_t r = 0; r < rows.size(); ++r) {
        for (std::size_t c = 0; c < cols.size(); ++c) {
            const Value* v = rows[r].find(cols[c]);
            std::string text = v ? display_text(*v) : "";
            for (char& ch : text) {
                if (ch == '\n' || ch == '\t' || ch == '\r') ch = ' ';
            }
            cells[r][c] = truncate_display(text, options.max_column_width);
            numeric[r][c] = v && v->is_number();
            width[c] = std::max(width[c], display_width(cells[r][c]));
        }
    }
    auto pad = [](const std::string& s, std::size_t w, bool right) {
        const std::string fill(w - std::min(w, display_width(s)), ' ');
        return right ? fill + s : s + fill;
    };
    std::string out;
    for (std::size_t c = 0; c < cols.size(); ++c) {
        if (c) out += "  ";
        out += pad(cols[c], width[c], false);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    out += '\n';
    for (std::size_t c = 0; c < cols.size(); ++c) {
        if (c) out += "  ";
        out += std::string(width[c], '-');
    }
    out += '\n';
    for (std::size_t r = 0; r < rows.size(); ++r) {
        std::string line;
        for (std::size_t c = 0; c < cols.size(); ++c) {
            if (c) line += "  ";
            line += pad(cells[r][c], width[c], numeric[r][c]);
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line + '\n';
    }
    return out;
}

std::string to_logfmt(const Record& r, bool include_internal) {
    std::string out;
    for (const auto& f : r.fields()) {
        if (!visible(f.name, include_internal)) continue;
        if (!out.empty()) out += ' ';
        out += f.name;
        out += '=';
        const std::string text = f.value.to_string();
        const bool quote = text.empty() || text.find_first_of(" \t\"=\n\r\\") != std::string::npos;
        if (!quote) {
            out += text;
            continue;
        }
        out += '"';
        for (char c : text) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: out += c;
            }
        }
        out += '"';
    }
    return out;
}

std::string csv_field(std::string_view text) {
    if (text.find_first_of(",\"\n\r") == std::string_view::npos) return std::string(text);
    std::string out = "\"";
    for (char c : text) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + "\"";
}

std::vector<Record> field_summary(const std::vector<Record>& records, bool show_internal) {
    struct Summary {
        std::string name;
        std::int64_t count = 0;
        std::array<std::int64_t, 6> types{};
        Value example;
    };
    std::vector<Summary> summaries;
    std::unordered_map<std::string, std::size_t> index;
    for (const Record& r : records) {
        for (const auto& f : r.fields()) {
            if (!visible(f.name, show_internal)) continue;
            auto [it, inserted] = index.emplace(f.name, summaries.size());
            if (inserted) summaries.push_back(Summary{f.name, 0, {}, f.value});
            Summary& s = summaries[it->second];
            ++s.count;
            ++s.types[static_cast<std::size_t>(f.value.type())];
        }
    }
    std::vector<Record> out;
    for (const Summary& s : summaries) {
        std::string types;
        for (std::size_t t = 0; t < s.types.size(); ++t) {
            if (s.types[t] == 0) continue;
            if (!types.empty()) types += ", ";
            types += std::string(type_name(static_cast<Type>(t))) + " " + std::to_string(s.types[t]);
        }
        Record r;
        r.set("field", Value::string(s.name));
        r.set("count", Value::integer(s.count));
        r.set("types", Value::string(types));
        r.set("example", Value::string(s.example.to_string()));
        out.push_back(std::move(r));
    }
    return out;
}

namespace {

class TableWriter final : public RecordWriter {
public:
    TableWriter(std::ostream& out, OutputOptions options) : out_(out), options_(options) {}
    void write(const Record& r) override { rows_.push_back(r); }
    void finish() override {
        out_ << format_table(rows_, options_);
        rows_.clear();
    }

private:
    std::ostream& out_;
    OutputOptions options_;
    std::vector<Record> rows_;
};

class CsvWriter final : public RecordWriter {
public:
    CsvWriter(std::ostream& out, OutputOptions options) : out_(out), options_(options) {}
    void write(const Record& r) override { rows_.push_back(r); }
    void finish() override {
        const auto cols = table_columns(rows_, options_.show_internal);
        for (std::size_t c = 0; c < cols.size(); ++c) out_ << (c ? "," : "") << csv_field(cols[c]);
        if (!cols.empty()) out_ << '\n';
        for (const Record& r : rows_) {
            for (std::size_t c = 0; c < cols.size(); ++c) {
                const Value* v = r.find(cols[c]);
                out_ << (c ? "," : "") << csv_field(v ? v->to_string() : "");
            }
            out_ << '\n';
        }
        rows_.clear();
    }

private:
    std::ostream& out_;
    OutputOptions options_;
    std::vector<Record> rows_;
};

class JsonWriter final : public RecordWriter {
public:
    JsonWriter(std::ostream& out, OutputOptions options) : out_(out), options_(options) {}
    void write(const Record& r) override { out_ << record_to_json(r, options_.show_internal) << '\n'; }

private:
    std::ostream& out_;
    OutputOptions options_;
};

class LogfmtWriter final : public RecordWriter {
public:
    LogfmtWriter(std::ostream& out, OutputOptions options) : out_(out), options_(options) {}
    void write(const Record& r) override { out_ << to_logfmt(r, options_.show_internal) << '\n'; }

private:
    std::ostream& out_;
    OutputOptions options_;
};

class RawWriter final : public RecordWriter {
public:
    explicit RawWriter(std::ostream& out) : out_(out) {}
    void write(const Record& r) override {
        const Value* raw = r.find(fields::kRaw);
        out_ << (raw ? raw->to_string() : to_logfmt(r)) << '\n';
    }

private:
    std::ostream& out_;
};

}  // namespace

std::unique_ptr<RecordWriter> make_writer(OutputFormat format, std::ostream& out, OutputOptions options) {
    switch (format) {
        case OutputFormat::Table: return std::make_unique<TableWriter>(out, options);
        case OutputFormat::Json: return std::make_unique<JsonWriter>(out, options);
        case OutputFormat::Csv: return std::make_unique<CsvWriter>(out, options);
        case OutputFormat::Logfmt: return std::make_unique<LogfmtWriter>(out, options);
        case OutputFormat::Raw: return std::make_unique<RawWriter>(out);
    }
    return std::make_unique<TableWriter>(out, options);
}

}  // namespace logsift
