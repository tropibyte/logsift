# logsift query language

A query is a pipeline of stages separated by `|`. Records flow left to
right; each stage filters, reshapes or summarizes them.

```
level == "error" and http.status >= 500
  | eval slow = duration_ms > 250
  | stats count(), p95(duration_ms) as p95_ms by service
  | sort -count
  | head 5
```

An empty query or `*` returns every record. A segment that does not start
with a stage keyword is a `where` filter, so the first stage usually needs
no keyword.

## Values and fields

Every field holds one of: `null`, a boolean, a 64-bit integer, a float, a
string or a time (UTC, nanosecond precision). A field that is missing reads
as `null`.

| Syntax | Meaning |
| --- | --- |
| `status`, `http.status`, `@timestamp` | field names: letters, digits, `_`, `.`, `@` |
| `` `user-agent` `` | any other field name, in backticks |
| `42`, `-7`, `2.5`, `1e3` | numbers |
| `"text"`, `'text'` | strings, with `\"` `\\` `\n` `\t` escapes |
| `true`, `false`, `null` | literals |
| `250ms`, `5m`, `1h30m`, `2d` | durations, as integer nanoseconds (units ns us ms s m h d w) |

### Special fields

| Field | Contents |
| --- | --- |
| `_time` | the record's timestamp, found by the input parser |
| `_raw` | the original line |
| `_line` | line number within its file |
| `_file` | file name (absent for standard input) |
| `_parse_error` | `true` when the line did not match the input format |

Fields starting with `_` other than `_time` are hidden in output unless
`--internal` is given.

## Operators

From lowest to highest precedence:

| Operators | Notes |
| --- | --- |
| `or`, `\|\|` | |
| `and`, `&&` | |
| `not`, `!` | |
| `==` (or `=`), `!=`, `<`, `<=`, `>`, `>=`, `=~`, `!~`, `in (...)`, `not in (...)` | comparisons |
| `+`, `-` | |
| `*`, `/`, `%` | |
| unary `-` | |

Comparison rules:

- If either side is a number, the other side is compared numerically when it
  is a number or numeric text (`"500" == 500` is true).
- Two strings compare byte-wise (`"10" < "9"`).
- Times compare with times and with timestamp text
  (`_time > "2026-11-15T09:00:00Z"`).
- `null == null` is true; any other comparison involving null is false
  (`!=` is the negation of `==`).
- Values that cannot be compared (a word and a number) make `<`, `>`, `==`
  false and `!=` true.
- `=~` and `!~` use ECMAScript regular expressions and search anywhere in the
  text (anchor with `^`/`$`). A missing field matches neither.

Arithmetic: integers stay integers (widening to float on overflow), `/`
always produces a float, division by zero is null. `time - time` gives a
duration in nanoseconds; `time + duration` gives a time. Anything else that
does not make sense yields null.

## Stages

| Stage | Example | Effect |
| --- | --- | --- |
| `where EXPR` | `where status >= 500` | keep records where EXPR is truthy |
| `eval NAME = EXPR, ...` | `eval kb = bytes / 1024` | add or replace fields; later assignments see earlier ones |
| `fields A, B, prefix.*` | `fields _time, http.*` | keep only these fields, in this order |
| `fields - A, ...` | `fields - _raw, tags` | drop fields |
| `rename A as B, ...` | `rename msg as message` | rename fields |
| `sort [-]A, B desc, ...` | `sort -count, path` | stable sort; `-` or `desc` for descending; nulls last |
| `head [N]` | `head 20` | first N records (default 10); stops reading input early |
| `tail [N]` | `tail 5` | last N records |
| `dedup A, ...` | `dedup user_id` | first record for each distinct combination |
| `stats AGG [as NAME], ... [by A, ...]` | `stats avg(ms) as avg_ms by service` | one record per group |
| `count [by A, ...]` | `count by level` | `stats count() by ...` |
| `top [N] A, ...` | `top 5 path` | most frequent values: `stats count() by ... \| sort -count \| head N` |
| `bin FIELD span=S [as NAME]` | `bin _time span=5m` | round times (duration span) or numbers (numeric span) down |

`stats` output is sorted by the group-by values. Records missing a group-by
field form their own group, in which that field is absent.

### Aggregates

| Aggregate | Result |
| --- | --- |
| `count()` | number of records (named `count`) |
| `count(x)` | records where x is not null |
| `sum(x)`, `avg(x)` | over numeric values (numeric text counts) |
| `min(x)`, `max(x)` | smallest/largest non-null value in the total order below |
| `dc(x)` | number of distinct values (`5` and `"5"` count once) |
| `values(x)` | distinct values, sorted, joined with ", " (first 100) |
| `first(x)`, `last(x)` | first/last non-null value in input order |
| `median(x)`, `p50`, `p90`, `p95`, `p99`, `percentile(x, Q)` | linear interpolation between ranks |
| `stdev(x)` | sample standard deviation |
| `range(x)` | max − min of numeric values |

Aggregate arguments can be expressions: `sum(bytes / 1024)`.

The total order used by `sort`, `min`, `max`, `dedup` and grouping is:
null < booleans < numbers < times < strings.

## Functions

| Function | Result |
| --- | --- |
| `lower(s)`, `upper(s)`, `trim(s)`, `len(s)` | text basics (ASCII case mapping; length in code points) |
| `contains(s, t)`, `startswith(s, t)`, `endswith(s, t)` | booleans |
| `substr(s, start[, n])` | bytes, 0-based |
| `replace(s, from, to)`, `concat(a, ...)`, `split_part(s, sep, n)` | |
| `match(s, re)`, `extract(s, re[, group])` | regex test / capture group |
| `tonumber(x)`, `toint(x)`, `tostring(x)`, `totime(x)`, `typeof(x)` | conversions |
| `isnull(x)`, `isnotnull(x)`, `exists(x)`, `coalesce(a, ...)`, `if(c, a[, b])` | null handling |
| `abs`, `floor`, `ceil`, `round(x[, digits])`, `sqrt`, `least(...)`, `greatest(...)` | math |
| `now()`, `ago(d)`, `duration("1h")`, `format_duration(ns)` | time and durations |
| `year`, `month`, `day`, `hour`, `minute`, `second`, `weekday`, `date`, `bucket(t, span)` | time parts (UTC) |

`now()` is fixed when the query starts. `logsift --functions` prints the list.

## Indexes

By default logsift streams records through the pipeline. With
`--index FIELD` it loads all input, builds a time index and an equality
index per listed field, and plans the first filter: top-level `and` terms of
the form `_time <op> constant`, `field == constant` and
`field in (constants)` on indexed fields narrow the records examined. The
filter still runs on every candidate, so results are the same either way;
`--explain` shows the plan and `--stats` the number of records examined.
