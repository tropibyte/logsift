# logsift architecture

```
bytes ─► RecordReader ─► Record ─► Pipeline (stage nodes) ─► Sink ─► RecordWriter
            │  formats/                    ▲
            │  detect_format               │ execute(): candidates from
            ▼                              │ LogStore indexes + ScanPlan
         LogStore (optional, --index) ─────┘
```

## Records and values

`Value` (`value.hpp`) is a variant of null, bool, int64, double, string and
`TimePoint` (int64 nanoseconds since the Unix epoch, UTC). Three comparison
functions serve different purposes:

- `compare_total`: a total order (null < bool < number < time < string) for
  sort, dedup and grouping.
- `compare_query` / `equals_query`: the semantics of query operators,
  including numeric comparison of numeric text and time comparison of
  timestamp text. Returns nullopt for incomparable pairs.
- `index_key`: a canonical string such that `equals_query(a, b)` implies
  `index_key(a) == index_key(b)`. Equality indexes rely on this; there is a
  test that checks the property over a set of tricky values.

`Record` (`record.hpp`) is an ordered vector of fields. Records are small,
so lookups are linear scans; field order is preserved for output.

## Input

`RecordReader` (`formats.hpp`) reads lines, buffers the first 20 to detect
the format when asked to, and turns each non-empty line into a record via
`parse_line`. Each parser lives in `src/formats/` and returns nullopt when
a line does not match, so detection can score parsers by success rate.
`assign_time` then promotes the first timestamp-like field to `_time`.

## Queries

`query_parser.cpp` contains the lexer and a recursive-descent parser that
produces a `Query`: a vector of `Stage`s, each holding expression trees
(`Expr`). Function names and argument counts are validated at parse time,
and regular expressions with literal patterns are compiled once. `top` and
`count` are expanded into ordinary stages.

`eval.cpp` evaluates expressions against a record and holds the scalar
function table. Functions receive evaluated arguments
(`std::span<const Value>`) and return null for invalid input rather than
failing the query.

## Pipeline

`Pipeline` (`pipeline.cpp`) turns stages into a linked chain of `StageNode`s
ending in the caller's `Sink`. `push()` returns false once no more input is
needed: `head` returns false after its last row, and the result travels back
up the chain so readers can stop. Buffering stages (`sort`, `tail`, `stats`)
emit in `finish()`.

`stats` keeps a `std::map` from group key (vector of values, ordered by
`compare_total`) to one `Aggregator` per aggregate. `make_aggregator`
creates them by name.

## Store and planning

`LogStore` (`store.hpp`) holds records with dense ids, a sorted
`(time, id)` index and per-field hash maps from `index_key` to ascending id
lists. Indexes are kept current on `add()`.

`plan_scan` looks only at the first stage, and only if it is a `where`. It
splits the predicate into top-level `and` terms and extracts:

- `_time` comparisons against constants (including constant expressions
  such as `ago(1h)`, detected with `is_constant`) into time bounds;
- `field == constant` and `field in (constants)` on indexed fields into
  equality sets.

`candidates` intersects the time range with the posting lists. The
pipeline, including the original filter, then runs over the candidates in id
order, so planned and unplanned execution return identical results; the
tests check this over many queries.

## Output

`output.cpp` implements the writers. Tables and CSV buffer rows to compute
columns (`_time` first, then fields in order of first appearance); JSON
Lines, logfmt and raw stream.

## Adding things

- **A scalar function**: add a `Value fn_x(Args, const EvalContext&)` in
  `eval.cpp` and a row in `kFunctions`; the parser picks up its arity.
  Return null for bad input. Test it in `tests/test_eval.cpp`.
- **An aggregate**: add a class implementing `Aggregator` in `pipeline.cpp`,
  a case in `make_aggregator`, and its name in `kAggregates` (`eval.cpp`).
- **A stage**: add a `Stage::Kind`, parse it in `Parser::parse_segment`,
  describe it in `describe()`, and add a `StageNode` in `pipeline.cpp`.
- **An input format**: write `parse_xxx` in `src/formats/`, add it to
  `Format`, `parse_line` and the detection candidates in `reader.cpp`, and
  add a sample to `examples/`.
