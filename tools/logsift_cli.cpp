// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// logsift command-line tool.

#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "logsift/logsift.hpp"

using namespace logsift;

namespace {

const char* const kUsage = R"USAGE(logsift - query structured and semi-structured logs

Usage:
  logsift [options] <query> [file...]
  logsift --schema [options] [file...]

Reads standard input when no files are given ("-" also means stdin).

Query:  a pipeline of stages separated by "|", for example
  logsift 'level == "error" | stats count() by service | sort -count' app.jsonl
  logsift 'status >= 500 | top 5 path' access.log
  logsift '*' --since 15m -o json app.jsonl
Stages: where, eval, fields, rename, sort, head, tail, dedup, stats, count,
        top, bin. A segment without a stage keyword is a where filter.

Options:
  -f, --format FMT    input format: auto (default), json, logfmt, syslog, clf, plain
  -o, --output FMT    output: table (default), json, csv, logfmt, raw
      --since WHEN    records at or after WHEN: a timestamp or a duration ago (15m, 2h, 7d)
      --until WHEN    records before WHEN
  -i, --index FIELD   load everything and build an equality index on FIELD (repeatable)
      --year N        year for timestamps that lack one (BSD syslog); default: this year
      --internal      also show _raw, _line, _file and other internal fields
      --width N       maximum table column width (default 60)
      --schema        list the fields found, with types and counts
      --explain       print the parsed query and scan plan, then exit
      --stats         print record counts and timing to stderr
      --functions     list scalar functions and aggregates
  -h, --help          show this help
      --version       show the version
)USAGE";

struct Options {
    Format format = Format::Auto;
    OutputFormat output = OutputFormat::Table;
    std::optional<std::string> since;
    std::optional<std::string> until;
    std::vector<std::string> indexes;
    std::optional<int> year;
    bool internal = false;
    std::size_t width = 60;
    bool schema = false;
    bool explain = false;
    bool stats = false;
    std::optional<std::string> query;
    std::vector<std::string> files;
};

class UsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

Options parse_args(int argc, char** argv) {
    Options o;
    std::vector<std::string> positional;
    auto value = [&](int& i, const std::string& flag) -> std::string {
        if (i + 1 >= argc) throw UsageError(flag + " needs a value");
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-f" || a == "--format") {
            const std::string v = value(i, a);
            auto f = parse_format_name(v);
            if (!f) throw UsageError("unknown input format '" + v + "'");
            o.format = *f;
        } else if (a == "-o" || a == "--output") {
            const std::string v = value(i, a);
            auto f = parse_output_format(v);
            if (!f) throw UsageError("unknown output format '" + v + "'");
            o.output = *f;
        } else if (a == "--since") {
            o.since = value(i, a);
        } else if (a == "--until") {
            o.until = value(i, a);
        } else if (a == "-i" || a == "--index") {
            o.indexes.push_back(value(i, a));
        } else if (a == "--year") {
            const std::string v = value(i, a);
            auto y = parse_int(v);
            if (!y || *y < 1678 || *y > 2261) throw UsageError("invalid --year '" + v + "'");
            o.year = static_cast<int>(*y);
        } else if (a == "--internal") {
            o.internal = true;
        } else if (a == "--width") {
            const std::string v = value(i, a);
            auto w = parse_int(v);
            if (!w || *w < 4) throw UsageError("invalid --width '" + v + "'");
            o.width = static_cast<std::size_t>(*w);
        } else if (a == "--schema") {
            o.schema = true;
        } else if (a == "--explain") {
            o.explain = true;
        } else if (a == "--stats") {
            o.stats = true;
        } else if (a == "--") {
            for (++i; i < argc; ++i) positional.emplace_back(argv[i]);
        } else if (a.size() > 1 && a[0] == '-' && a != "-") {
            throw UsageError("unknown option '" + a + "'");
        } else {
            positional.push_back(a);
        }
    }
    std::size_t first_file = 0;
    if (!o.schema) {
        if (positional.empty()) throw UsageError("missing query (use '*' for everything)");
        o.query = positional[0];
        first_file = 1;
    }
    o.files.assign(positional.begin() + static_cast<std::ptrdiff_t>(first_file), positional.end());
    return o;
}

TimePoint now() {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    return TimePoint{static_cast<std::int64_t>(ns)};
}

// A timestamp, or a duration meaning "that long ago".
ScanPlan::Bound parse_when(const std::string& text, TimePoint reference, bool inclusive, int year) {
    if (auto t = parse_time(text, year)) return {*t, inclusive};
    if (auto d = parse_duration(text)) return {TimePoint{reference.ns - *d}, inclusive};
    throw UsageError("cannot parse time '" + text + "' (use RFC 3339 or a duration such as 15m)");
}

// Calls fn(record) for every record of every input; stops when fn returns false.
struct InputStats {
    std::size_t lines = 0;
    std::size_t records = 0;
    std::size_t parse_errors = 0;
    std::vector<std::string> formats;
};

template <class Fn>
void read_inputs(const Options& o, const ParseOptions& parse, InputStats& stats, Fn&& fn) {
    std::vector<std::string> files = o.files;
    if (files.empty()) files.emplace_back("-");
    for (const std::string& path : files) {
        std::ifstream file;
        std::istream* in = &std::cin;
        if (path != "-") {
            file.open(path, std::ios::binary);
            if (!file) throw std::runtime_error("cannot open '" + path + "'");
            in = &file;
        }
        RecordReader reader(*in, o.format, parse, path == "-" ? std::string() : path);
        bool more = true;
        while (more) {
            auto r = reader.next();
            if (!r) break;
            ++stats.records;
            more = fn(std::move(*r));
        }
        stats.lines += reader.lines_read();
        stats.parse_errors += reader.parse_errors();
        stats.formats.push_back((path == "-" ? std::string("stdin") : path) + ": " +
                                std::string(format_name(reader.format())));
        if (!more) break;
    }
}

int run(const Options& o) {
    const auto started = std::chrono::steady_clock::now();
    const TimePoint reference = now();
    ParseOptions parse;
    parse.default_year = o.year ? *o.year : to_civil(reference).year;
    OutputOptions out_opts;
    out_opts.show_internal = o.internal;
    out_opts.max_column_width = o.width;
    InputStats input;

    if (o.schema) {
        std::vector<Record> all;
        read_inputs(o, parse, input, [&](Record&& r) {
            all.push_back(std::move(r));
            return true;
        });
        auto writer = make_writer(o.output, std::cout, OutputOptions{false, o.width});
        for (const Record& r : field_summary(all, o.internal)) writer->write(r);
        writer->finish();
        return 0;
    }

    const Query query = parse_query(*o.query);
    EvalContext ctx;
    ctx.now = reference;
    std::optional<ScanPlan::Bound> from;
    std::optional<ScanPlan::Bound> to;
    if (o.since) from = parse_when(*o.since, reference, true, parse.default_year);
    if (o.until) to = parse_when(*o.until, reference, false, parse.default_year);

    auto writer = make_writer(o.output, std::cout, out_opts);
    std::size_t emitted = 0;
    std::size_t examined = 0;
    std::string plan_text;

    if (!o.indexes.empty()) {
        // Indexed mode: load everything, then plan.
        LogStore store;
        read_inputs(o, parse, input, [&](Record&& r) {
            store.add(std::move(r));
            return true;
        });
        store.build_time_index();
        for (const auto& field : o.indexes) store.build_field_index(field);
        if (o.explain) {
            std::cout << describe(query) << "plan: " << plan_scan(query, store, ctx).describe() << '\n';
            return 0;
        }
        const ExecStats stats = execute(query, store, [&](Record&& r) {
            writer->write(r);
            return true;
        }, ctx, from, to);
        emitted = stats.emitted;
        examined = stats.examined;
        plan_text = stats.plan.describe();
    } else {
        // Streaming mode: records flow straight through the pipeline.
        if (o.explain) {
            std::cout << describe(query) << "plan: streaming scan (pass --index to build indexes)\n";
            return 0;
        }
        Pipeline pipeline(query, [&](Record&& r) {
            ++emitted;
            writer->write(r);
            return true;
        }, ctx);
        read_inputs(o, parse, input, [&](Record&& r) {
            if (from || to) {
                const auto t = r.time();
                if (!t) return true;
                if (from && *t < from->time) return true;
                if (to && *t >= to->time) return true;
            }
            ++examined;
            return pipeline.push(std::move(r));
        });
        pipeline.finish();
        plan_text = "streaming scan";
    }
    writer->finish();

    if (o.stats) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        std::cerr << "inputs:       ";
        for (std::size_t i = 0; i < input.formats.size(); ++i) std::cerr << (i ? ", " : "") << input.formats[i];
        std::cerr << "\nrecords:      " << input.records << " (" << input.parse_errors << " unparsed)"
                  << "\nexamined:     " << examined << "\nemitted:      " << emitted << "\nplan:         "
                  << plan_text << "\nelapsed:      " << ms.count() << " ms\n";
    }
    return 0;
}

void list_functions() {
    std::cout << "Scalar functions:\n";
    for (const FunctionDef& f : function_table()) std::cout << "  " << f.help << '\n';
    std::cout << "\nAggregates (stats):\n  ";
    const auto names = aggregate_function_names();
    for (std::size_t i = 0; i < names.size(); ++i) std::cout << (i ? ", " : "") << names[i];
    std::cout << "\n  percentile(x, rank) takes a rank from 0 to 100.\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::ios::sync_with_stdio(false);
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            std::cout << kUsage;
            return 0;
        }
        if (a == "--version") {
            std::cout << "logsift " << kVersion << '\n';
            return 0;
        }
        if (a == "--functions") {
            list_functions();
            return 0;
        }
    }
    try {
        return run(parse_args(argc, argv));
    } catch (const UsageError& e) {
        std::cerr << "logsift: " << e.what() << "\n(see logsift --help)\n";
        return 2;
    } catch (const QueryError& e) {
        std::cerr << "logsift: query error at position " << e.offset() + 1 << ": " << e.what() << '\n';
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "logsift: " << e.what() << '\n';
        return 1;
    }
}
