# Roadmap

Planned work. Nothing here is implemented yet.

## Input

- **Multi-line records**: stack traces and other continuation lines (lines
  starting with whitespace, or not matching the format) appended to the
  previous record's message instead of becoming parse errors.
- **Compressed input**: read `.gz` files transparently.
- **Follow mode** (`--follow`): keep reading a growing file, as `tail -f`
  does, with streaming-safe stages only.
- **CSV/TSV input** with a header row.
- **Custom formats**: a `--pattern` option with named regex groups
  (`(?<ip>\S+) - (?<user>\S+) ...`) for arbitrary line layouts.
- **Native arrays**: JSON arrays are kept as text; an array value type with
  `contains()`, indexing (`tags[0]`) and `mvexpand`-style expansion.
- **Time zones**: `--tz` for timestamps without an offset (currently UTC)
  and for display.

## Query language

- **`join`** against a second query or file by key.
- **`timechart`**: `bin` + `stats` + one column per series in a single stage,
  filling empty buckets with zeros.
- **`eventstats` / `streamstats`**: aggregates added to each record rather
  than replacing them.
- **`fillnull`**, **`mvexpand`**, **`lookup`** (enrich from a CSV file).
- **Case-insensitive operators**: `=~*` or an `(?i)` flag (std::regex has
  no inline flags), and `ilike`/`like` with `%` wildcards.
- **`sort` limits**: `sort -x | head 10` buffers everything; a top-k heap
  when a `head` follows directly.

## Performance

- **Regex engine**: `std::regex` is slow; replace with a small Thompson-NFA
  or DFA engine for the common subset, keeping std::regex as a fallback.
- **Parallel parsing** of large files by splitting on line boundaries.
- **Columnar store**: per-field column vectors instead of row records when
  `--index` loads everything, with dictionary-encoded strings.
- **On-disk index** segments so repeated queries over the same files skip
  parsing.
- **Range indexes** for numeric fields (`status >= 500`), not only equality.
- **Planner support for `or`** of indexable terms (union of candidates).

## Output

- **Colour** for levels and errors on terminals.
- **Histogram output** for `bin _time` + `count` (sparkline bars in tables).
- **Pager-friendly wide tables**: wrap or elide middle columns to fit the
  terminal width.
