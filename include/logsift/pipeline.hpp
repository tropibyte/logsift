// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Executes a parsed query over a stream of records.

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include "logsift/eval.hpp"
#include "logsift/query.hpp"
#include "logsift/record.hpp"

namespace logsift {

/// Receives output records; returns false to stop the pipeline early.
using Sink = std::function<bool(Record&&)>;

/// Accumulates values for one aggregate in a stats stage.
class Aggregator {
public:
    virtual ~Aggregator() = default;
    virtual void add(const Value& v) = 0;
    virtual Value result() const = 0;
};

/// Creates the accumulator for an aggregate function (count, sum, avg, min,
/// max, dc, values, first, last, median, p50/p90/p95/p99, percentile, stdev,
/// range). Throws std::invalid_argument for unknown names.
std::unique_ptr<Aggregator> make_aggregator(const std::string& function, double parameter = 0);

namespace detail {
class StageNode;
}

/// A query compiled into a chain of stages.
///
/// Streaming stages (where, eval, fields, rename, head, dedup, bin) pass
/// records through immediately; sort, tail and stats hold records until
/// finish(). head stops the pipeline once it has its rows, which lets
/// callers stop reading input.
class Pipeline {
public:
    /// The query must outlive the pipeline.
    Pipeline(const Query& query, Sink sink, EvalContext ctx = {});
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    /// Feeds one record. Returns false when no more input is needed.
    bool push(Record r);

    /// Flushes buffering stages. Call once, after the last push().
    void finish();

    /// Records pushed so far.
    std::size_t consumed() const noexcept { return consumed_; }

private:
    EvalContext ctx_;
    std::vector<std::unique_ptr<detail::StageNode>> nodes_;
    std::size_t consumed_ = 0;
    bool open_ = true;
    bool finished_ = false;
};

/// Convenience: runs a query over records and collects the output.
std::vector<Record> run_query(const Query& query, const std::vector<Record>& input, const EvalContext& ctx = {});

}  // namespace logsift
