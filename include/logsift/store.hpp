// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// In-memory record store with a time index and equality indexes, and the
// planner that uses them to avoid scanning every record.

#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "logsift/eval.hpp"
#include "logsift/query.hpp"
#include "logsift/record.hpp"

namespace logsift {

class LogStore {
public:
    /// Appends a record and returns its id (ids are dense, starting at 0).
    /// Existing indexes are updated.
    std::size_t add(Record r);

    std::size_t size() const noexcept { return records_.size(); }
    const Record& operator[](std::size_t id) const { return records_.at(id); }
    const std::vector<Record>& records() const noexcept { return records_; }

    /// Indexes records by _time. Kept up to date by later add() calls.
    void build_time_index();
    bool has_time_index() const noexcept { return time_indexed_; }

    /// Indexes a field for equality lookups. Kept up to date by add().
    void build_field_index(const std::string& field);
    bool has_field_index(std::string_view field) const;
    std::vector<std::string> indexed_fields() const;

    /// Ids (ascending) of records whose _time lies within the bounds.
    std::vector<std::size_t> time_range(std::optional<TimePoint> from, bool from_inclusive,
                                        std::optional<TimePoint> to, bool to_inclusive) const;

    /// Ids (ascending) of records whose indexed field may equal `v`
    /// (every record for which equals_query(field, v) holds is included).
    std::vector<std::size_t> lookup(std::string_view field, const Value& v) const;

    /// Earliest and latest _time, if any record has one.
    std::optional<std::pair<TimePoint, TimePoint>> time_span() const;

private:
    void index_time(std::size_t id);
    void index_field(const std::string& field, std::size_t id);

    std::vector<Record> records_;
    bool time_indexed_ = false;
    std::vector<std::pair<std::int64_t, std::size_t>> time_index_;  // sorted (ns, id)
    // field -> index key -> ascending ids
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::size_t>>> field_indexes_;
};

/// How a query will read the store.
struct ScanPlan {
    struct Bound {
        TimePoint time;
        bool inclusive;
    };
    std::optional<Bound> from;
    std::optional<Bound> to;
    /// Each entry restricts the scan to records whose field equals one of
    /// the values (values OR-ed, entries AND-ed).
    std::vector<std::pair<std::string, std::vector<Value>>> equalities;

    bool full_scan() const noexcept { return !from && !to && equalities.empty(); }
    std::string describe() const;
};

/// Inspects the first stage of the query: when it is a filter, top-level
/// "and" terms of the form `_time <op> constant`, `field == constant` and
/// `field in (constants)` on indexed fields narrow the scan. The filter
/// itself still runs on every candidate, so results are identical to a full
/// scan.
ScanPlan plan_scan(const Query& query, const LogStore& store, const EvalContext& ctx = {});

/// Ids (ascending) of records the plan says must be examined.
std::vector<std::size_t> candidates(const LogStore& store, const ScanPlan& plan);

struct ExecStats {
    ScanPlan plan;
    std::size_t candidates = 0;
    std::size_t examined = 0;
    std::size_t emitted = 0;
};

/// Plans and runs a query over the store, sending results to `sink`.
/// `extra_from`/`extra_to` restrict input to a time window (records without
/// a _time are then skipped), as the CLI's --since/--until do.
ExecStats execute(const Query& query, const LogStore& store, const std::function<bool(Record&&)>& sink,
                  const EvalContext& ctx = {}, std::optional<ScanPlan::Bound> extra_from = std::nullopt,
                  std::optional<ScanPlan::Bound> extra_to = std::nullopt);

}  // namespace logsift
