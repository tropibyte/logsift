# logsift

Query structured and semi-structured logs from the command line.

logsift reads JSON Lines, logfmt, syslog (RFC 5424 and BSD), Apache/nginx
access logs, or plain text, works out the format and the timestamps on its
own, and runs a small pipeline language over the records: filter, compute,
group, sort and summarize. It is portable C++20 with no dependencies.

```
$ logsift 'level == "error" | stats count(), p95(duration_ms) as p95_ms by service | sort -count' examples/app.jsonl
service  count  p95_ms
-------  -----  -------
billing      8  496.605
auth         5    496.7
api          4   572.82

$ logsift 'status >= 400 | top 3 path' examples/access.log
path          count
------------  -----
/admin            8
/favicon.ico      7
/api/items        1

$ logsift --year 2026 'app == "sshd" and message =~ "^Failed"
    | eval ip = extract(message, "from ([0-9.]+)") | count by ip' examples/syslog.log
```

## Features

- **Input formats**: JSON Lines (nested objects flattened to `http.status`),
  logfmt, syslog RFC 5424 (with structured data) and RFC 3164, Common and
  Combined Log Format, and plain text. Detected automatically from the first
  lines, or set with `--format`. Lines that do not parse are kept, marked
  with `_parse_error`.
- **Timestamps**: RFC 3339/ISO 8601 with offsets, CLF and BSD syslog dates,
  and Unix epochs in s/ms/µs/ns, found in `@timestamp`, `timestamp`, `time`,
  `ts` and similar fields and normalized to UTC in `_time`.
- **Query pipeline**: `where`, `eval`, `fields`, `rename`, `sort`, `head`,
  `tail`, `dedup`, `stats`, `count`, `top`, `bin`, with 40+ scalar functions
  and aggregates including `dc`, `values` and percentiles. See
  [docs/QUERY_LANGUAGE.md](docs/QUERY_LANGUAGE.md).
- **Streaming**: records flow through the pipeline as they are read; `head`
  stops reading as soon as it has its rows.
- **Indexes**: `--index FIELD` loads the input into an in-memory store with a
  time index and equality indexes, and a planner uses them for the first
  filter. `--explain` shows the plan.
- **Output**: aligned tables, JSON Lines, CSV, logfmt, or the raw lines.
- **Time windows**: `--since 2h`, `--until 2026-11-15T10:00:00Z`.
- **Schema discovery**: `--schema` lists every field with its types and counts.

## Building

Requirements: CMake 3.20+ and a C++20 compiler (GCC 11+, Clang 14+, MSVC 2022).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

On Windows: `cmake -S . -B build`, `cmake --build build --config Release`,
`ctest --test-dir build -C Release`. `CMakePresets.json` (CMake 3.21+) has
`dev`, `release` and `asan` presets.

| Option | Default | Meaning |
| --- | --- | --- |
| `LOGSIFT_BUILD_TESTS` | ON when top-level | doctest unit tests and CLI smoke tests |
| `LOGSIFT_BUILD_CLI` | ON when top-level | the `logsift` tool |
| `LOGSIFT_WARNINGS_AS_ERRORS` | OFF | `-Werror` / `/WX` |
| `LOGSIFT_SANITIZE` | OFF | AddressSanitizer + UBSan (GCC/Clang) |

## Command line

```
logsift [options] <query> [file...]
logsift --schema [options] [file...]

  -f, --format FMT    auto (default), json, logfmt, syslog, clf, plain
  -o, --output FMT    table (default), json, csv, logfmt, raw
      --since WHEN    a timestamp, or a duration meaning "ago" (15m, 2h, 7d)
      --until WHEN
  -i, --index FIELD   build an equality index (repeatable); enables planning
      --year N        year for BSD syslog timestamps (default: this year)
      --internal      show _raw, _line, _file
      --width N       maximum table column width
      --explain       print the parsed query and the scan plan
      --stats         print counts and timing to stderr
      --functions     list functions and aggregates
```

Standard input is read when no files are given. Exit status is 0 on
success, 1 for runtime errors such as unreadable files, and 2 for usage and
query errors.

## Using the library

```cpp
#include "logsift/logsift.hpp"

std::ifstream in("app.jsonl");
logsift::RecordReader reader(in, logsift::Format::Auto);
const logsift::Query q = logsift::parse_query("level == \"error\" | count by service");

logsift::Pipeline pipeline(q, [](logsift::Record&& r) {
    std::cout << logsift::to_logfmt(r) << '\n';
    return true;  // false stops the pipeline
});
while (auto r = reader.next()) {
    if (!pipeline.push(std::move(*r))) break;
}
pipeline.finish();
```

Link against `logsift::logsift`. `LogStore` + `execute()` give the indexed
path; `run_query()` is a one-call helper for vectors of records.

## Project layout

```
include/logsift/   public headers; logsift.hpp includes them all
src/               values, time, JSON, query parser, evaluator, pipeline, store, output
src/formats/       logfmt, syslog, CLF parsers, detection and the line reader
tools/             the logsift command-line tool
tests/             doctest unit tests and CLI smoke tests (via CTest)
examples/          sample logs in each format
docs/              query language reference and architecture notes
third_party/       doctest (MIT), used only by the tests
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and [ROADMAP.md](ROADMAP.md).

## License

MIT. Copyright (c) 2026 Tropibyte, Inc. See [LICENSE](LICENSE).
doctest is MIT-licensed; see `third_party/doctest/LICENSE.txt`.
